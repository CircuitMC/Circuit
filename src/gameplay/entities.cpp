// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <limits>
#include <tuple>
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/physics.hpp>
#include <circuit/gameplay/survival.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <tuple>

// Item entities, as both official servers run them (checked against their
// traces of falling, sliding, resting and merging items; minecraft.wiki
// "Item (entity)" and "Entity"). Each tick:
// - the tick count goes up, and the pickup delay down;
// - currents push it (also after the move), and in water it floats up
//   instead of falling; otherwise gravity takes 0.04 from the vertical
//   velocity;
// - the item moves, unless it rests on the ground with a horizontal velocity
//   whose square is at most 1e-5, in which case it moves only when its tick
//   count plus its id is a multiple of 4; an item that moves inside a hopper
//   may be taken by it;
// - after a move the velocity is damped: horizontally by 0.98 (the block's
//   slipperiness times 0.98 on the ground), both in single precision, and
//   vertically by 0.98; an item on the ground bounces back up at half its
//   downward speed;
// - every 40 ticks, and every 2 ticks when the move took it into another
//   block, it merges with items next to it;
// - its age goes up; at 6000 the item despawns.
namespace circuit::gameplay {
namespace {
using contracts::Vec3;
constexpr double gravity = 0.04;
constexpr double item_size = 0.25;

// A full cube: what entities collide with in unloaded chunks (they do not
// enter them). Above and below the world there is nothing.
constexpr registry::Box full_cube{0.0, 0.0, 0.0, 1.0, 1.0, 1.0};

// A moving block collides where it stands in its move.
physics::ShapeAt shapes(RuleContext& ctx) {
    auto moving = std::make_shared<std::deque<std::vector<registry::Box>>>();
    return [&ctx, moving](contracts::BlockPos p) -> std::span<const registry::Box> {
        if (!ctx.world().contains_y(p.y)) return {};
        const auto s = ctx.block(p);
        if (s.is_unloaded()) return {&full_cube, 1};
        if (s.is(BlockType::moving_piston)) {
            moving->push_back(detail::moving_block_boxes(ctx, p));
            return moving->back();
        }
        return physics::collision_boxes(s, ctx.bundle().schema.content_release);
    };
}

} // namespace

// minecraft.wiki "Slipperiness": 0.6 but for ice (0.98), blue ice (0.989)
// and slime blocks (0.8).
float detail::slipperiness(BlockState s) {
    if (s.is(BlockType::slime_block)) return 0.8f;
    if (!s.is(BlockType::other)) return 0.6f;
    const auto block = registry::block_of(s.registry_state());
    if (!block) return 0.6f;
    const auto name = registry::block(*block).name;
    if (name == "ice" || name == "packed_ice" || name == "frosted_ice") return 0.98f;
    if (name == "blue_ice") return 0.989f;
    return 0.6f;
}

namespace {
using detail::slipperiness;

// ---- water ---------------------------------------------------------------
//
// Fitted to both official servers' traces of items in still water, in
// channels of flowing water, on ice under flowing water, around a corner
// and in falling water, every value exact. minecraft.wiki "Water": a cell
// holds level/9 of water (a source or falling water 8/9, a full block under
// more water), and its current is the sum of the flows to and from its four
// neighbors; falling water flows down.

// The water a cell holds, in ninths of a block, ignoring water above.
float own_height(BlockState s) {
    if (!s.is(BlockType::water)) return 0.0f;
    const int amount = s.flag() || s.level() == 0 ? 8 : 8 - s.level();
    return static_cast<float>(amount) / 9.0f;
}

Vec3 normalized(Vec3 v) {
    const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length < 1.0e-4 ? Vec3{} : Vec3{v.x / length, v.y / length, v.z / length};
}

// The direction of a water cell's current: toward neighbors with less water,
// and toward water below an open neighbor; falling water beside a whole face
// also flows down.
Vec3 current(RuleContext& ctx, BlockPos p, BlockState water) {
    const double own = own_height(water);
    double dx = 0.0, dz = 0.0;
    for (const auto dir : {Direction::north, Direction::south, Direction::west, Direction::east}) {
        const auto n = p.relative(dir);
        const auto there = ctx.block(n);
        if (there.is_unloaded() || !detail::holds_water(there)) continue;
        const double g = own_height(there);
        double h = 0.0;
        if (g == 0.0) {
            const auto below = ctx.block(n.relative(Direction::down));
            if (!below.is_unloaded() && detail::holds_water(below)) {
                const double g2 = own_height(below);
                if (g2 > 0.0) h = own - (g2 - static_cast<double>(8.0f / 9.0f));
            }
        } else {
            h = own - g;
        }
        if (h != 0.0) {
            const auto o = contracts::offset(dir);
            dx += o.dx * h;
            dz += o.dz * h;
        }
    }
    Vec3 flow{dx, 0.0, dz};
    if (water.flag()) {
        for (const auto dir : {Direction::north, Direction::south, Direction::west, Direction::east}) {
            const auto n = p.relative(dir);
            const auto side = contracts::opposite(dir);
            if (detail::face_sturdy(ctx.block(n), side) || detail::face_sturdy(ctx.block(n.relative(Direction::up)), side)) {
                const auto across = normalized(flow);
                flow = {across.x, -6.0, across.z};
                break;
            }
        }
    }
    return normalized(flow);
}

} // namespace

namespace detail {
InWater water_at(RuleContext& ctx, const physics::Aabb& entity) {
    const auto box = entity.deflate(0.001);
    const auto cell = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    InWater out;
    Vec3 sum{};
    int count = 0;
    for (auto x = cell(box.min_x); x <= cell(box.max_x); ++x)
        for (auto y = cell(box.min_y); y <= cell(box.max_y); ++y)
            for (auto z = cell(box.min_z); z <= cell(box.max_z); ++z) {
                const contracts::BlockPos p{x, y, z};
                if (!ctx.world().contains_y(p.y)) continue;
                const auto s = ctx.block(p);
                if (!s.is(BlockType::water)) continue;
                // The surface's height in single precision, as the target versions add it.
                const double top = static_cast<double>(static_cast<float>(y) + fluid_height(s, ctx.block(p.relative(Direction::up))));
                if (top < box.min_y) continue;
                out.inside = true;
                out.depth = std::max(out.depth, top - box.min_y);
                auto flow = current(ctx, p, s);
                if (out.depth < 0.4) flow = {flow.x * out.depth, flow.y * out.depth, flow.z * out.depth};
                sum = {sum.x + flow.x, sum.y + flow.y, sum.z + flow.z};
                ++count;
            }
    if (std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z) > 0.0) {
        sum = {sum.x / count, sum.y / count, sum.z / count};
        const double length = std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z);
        out.push = {sum.x / length * 0.014, sum.y / length * 0.014, sum.z / length * 0.014};
    }
    return out;
}

namespace {
bool is_soul_sand(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto block = registry::block_of(s.registry_state());
    return block && registry::block(*block).name == "soul_sand";
}
float speed_factor(BlockState s) { return s.is(BlockType::honey_block) || is_soul_sand(s) ? 0.4f : 1.0f; }
} // namespace

bool move_entity(RuleContext& ctx, Vec3& position, Vec3& velocity, double width, double height, const Entity* self, Vec3* moved) {
    return move_entity(ctx, position, velocity, velocity, width, height, self, moved);
}

bool move_entity(RuleContext& ctx, Vec3& position, Vec3& velocity, Vec3 movement, double width, double height, const Entity* self,
                 Vec3* moved) {
    const bool landed = move_entity_by(ctx, position, velocity, movement, width, height, self, moved);
    // Honey and soul sand slow what moves in or on them: the block at its
    // feet, or the one half a block below (items and primed TNT observed).
    const auto at = [&](double dy) {
        const auto cell = contracts::block_of(Vec3{position.x, position.y + dy, position.z});
        return ctx.world().contains_y(cell.y) ? ctx.block(cell) : blocks::air();
    };
    const auto here = at(0.0);
    float f = here.is(BlockType::water) ? 1.0f : speed_factor(here);
    if (f == 1.0f && !here.is(BlockType::water)) f = speed_factor(at(-0.500001));
    velocity = {velocity.x * static_cast<double>(f), velocity.y, velocity.z * static_cast<double>(f)};
    return landed;
}

std::optional<BlockPos> supporting_block(RuleContext& ctx, const physics::Aabb& box, Vec3 position, Vec3 moved) {
    // The nearest to the entity's position (ties: the lowest, then by z, then x).
    const auto find = [&](const physics::Aabb& under) -> std::optional<BlockPos> {
        std::optional<BlockPos> best;
        double nearest = std::numeric_limits<double>::max();
        for (const auto& c : physics::colliders(under, shapes(ctx))) {
            const double dx = c.pos.x + 0.5 - position.x, dy = c.pos.y + 0.5 - position.y, dz = c.pos.z + 0.5 - position.z;
            const double d = dx * dx + dy * dy + dz * dz;
            const auto key = [](BlockPos p) { return std::tuple{p.y, p.z, p.x}; };
            if (d < nearest || (d == nearest && key(c.pos) < key(*best))) {
                best = c.pos;
                nearest = d;
            }
        }
        return best;
    };
    const physics::Aabb under{box.min_x, box.min_y - 1.0e-6, box.min_z, box.max_x, box.min_y, box.max_z};
    if (auto found = find(under)) return found;
    if (moved.x == 0.0 && moved.z == 0.0) return std::nullopt;
    return find(under.move(-moved.x, 0.0, -moved.z));
}

bool honey_slide(RuleContext& ctx, Vec3 position, Vec3& velocity, double width, double height, bool on_ground) {
    if (on_ground) return false;
    constexpr double drag = static_cast<double>(0.98f);
    const double w = velocity.y / drag + 0.08;
    if (w >= -0.08) return false;
    const auto box = physics::box_at(position, width, height);
    const auto cell = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    const double reach = 0.4375 + width / 2.0;
    for (auto z = cell(box.min_z); z <= cell(box.max_z); ++z)
        for (auto y = cell(box.min_y); y <= cell(box.max_y); ++y)
            for (auto x = cell(box.min_x); x <= cell(box.max_x); ++x) {
                if (!ctx.world().contains_y(y) || !ctx.block({x, y, z}).is(BlockType::honey_block)) continue;
                if (position.y > y + 0.9375 - 1.0e-7) continue;
                if (std::abs(x + 0.5 - position.x) + 1.0e-7 <= reach && std::abs(z + 0.5 - position.z) + 1.0e-7 <= reach) continue;
                if (w < -0.13) {
                    const double d = -0.05 / w;
                    velocity.x *= d;
                    velocity.z *= d;
                }
                velocity.y = (-0.05 - 0.08) * drag;
                return true;
            }
    return false;
}

bool vehicle_collides(const Entity& mover, const Entity& other) noexcept {
    if (other.id == mover.id) return false;
    const bool vehicle = mover.kind == EntityKind::boat || mover.kind == EntityKind::minecart;
    if (vehicle) return other.kind == EntityKind::boat || other.kind == EntityKind::minecart;
    const bool solid_mover = mover.kind == EntityKind::item || mover.kind == EntityKind::primed_tnt || mover.kind == EntityKind::falling_block;
    return solid_mover && other.kind == EntityKind::boat;
}

bool move_entity_by(RuleContext& ctx, Vec3& position, Vec3& velocity, Vec3 movement, double width, double height, const Entity* self,
                    Vec3* moved) {
    const auto box = physics::box_at(position, width, height);
    Vec3 done = movement;
    if (movement.x != 0.0 || movement.y != 0.0 || movement.z != 0.0) {
        const auto area = box.expand_towards(movement.x, movement.y, movement.z);
        auto boxes = physics::flatten(physics::colliders(area, shapes(ctx)));
        // The boxes of the entities it cannot pass come first, as on both official servers.
        if (self) {
            std::vector<physics::Aabb> entities;
            for (const auto id : entities_intersecting(ctx, area.inflate(1.0e-7, 1.0e-7, 1.0e-7)))
                if (const auto* other = ctx.peek_entity(id); other && vehicle_collides(*self, *other)) entities.push_back(entity_box(*other));
            boxes.insert(boxes.begin(), entities.begin(), entities.end());
        }
        done = physics::collide_with(movement, box, boxes);
    }
    const double made = done.x * done.x + done.y * done.y + done.z * done.z;
    const double wanted = movement.x * movement.x + movement.y * movement.y + movement.z * movement.z;
    if (made > 1.0e-7 || wanted - made < 1.0e-7) position = {position.x + done.x, position.y + done.y, position.z + done.z};
    if (moved) *moved = done;
    const bool landed = done.y != movement.y && movement.y < 0.0;
    // A horizontal component stops only when the collision shortened it by
    // 1e-5 or more (both official servers keep primed TNT's last 5e-8 of
    // speed while a shorter move is dropped).
    constexpr double horizontal = 1.0e-5f;
    if (std::abs(done.x - movement.x) >= horizontal) velocity.x = 0.0;
    if (std::abs(done.z - movement.z) >= horizontal) velocity.z = 0.0;
    if (done.y != movement.y) {
        // A slime block under the entity sends it back up at 80% of the
        // speed it landed with (minecraft.wiki "Slime Block"; observed for
        // items); anything else stops it.
        const auto under = contracts::block_of(Vec3{position.x, position.y - 0.2, position.z});
        const bool slime = landed && ctx.world().contains_y(under.y) && ctx.block(under).is(BlockType::slime_block);
        velocity.y = slime && velocity.y < 0.0 ? -velocity.y * 0.8 : 0.0;
        // Landed on it with less than 0.1 of vertical speed left, the entity
        // also loses horizontal speed: it keeps 0.4 plus a fifth of that
        // speed (both official servers; slime_landing).
        if (slime && std::abs(velocity.y) < 0.1) {
            const double keep = 0.4 + std::abs(velocity.y) * 0.2;
            velocity.x *= keep;
            velocity.z *= keep;
        }
    }
    return landed;
}
} // namespace detail

namespace {
using detail::water_at;

// Items that may merge: not of the ages and pickup delays meaning "never".
bool mergeable(const Entity& e) {
    return e.kind == EntityKind::item && e.pickup_delay != item_pickup_never && e.age != item_age_never;
}
// Stacks that merge into one: the same kind, and together within the limit
// (40 and 40 redstone stay apart, 60 and 4 stone merge; unstackable items
// never merge).
bool stacks_merge(const ItemStack& a, const ItemStack& b) {
    return a.same_kind(b) && a.count + b.count <= items::max_stack(a);
}

// `from` joins `into`: its count, the younger age and the longer pickup
// delay; `from` is removed.
void absorb(RuleContext& ctx, EntityId into_id, EntityId from_id) {
    auto* into = ctx.entity(into_id);
    auto* from = ctx.entity(from_id);
    const auto before = into->stack;
    into->stack.count = static_cast<std::uint8_t>(into->stack.count + from->stack.count);
    into->age = std::min(into->age, from->age);
    into->pickup_delay = std::max(into->pickup_delay, from->pickup_delay);
    ctx.record(TraceKind::entity_stack_changed, contracts::block_of(into->position), into_id.value, contracts::pack_stack(before),
               contracts::pack_stack(into->stack));
    ctx.record(TraceKind::entity_removed, contracts::block_of(from->position), from_id.value, static_cast<std::uint64_t>(RemovalReason::merged));
    ctx.remove_entity(from_id);
}

// Merges an item with the items whose boxes reach within half a block of it
// horizontally (none above or below it), in ascending id order: it takes in
// a smaller stack and joins any other. Returns whether it still exists.
bool merge_with_neighbours(RuleContext& ctx, EntityId id) {
    const auto* self = ctx.peek_entity(id);
    if (!self || !mergeable(*self)) return self != nullptr;
    for (const auto other : detail::entities_intersecting(ctx, detail::entity_box(*self).inflate(0.5, 0.0, 0.5))) {
        if (other == id) continue;
        const auto* a = ctx.peek_entity(id);
        const auto* b = ctx.peek_entity(other);
        if (!b || !mergeable(*b) || !stacks_merge(a->stack, b->stack)) continue;
        if (b->stack.count < a->stack.count) {
            absorb(ctx, id, other);
        } else {
            absorb(ctx, other, id);
            return false;
        }
    }
    return true;
}

// Hoppers in the cells an item's box overlaps take it, the first in cell
// order; returns whether the item was taken whole.
bool enter_hoppers(RuleContext& ctx, EntityId id) {
    const auto* e = ctx.peek_entity(id);
    const auto box = detail::entity_box(*e).deflate(1.0e-7);
    const auto cell = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    for (auto x = cell(box.min_x); x <= cell(box.max_x); ++x)
        for (auto y = cell(box.min_y); y <= cell(box.max_y); ++y)
            for (auto z = cell(box.min_z); z <= cell(box.max_z); ++z) {
                const contracts::BlockPos p{x, y, z};
                if (!ctx.world().contains_y(p.y) || !ctx.block(p).is(BlockType::hopper)) continue;
                if (detail::hopper_takes_item(ctx, p, id)) return true;
            }
    return false;
}

void run_item(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    if (e.pickup_delay > 0 && e.pickup_delay != item_pickup_never) --e.pickup_delay;
    const auto ticks = e.tick_count;
    // Burning first: 1 when its fire ticks are a multiple of 20, out of lava;
    // on 1.21.4 lava then hurts it, but in its first tick (entities_in_lava).
    const bool resistant = detail::fire_resistant(e.stack);
    double lava_depth = 0.0;
    const bool lava = detail::in_lava(ctx, detail::entity_box(e), &lava_depth);
    if (water_at(ctx, detail::entity_box(e)).inside) e.fire = 0; // water puts it out
    int burnt = 0;
    if (e.fire > 0) {
        if (resistant) e.fire = std::max(0, e.fire - 4);
        else {
            if (e.fire % 20 == 0 && !lava) ++burnt;
            --e.fire;
        }
    }
    if (ctx.bundle().rules->lava == contracts::LavaContact::tick_start && ticks > 1 && lava && !resistant) {
        burnt += 4;
        e.fire = std::max(e.fire, 300);
    }
    if (detail::hurt_item(ctx, id, burnt)) return;
    const auto start = e.position;
    auto v = e.velocity;
    auto p = e.position;
    // Currents push before the move and after it. An item more than 0.1 deep
    // floats up by 0.0005 a tick (while slower than 0.06) and loses 1% of its
    // horizontal speed, instead of falling; more than 0.1 deep in lava it
    // floats as slowly and loses 5% (entities_in_lava).
    if (const auto water = water_at(ctx, detail::entity_box(e)); water.inside) {
        e.fall_distance = 0.0; // water breaks the fall
        v = {v.x + water.push.x, v.y + water.push.y, v.z + water.push.z};
        if (water.depth > 0.1) {
            constexpr double drag = 0.99f, rise = 0.0005f;
            v = {v.x * drag, v.y + (v.y < 0.06 ? rise : 0.0), v.z * drag};
        } else {
            v.y -= gravity;
        }
    } else if (lava && lava_depth > 0.1) {
        constexpr double drag = 0.95f, rise = 0.0005f;
        v = {v.x * drag, v.y + (v.y < static_cast<double>(0.06f) ? rise : 0.0), v.z * drag};
    } else {
        v.y -= gravity;
    }
    const bool moves = !(e.on_ground && v.x * v.x + v.z * v.z <= 1.0e-5 && (ticks + id.value) % 4 != 0);
    if (moves) {
        Vec3 moved{};
        e.on_ground = detail::move_entity(ctx, p, v, item_size, item_size, &e, &moved);
        detail::entity_fell(ctx, e, p, moved, e.on_ground);
        detail::honey_slide(ctx, p, v, item_size, item_size, e.on_ground);
        float f = 0.98f;
        if (e.on_ground) f = slipperiness(ctx.block(contracts::block_of(Vec3{p.x, p.y - 0.5000001, p.z}))) * 0.98f;
        v = {v.x * static_cast<double>(f), v.y * 0.98, v.z * static_cast<double>(f)};
        if (e.on_ground && v.y < 0.0) v.y *= -0.5;
    }
    if (const auto water = water_at(ctx, physics::box_at(p, item_size, item_size)); water.inside)
        v = {v.x + water.push.x, v.y + water.push.y, v.z + water.push.z};
    e.velocity = v;
    if (p != start) {
        const auto from = contracts::chunk_of(contracts::block_of(start));
        const auto to = contracts::chunk_of(contracts::block_of(p));
        if (from != to)
            ctx.record(TraceKind::entity_changed_chunk, contracts::block_of(p), id.value, contracts::pack_chunk(from), contracts::pack_chunk(to));
        ctx.relocate_entity(id, p); // `e` must not be used after relocation.
    }
    if (moves) {
        // Plates and buttons in the cells its box reaches react to the move;
        // cacti there hurt it.
        if (const auto* self = ctx.peek_entity(id)) detail::entity_inside(ctx, *self);
        if (detail::cactus_hurts(ctx, id)) return;
        // Fire its box reaches hurts it and sets it burning; so does lava on
        // 1.21.8 (items_in_fire, entities_in_lava).
        if (auto* self = ctx.entity(id); self && !resistant) {
            int damage = 0;
            const auto box = detail::entity_box(*self);
            if (const int f = detail::fire_contact(ctx, box); f >= 0) {
                damage += f;
                self->fire = std::max(self->fire, 160);
            }
            if (ctx.bundle().rules->lava == contracts::LavaContact::after_move && detail::in_lava(ctx, box)) {
                damage += 4;
                self->fire = std::max(self->fire, 300);
            }
            if (detail::hurt_item(ctx, id, damage)) return;
        }
        if (enter_hoppers(ctx, id)) return;
    }
    const bool changed_block = contracts::block_of(start) != contracts::block_of(p);
    if ((ticks % 40 == 0 || (changed_block && ticks % 2 == 0)) && !merge_with_neighbours(ctx, id)) return;
    auto* self = ctx.entity(id);
    if (self->age != item_age_never) ++self->age;
    if (self->age >= item_despawn_age) {
        ctx.record(TraceKind::entity_removed, contracts::block_of(self->position), id.value, static_cast<std::uint64_t>(RemovalReason::despawned));
        ctx.remove_entity(id);
    }
}

// ---- experience orbs -------------------------------------------------------
//
// Experience orbs, as both official servers run them without players near
// (checked against their traces of falling, sliding, resting, merging and
// floating orbs; minecraft.wiki "Experience orb"). Each tick:
// - the tick count goes up;
// - with its eye (0.425 above its feet) in water it floats up by 0.0005 a
//   tick (while slower than 0.06) and loses 1% of its horizontal speed,
//   whether the eye was in water decided where it was at the tick's start;
//   otherwise gravity takes 0.03 from the vertical velocity;
// - it moves; its velocity is damped by 0.98 (the block's slipperiness
//   times 0.98 on the ground) in single precision, but on 1.21.4 its
//   vertical velocity by 0.98 in double precision (OrbMotion); on the ground
//   it bounces back at 0.4 of the speed it moved with (on 1.21.8 only when
//   that was faster than 0.03, else it keeps what the block left it: none,
//   or a slime block's bounce);
// - in the ticks whose tick count is 1 more than a multiple of 20 (its first
//   among them) it takes in the orbs of its experience whose ids are a
//   multiple of 40 apart from its own and whose boxes reach within half a
//   block of its own: their orbs join its own, it keeps the younger age;
// - its age goes up; at 6000 it is gone.
// The nearest player within 8 blocks draws it, and a player who touches it
// takes it (contracts::PickUp). Fire and lava hurt it as they hurt items (5
// health). In lava the official
// servers make it jump by an unseeded random of its own; Circuit lifts it
// by 0.2 without the random, which is not compared.
constexpr double orb_size = 0.5;
constexpr double orb_gravity = 0.03;
const double orb_eye = static_cast<double>(0.425f);

bool eye_in_water(RuleContext& ctx, Vec3 feet) {
    const double eye = feet.y + orb_eye;
    const auto cell = contracts::block_of(Vec3{feet.x, eye, feet.z});
    if (!ctx.world().contains_y(cell.y)) return false;
    const auto s = ctx.block(cell);
    if (!s.is(BlockType::water)) return false;
    const auto above = ctx.world().contains_y(cell.y + 1) ? ctx.block(cell.relative(Direction::up)) : blocks::air();
    return eye < static_cast<double>(static_cast<float>(cell.y) + fluid_height(s, above));
}

// Takes in the orbs this one may merge with (see above); ascending id order.
void merge_orbs(RuleContext& ctx, EntityId id) {
    const auto* self = ctx.peek_entity(id);
    if (!self) return;
    const auto area = detail::entity_box(*self).inflate(0.5, 0.5, 0.5);
    for (const auto other : detail::entities_intersecting(ctx, area)) {
        if (other == id) continue;
        const auto* o = ctx.peek_entity(other);
        const auto* me = ctx.peek_entity(id);
        if (!o || !me || o->kind != EntityKind::experience_orb || o->experience != me->experience) continue;
        const auto apart = other.value > id.value ? other.value - id.value : id.value - other.value;
        if (apart % 40 != 0) continue;
        const auto orbs = o->orbs;
        const auto age = o->age;
        const auto cell = contracts::block_of(o->position);
        ctx.record(TraceKind::entity_removed, cell, other.value, static_cast<std::uint64_t>(RemovalReason::merged));
        ctx.remove_entity(other);
        auto* into = ctx.entity(id);
        into->orbs = static_cast<std::int32_t>(std::min<std::int64_t>(std::int64_t{into->orbs} + orbs, INT32_MAX));
        into->age = std::min(into->age, age);
    }
}

void run_orb(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    const bool settling = ctx.bundle().rules->orbs == contracts::OrbMotion::settling;
    const auto start = e.position;
    auto v = e.velocity;
    auto p = e.position;
    double lava_depth = 0.0;
    if (eye_in_water(ctx, p)) {
        constexpr double drag = 0.99f, rise = 0.0005f;
        v = {v.x * drag, v.y + (v.y < 0.06 ? rise : 0.0), v.z * drag};
    } else {
        v.y -= orb_gravity;
    }
    if (detail::in_lava(ctx, detail::entity_box(e), &lava_depth)) v.y = 0.2;
    // The nearest player within 8 blocks draws it toward the middle of their
    // body, the more the nearer they are (not compared tick by tick).
    if (const auto player = ctx.nearest_player(p, 8.0)) {
        const Vec3 to{player->position.x - p.x, player->position.y + static_cast<double>(1.62f) / 2.0 - p.y, player->position.z - p.z};
        const double length = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
        if (length > 0.0 && length < 8.0) {
            const double pull = (1.0 - length / 8.0) * (1.0 - length / 8.0) * 0.1;
            v = {v.x + to.x / length * pull, v.y + to.y / length * pull, v.z + to.z / length * pull};
        }
    }
    const double fall = v.y;
    Vec3 moved{};
    e.on_ground = detail::move_entity(ctx, p, v, orb_size, orb_size, &e, &moved);
    float f = 0.98f;
    if (e.on_ground) f = slipperiness(ctx.block(contracts::block_of(Vec3{p.x, p.y - 0.5000001, p.z}))) * 0.98f;
    v = {v.x * static_cast<double>(f), settling ? v.y * static_cast<double>(f) : v.y * 0.98, v.z * static_cast<double>(f)};
    if (e.on_ground && (!settling || fall < -orb_gravity)) v.y = fall * -0.4;
    e.velocity = v;
    if (p != start) {
        const auto from = contracts::chunk_of(contracts::block_of(start));
        const auto to = contracts::chunk_of(contracts::block_of(p));
        if (from != to)
            ctx.record(TraceKind::entity_changed_chunk, contracts::block_of(p), id.value, contracts::pack_chunk(from), contracts::pack_chunk(to));
        ctx.relocate_entity(id, p); // `e` must not be used after relocation.
    }
    // Fire its box reaches and lava hurt it.
    if (const auto* self = ctx.peek_entity(id)) {
        const auto box = detail::entity_box(*self);
        int damage = 0;
        if (const int fire = detail::fire_contact(ctx, box); fire >= 0) damage += fire;
        if (detail::in_lava(ctx, box)) damage += 4;
        if (detail::hurt_item(ctx, id, damage)) return;
    }
    if (ctx.peek_entity(id)->tick_count % 20 == 1) merge_orbs(ctx, id);
    auto* self = ctx.entity(id);
    if (++self->age >= orb_despawn_age) {
        ctx.record(TraceKind::entity_removed, contracts::block_of(self->position), id.value, static_cast<std::uint64_t>(RemovalReason::despawned));
        ctx.remove_entity(id);
    }
}
} // namespace

namespace detail {
physics::Aabb entity_box(const Entity& e) {
    switch (e.kind) {
    case EntityKind::item: case EntityKind::snowball: return physics::box_at(e.position, item_size, item_size);
    case EntityKind::arrow: return physics::box_at(e.position, 0.5, 0.5);
    case EntityKind::minecart: return physics::box_at(e.position, static_cast<double>(0.98f), static_cast<double>(0.7f));
    case EntityKind::boat: return physics::box_at(e.position, 1.375, 0.5625);
    case EntityKind::experience_orb: return physics::box_at(e.position, orb_size, orb_size);
    default: break;
    }
    const double size = static_cast<double>(0.98f);
    return physics::box_at(e.position, size, size);
}

std::vector<EntityId> entities_in_section_order(RuleContext& ctx, std::vector<EntityId> ids) {
    // Sections by chunk x (ascending), then chunk z and section y as the
    // target versions key them: in 22 and 20 bits, so that negative ones
    // come after positive ones (tnt_items, tnt_entity_order).
    const auto key = [&](EntityId id) {
        const auto& p = ctx.peek_entity(id)->position;
        const auto c = contracts::block_of(p);
        return std::tuple{c.x >> 4, static_cast<std::uint32_t>(c.z >> 4) & 0x3FFFFFU, static_cast<std::uint32_t>(c.y >> 4) & 0xFFFFFU, id.value};
    };
    std::ranges::sort(ids, {}, key);
    return ids;
}

std::vector<EntityId> entities_intersecting(RuleContext& ctx, const physics::Aabb& area) {
    // An entity's box reaches less than a block beyond the cell of its
    // position sideways, and less than a block above it.
    const auto cell = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    const BlockPos low{cell(area.min_x) - 1, cell(area.min_y) - 1, cell(area.min_z) - 1};
    const BlockPos high{cell(area.max_x) + 1, cell(area.max_y), cell(area.max_z) + 1};
    std::vector<EntityId> out;
    for (const auto id : ctx.entities_in_cells(low, high))
        if (const auto* e = ctx.peek_entity(id); e && entity_box(*e).intersects(area)) out.push_back(id);
    return out;
}
} // namespace detail

void run_entity(RuleContext& ctx, EntityId id) {
    auto* e = ctx.entity(id);
    if (!e) return;
    const auto now = ctx.now();
    if (e->last_tick == now) return; // Basic advancement happens at most once per logical tick.
    e->last_tick = now;
    // An entity more than 64 below the world is gone at the start of its
    // tick (items: torch_basics); falling blocks and primed TNT fall on
    // until their time is up (falling_void, tnt_void).
    if (e->kind != EntityKind::falling_block && e->kind != EntityKind::primed_tnt
        && e->position.y < static_cast<double>(ctx.world().min_y - 64)) {
        ctx.record(TraceKind::entity_removed, contracts::block_of(e->position), id.value,
                   static_cast<std::uint64_t>(RemovalReason::fell_out_of_world));
        ctx.remove_entity(id);
        return;
    }
    const auto kind = e->kind;
    switch (kind) {
    case EntityKind::item: run_item(ctx, id, *e); break;
    case EntityKind::falling_block: detail::run_falling_block(ctx, id, *e); break;
    case EntityKind::primed_tnt: detail::run_primed_tnt(ctx, id, *e); break;
    case EntityKind::arrow: detail::run_arrow(ctx, id, *e); break;
    case EntityKind::snowball: detail::run_snowball(ctx, id, *e); break;
    case EntityKind::minecart: detail::run_minecart(ctx, id, *e); break;
    case EntityKind::boat: detail::run_boat(ctx, id, *e); break;
    case EntityKind::experience_orb: run_orb(ctx, id, *e); break;
    }
    // Entities that move every tick press plates and make buttons check
    // where their move ends (items, arrows and snowballs do in their tick).
    switch (kind) {
    case EntityKind::falling_block: case EntityKind::primed_tnt: case EntityKind::minecart: case EntityKind::boat:
        if (const auto* after = ctx.peek_entity(id)) detail::entity_inside(ctx, *after);
        break;
    default: break;
    }
}

} // namespace circuit::gameplay
