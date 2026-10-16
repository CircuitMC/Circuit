// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/physics.hpp>
#include <algorithm>
#include <cmath>

// Item entities, as both official servers run them (checked against their
// traces of falling, sliding, resting and merging items; minecraft.wiki
// "Item (entity)" and "Entity"). Each tick:
// - the tick count goes up, and the pickup delay down;
// - gravity takes 0.04 from the vertical velocity;
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

physics::ShapeAt shapes(RuleContext& ctx) {
    return [&ctx](contracts::BlockPos p) -> std::span<const registry::Box> {
        if (!ctx.world().contains_y(p.y)) return {};
        const auto s = ctx.block(p);
        if (s.is_unloaded()) return {&full_cube, 1};
        return physics::collision_boxes(s, ctx.bundle().schema.content_release);
    };
}

// minecraft.wiki "Slipperiness": 0.6 but for ice (0.98), blue ice (0.989)
// and slime blocks (0.8).
float slipperiness(BlockState s) {
    if (!s.is(BlockType::other)) return 0.6f;
    const auto block = registry::block_of(s.registry_state());
    if (!block) return 0.6f;
    const auto name = registry::block(*block).name;
    if (name == "ice" || name == "packed_ice" || name == "frosted_ice") return 0.98f;
    if (name == "blue_ice") return 0.989f;
    if (name == "slime_block") return 0.8f;
    return 0.6f;
}

// Moves an entity's box by its velocity against the blocks around it; the
// velocity loses the components that collided, and the vertical one too
// when it hit something. A move that a collision shortened to a squared
// length below 1e-7 is not made (both official servers leave such items in
// place, while longer moves, from 1.8e-7, are made). Returns whether it
// landed (hit something while falling).
bool move(RuleContext& ctx, Vec3& position, Vec3& velocity, double width, double height) {
    const auto box = physics::box_at(position, width, height);
    const auto done = physics::collide(box, velocity, shapes(ctx));
    const double made = done.x * done.x + done.y * done.y + done.z * done.z;
    const double wanted = velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
    if (made > 1.0e-7 || wanted - made < 1.0e-7) position = {position.x + done.x, position.y + done.y, position.z + done.z};
    const bool landed = done.y != velocity.y && velocity.y < 0.0;
    if (done.x != velocity.x) velocity.x = 0.0;
    if (done.z != velocity.z) velocity.z = 0.0;
    if (done.y != velocity.y) velocity.y = 0.0;
    return landed;
}

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
    v.y -= gravity;
    const bool moves = !(e.on_ground && v.x * v.x + v.z * v.z <= 1.0e-5 && (ticks + id.value) % 4 != 0);
    if (moves) {
        e.on_ground = move(ctx, p, v, item_size, item_size);
        float f = 0.98f;
        if (e.on_ground) f = slipperiness(ctx.block(contracts::block_of(Vec3{p.x, p.y - 0.5000001, p.z}))) * 0.98f;
        v = {v.x * static_cast<double>(f), v.y * 0.98, v.z * static_cast<double>(f)};
        if (e.on_ground && v.y < 0.0) v.y *= -0.5;
    }
    e.velocity = v;
    if (p.y < static_cast<double>(ctx.world().min_y - 64)) {
        ctx.record(TraceKind::entity_removed, contracts::block_of(p), id.value, static_cast<std::uint64_t>(RemovalReason::fell_out_of_world));
        ctx.remove_entity(id);
        return;
    }
    if (p != start) {
        const auto from = contracts::chunk_of(contracts::block_of(start));
        const auto to = contracts::chunk_of(contracts::block_of(p));
        if (from != to)
            ctx.record(TraceKind::entity_changed_chunk, contracts::block_of(p), id.value, contracts::pack_chunk(from), contracts::pack_chunk(to));
        ctx.relocate_entity(id, p); // `e` must not be used after relocation.
    }
    if (moves && enter_hoppers(ctx, id)) return;
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
physics::Aabb entity_box(const Entity& e) { return physics::box_at(e.position, item_size, item_size); }

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
    switch (e->kind) {
    case EntityKind::item: run_item(ctx, id, *e); break;
    }
}

} // namespace circuit::gameplay
