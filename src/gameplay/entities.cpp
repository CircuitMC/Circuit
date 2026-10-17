// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
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
                const double top = static_cast<double>(y) + static_cast<double>(fluid_height(s, ctx.block(p.relative(Direction::up))));
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

bool move_entity(RuleContext& ctx, Vec3& position, Vec3& velocity, double width, double height, const Entity* self) {
    return move_entity(ctx, position, velocity, velocity, width, height, self);
}

bool move_entity(RuleContext& ctx, Vec3& position, Vec3& velocity, Vec3 movement, double width, double height, const Entity* self) {
    const bool landed = move_entity_by(ctx, position, velocity, movement, width, height, self);
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

bool move_entity_by(RuleContext& ctx, Vec3& position, Vec3& velocity, Vec3 movement, double width, double height, const Entity* self) {
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
    const auto start = e.position;
    auto v = e.velocity;
    auto p = e.position;
    // Currents push before the move and after it. An item more than 0.1 deep
    // floats up by 0.0005 a tick (while slower than 0.06) and loses 1% of its
    // horizontal speed, instead of falling.
    if (const auto water = water_at(ctx, detail::entity_box(e)); water.inside) {
        v = {v.x + water.push.x, v.y + water.push.y, v.z + water.push.z};
        if (water.depth > 0.1) {
            constexpr double drag = 0.99f, rise = 0.0005f;
            v = {v.x * drag, v.y + (v.y < 0.06 ? rise : 0.0), v.z * drag};
        } else {
            v.y -= gravity;
        }
    } else {
        v.y -= gravity;
    }
    const bool moves = !(e.on_ground && v.x * v.x + v.z * v.z <= 1.0e-5 && (ticks + id.value) % 4 != 0);
    if (moves) {
        e.on_ground = detail::move_entity(ctx, p, v, item_size, item_size, &e);
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
        // Plates and buttons in the cells its box reaches react to the move.
        if (const auto* self = ctx.peek_entity(id)) detail::entity_inside(ctx, *self);
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
} // namespace

namespace detail {
physics::Aabb entity_box(const Entity& e) {
    switch (e.kind) {
    case EntityKind::item: case EntityKind::snowball: return physics::box_at(e.position, item_size, item_size);
    case EntityKind::arrow: return physics::box_at(e.position, 0.5, 0.5);
    case EntityKind::minecart: return physics::box_at(e.position, static_cast<double>(0.98f), static_cast<double>(0.7f));
    case EntityKind::boat: return physics::box_at(e.position, 1.375, 0.5625);
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
