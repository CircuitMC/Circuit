// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay {
namespace {
using contracts::block_of;
using contracts::fixed_one;
constexpr std::uint32_t despawn_age = 6000;
constexpr std::int64_t gravity = 160;              // ~0.04 blocks per tick squared
constexpr std::int64_t max_speed = fixed_one - 1;  // below one block per tick: no cell is skipped

std::int64_t clamp_speed(std::int64_t v) { return std::clamp(v, -max_speed, max_speed); }
} // namespace

void run_entity(RuleContext& ctx, EntityId id) {
    auto* e = ctx.entity(id);
    if (!e) return;
    const auto now = ctx.now();
    if (e->last_tick == now) return; // Basic advancement happens at most once per logical tick.
    e->last_tick = now;
    e->age += 1;
    const auto start = e->position;
    if (e->age >= despawn_age) {
        ctx.record(TraceKind::entity_removed, block_of(start), id.value, static_cast<std::uint64_t>(RemovalReason::despawned));
        ctx.remove_entity(id);
        return;
    }
    auto v = e->velocity;
    v.y = clamp_speed(v.y - gravity);
    auto p = start;
    bool on_ground = false;

    auto next = p;
    next.y += v.y;
    if (v.y != 0) {
        const auto cell = block_of(next);
        if (is_solid(ctx.block(cell).type())) {
            if (v.y < 0) {
                next.y = (static_cast<std::int64_t>(cell.y) + 1) * fixed_one;
                on_ground = true;
            } else {
                next.y = p.y;
            }
            v.y = 0;
        }
    }
    p = next;
    const auto move_axis = [&](std::int64_t FixedVec::*axis, std::int64_t& speed) {
        if (speed == 0) return;
        auto candidate = p;
        candidate.*axis += speed;
        const auto cell = block_of(candidate);
        // Entities do not enter unloaded chunks.
        if (!ctx.chunk_generation(contracts::chunk_of(cell)) || is_solid(ctx.block(cell).type())) {
            speed = 0;
            return;
        }
        p = candidate;
    };
    move_axis(&FixedVec::x, v.x);
    move_axis(&FixedVec::z, v.z);

    v.x = v.x * 98 / 100;
    v.y = v.y * 98 / 100;
    v.z = v.z * 98 / 100;
    if (on_ground) {
        v.x = v.x * 6 / 10;
        v.z = v.z * 6 / 10;
    }
    e->velocity = {clamp_speed(v.x), clamp_speed(v.y), clamp_speed(v.z)};
    e->on_ground = on_ground;

    if (p.y < static_cast<std::int64_t>(ctx.bundle().world.min_y - 64) * fixed_one) {
        ctx.record(TraceKind::entity_removed, block_of(p), id.value, static_cast<std::uint64_t>(RemovalReason::fell_out_of_world));
        ctx.remove_entity(id);
        return;
    }
    if (p != start) {
        const auto from = contracts::chunk_of(block_of(start));
        const auto to = contracts::chunk_of(block_of(p));
        if (from != to)
            ctx.record(TraceKind::entity_changed_chunk, block_of(p), id.value, contracts::pack_chunk(from), contracts::pack_chunk(to));
        ctx.relocate_entity(id, p); // `e` must not be used after relocation.
    }
}

} // namespace circuit::gameplay
