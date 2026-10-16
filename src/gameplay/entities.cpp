// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/physics.hpp>
#include <algorithm>
#include <cmath>

// Item entities, as both official servers move them (checked against their
// traces of falling, sliding and resting items; minecraft.wiki "Item
// (entity)" and "Entity"). Each tick:
// - the pickup delay counts down, and the age up; at 6000 the item despawns;
// - gravity takes 0.04 from the vertical velocity;
// - the item moves, unless it rests on the ground with a horizontal velocity
//   whose square is at most 1e-5, in which case it moves only when its age
//   plus its id is a multiple of 4;
// - after a move the velocity is damped: horizontally by 0.98 (the block's
//   slipperiness times 0.98 on the ground), both in single precision, and
//   vertically by 0.98; an item on the ground bounces back up at half its
//   downward speed.
namespace circuit::gameplay {
namespace {
using contracts::Vec3;
constexpr std::uint32_t despawn_age = 6000;
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
// when it hit something. Returns whether it landed (hit something while
// falling).
bool move(RuleContext& ctx, Vec3& position, Vec3& velocity, double width, double height) {
    const auto box = physics::box_at(position, width, height);
    const auto done = physics::collide(box, velocity, shapes(ctx));
    position = {position.x + done.x, position.y + done.y, position.z + done.z};
    const bool landed = done.y != velocity.y && velocity.y < 0.0;
    if (done.x != velocity.x) velocity.x = 0.0;
    if (done.z != velocity.z) velocity.z = 0.0;
    if (done.y != velocity.y) velocity.y = 0.0;
    return landed;
}

void run_item(RuleContext& ctx, EntityId id, Entity& e) {
    if (e.pickup_delay > 0 && e.pickup_delay != 32767) --e.pickup_delay; // 32767: never
    e.age += 1;
    const auto start = e.position;
    if (e.age >= despawn_age) {
        ctx.record(TraceKind::entity_removed, contracts::block_of(start), id.value, static_cast<std::uint64_t>(RemovalReason::despawned));
        ctx.remove_entity(id);
        return;
    }
    auto v = e.velocity;
    auto p = e.position;
    v.y -= gravity;
    const bool resting = e.on_ground && v.x * v.x + v.z * v.z <= 1.0e-5 && (e.age + id.value) % 4 != 0;
    if (!resting) {
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
}
} // namespace

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
