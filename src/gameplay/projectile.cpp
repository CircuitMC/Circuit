// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/physics.hpp>
#include <array>
#include <cmath>
#include <optional>

// Arrows and snowballs, as both official servers move them (checked against
// their traces; minecraft.wiki "Arrow", "Snowball").
//
// An arrow in flight moves by its velocity unless the segment it covers
// meets a block's collision shape first; it then stops 0.05 (in single
// precision) short of the point it hit on every axis it moved along, stuck
// in the ground with no velocity. After the move its velocity loses 1% (40%
// when the arrow started the tick in water) and 0.05 downwards. A burning
// arrow counts its fire down while it flies, after fire and (on 1.21.8)
// lava its box reaches set it burning for at least 160 and 300 ticks (1.21.4
// lava after the count; a stuck arrow burns as long in them and its fire
// does not count), goes out in water it moves into, and primes TNT it hits
// (items_in_fire, entities_in_lava). A stuck
// arrow counts its life up and despawns at 1,200; when no block's shape is
// left within 0.06 of its position it starts falling again in the next tick.
//
// A snowball falls by 0.03, loses 1% of its velocity, then moves the same
// way; it breaks on the first block it hits.
namespace circuit::gameplay::detail {
namespace {
using contracts::Vec3;
constexpr double arrow_gravity = 0.05;
constexpr double snowball_gravity = 0.03;
constexpr double inset = static_cast<double>(0.05f);
constexpr std::int32_t arrow_despawn_life = 1200;

struct Hit {
    Vec3 at;
    BlockPos block;
    int axis{}; // of the face hit
};

// Where the segment from `from` to `to` first meets a block's collision
// box, visiting the cells it passes through in order.
std::optional<Hit> clip(RuleContext& ctx, Vec3 from, Vec3 to) {
    const auto& release = ctx.bundle().schema.content_release;
    const std::array<double, 3> start{from.x, from.y, from.z}, delta{to.x - from.x, to.y - from.y, to.z - from.z};
    std::array<std::int32_t, 3> cell{static_cast<std::int32_t>(std::floor(from.x)), static_cast<std::int32_t>(std::floor(from.y)),
                                     static_cast<std::int32_t>(std::floor(from.z))};
    const std::array<std::int32_t, 3> last{static_cast<std::int32_t>(std::floor(to.x)), static_cast<std::int32_t>(std::floor(to.y)),
                                           static_cast<std::int32_t>(std::floor(to.z))};
    std::array<int, 3> step{};
    std::array<double, 3> t_max{}, t_delta{};
    for (int i = 0; i < 3; ++i) {
        step[i] = delta[i] > 0.0 ? 1 : delta[i] < 0.0 ? -1 : 0;
        if (step[i] == 0) {
            t_max[i] = t_delta[i] = 2.0;
            continue;
        }
        const double boundary = step[i] > 0 ? std::floor(start[i]) + 1.0 : std::floor(start[i]);
        t_max[i] = (boundary - start[i]) / delta[i];
        t_delta[i] = static_cast<double>(step[i]) / delta[i];
    }
    for (int guard = 0; guard < 4096; ++guard) {
        const BlockPos p{cell[0], cell[1], cell[2]};
        const auto s = ctx.world().contains_y(p.y) ? ctx.block(p) : blocks::air();
        if (!s.is_air()) {
            std::optional<double> best;
            int axis = 0;
            const auto boxes = s.is_unloaded() ? std::span<const registry::Box>{} : physics::collision_boxes(s, release);
            for (const auto& b : boxes) {
                const std::array<double, 3> low{p.x + b.min_x, p.y + b.min_y, p.z + b.min_z};
                const std::array<double, 3> high{p.x + b.max_x, p.y + b.max_y, p.z + b.max_z};
                double t0 = 0.0, t1 = 1.0;
                int entered = -1;
                bool inside = true, missed = false;
                for (int i = 0; i < 3 && !missed; ++i) {
                    if (delta[i] == 0.0) {
                        if (start[i] < low[i] || start[i] > high[i]) missed = true;
                        inside = inside && start[i] > low[i] && start[i] < high[i];
                        continue;
                    }
                    double ta = (low[i] - start[i]) / delta[i], tb = (high[i] - start[i]) / delta[i];
                    if (ta > tb) std::swap(ta, tb);
                    inside = inside && ta < 0.0;
                    if (ta > t0) t0 = ta, entered = i;
                    t1 = std::min(t1, tb);
                    if (t0 > t1) missed = true;
                }
                if (missed) continue;
                // A segment that starts inside a box hits it at once.
                const double t = inside ? 0.001 : t0;
                if (!best || t < *best) {
                    best = t;
                    // Inside: the face its main motion leads to.
                    axis = entered >= 0 ? entered
                         : std::abs(delta[0]) >= std::abs(delta[1]) && std::abs(delta[0]) >= std::abs(delta[2]) ? 0
                         : std::abs(delta[1]) >= std::abs(delta[2])                                         ? 1
                                                                                                           : 2;
                }
            }
            if (best)
                return Hit{{from.x + *best * delta[0], from.y + *best * delta[1], from.z + *best * delta[2]}, p, axis};
        }
        if (cell == last) return std::nullopt;
        const int axis = t_max[0] <= t_max[1] ? (t_max[0] <= t_max[2] ? 0 : 2) : (t_max[1] <= t_max[2] ? 1 : 2);
        if (t_max[axis] > 1.0) return std::nullopt;
        cell[axis] += step[axis];
        t_max[axis] += t_delta[axis];
    }
    return std::nullopt;
}

double sign(double v) { return v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0; }

void moved(RuleContext& ctx, EntityId id, Vec3 start, Vec3 to) {
    if (to == start) return;
    const auto from = contracts::chunk_of(contracts::block_of(start));
    const auto chunk = contracts::chunk_of(contracts::block_of(to));
    if (from != chunk)
        ctx.record(TraceKind::entity_changed_chunk, contracts::block_of(to), id.value, contracts::pack_chunk(from), contracts::pack_chunk(chunk));
    ctx.relocate_entity(id, to);
}

void remove(RuleContext& ctx, EntityId id, Vec3 at, RemovalReason reason) {
    ctx.record(TraceKind::entity_removed, contracts::block_of(at), id.value, static_cast<std::uint64_t>(reason));
    ctx.remove_entity(id);
}

// Whether any block's collision shape meets the box.
bool touches_blocks(RuleContext& ctx, const physics::Aabb& box) {
    const auto& release = ctx.bundle().schema.content_release;
    for (auto x = static_cast<std::int32_t>(std::floor(box.min_x)); x <= static_cast<std::int32_t>(std::floor(box.max_x)); ++x)
        for (auto y = static_cast<std::int32_t>(std::floor(box.min_y)); y <= static_cast<std::int32_t>(std::floor(box.max_y)); ++y)
            for (auto z = static_cast<std::int32_t>(std::floor(box.min_z)); z <= static_cast<std::int32_t>(std::floor(box.max_z)); ++z) {
                if (!ctx.world().contains_y(y)) continue;
                const auto s = ctx.block({x, y, z});
                if (s.is_unloaded()) return true;
                for (const auto& b : physics::collision_boxes(s, release))
                    if (physics::Aabb{x + b.min_x, y + b.min_y, z + b.min_z, x + b.max_x, y + b.max_y, z + b.max_z}.intersects(box)) return true;
            }
    return false;
}
} // namespace

namespace {
// Fire and lava an arrow's box reaches at `at`: at least 160 and 300 ticks
// of fire; lava only `with_lava`.
void arrow_in_fire(RuleContext& ctx, Vec3 at, std::int32_t& fire, bool with_lava) {
    const auto box = physics::box_at(at, 0.5, 0.5);
    if (fire_contact(ctx, box) >= 0) fire = std::max(fire, 160);
    if (with_lava && in_lava(ctx, box)) fire = std::max(fire, 300);
}

void step_arrow(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    // An arrow inside the collision shape of its cell's block is stuck in it.
    if (const auto cell = contracts::block_of(e.position); !e.in_ground && ctx.world().contains_y(cell.y)) {
        const auto s = ctx.block(cell);
        if (!s.is_unloaded())
            for (const auto& b : physics::collision_boxes(s, ctx.bundle().schema.content_release)) {
                const auto& p = e.position;
                if (p.x >= cell.x + b.min_x && p.x < cell.x + b.max_x && p.y >= cell.y + b.min_y && p.y < cell.y + b.max_y
                    && p.z >= cell.z + b.min_z && p.z < cell.z + b.max_z)
                    e.in_ground = true;
            }
    }
    if (e.in_ground) {
        const auto& p = e.position;
        if (!touches_blocks(ctx, physics::Aabb{p.x, p.y, p.z, p.x, p.y, p.z}.inflate(0.06, 0.06, 0.06))) {
            // Its block is gone: it falls from the next tick on.
            e.in_ground = false;
            e.life = 0;
            return;
        }
        arrow_in_fire(ctx, e.position, e.fire, true);
        if (++e.life >= arrow_despawn_life) remove(ctx, id, e.position, RemovalReason::despawned);
        return;
    }
    const bool in_water = water_at(ctx, entity_box(e)).inside;
    const bool lava_first = ctx.bundle().rules->lava == contracts::LavaContact::after_move;
    // After the move: fire, lava on 1.21.8, the count, lava on 1.21.4.
    const auto burn = [&](Vec3 at) {
        arrow_in_fire(ctx, at, e.fire, lava_first);
        if (e.fire > 0) --e.fire;
        if (!lava_first && in_lava(ctx, physics::box_at(at, 0.5, 0.5))) e.fire = std::max(e.fire, 300);
    };
    const auto start = e.position;
    const Vec3 to{start.x + e.velocity.x, start.y + e.velocity.y, start.z + e.velocity.z};
    if (const auto hit = clip(ctx, start, to)) {
        const Vec3 at{hit->at.x - sign(e.velocity.x) * inset, hit->at.y - sign(e.velocity.y) * inset, hit->at.z - sign(e.velocity.z) * inset};
        e.velocity = {};
        e.in_ground = true;
        e.life = 0;
        const bool burning = e.fire > 0;
        burn(at);
        moved(ctx, id, start, at);
        if (burning && is_tnt(ctx.block(hit->block))) prime_by_fire(ctx, hit->block);
        target_hit(ctx, hit->block, hit->at, hit->axis, true);
        return;
    }
    const double drag = in_water ? static_cast<double>(0.6f) : static_cast<double>(0.99f);
    e.velocity = {e.velocity.x * drag, e.velocity.y * drag - arrow_gravity, e.velocity.z * drag};
    burn(to);
    // Water it ends the tick in puts it out.
    if (e.fire > 0 && water_at(ctx, physics::box_at(to, 0.5, 0.5)).inside) e.fire = 0;
    moved(ctx, id, start, to);
}
} // namespace

void run_arrow(RuleContext& ctx, EntityId id, Entity& e) {
    step_arrow(ctx, id, e);
    // Where it is now, flying or stuck, it presses plates and wooden buttons.
    if (const auto* arrow = ctx.peek_entity(id)) entity_inside(ctx, *arrow);
}

void run_snowball(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    const bool in_water = water_at(ctx, entity_box(e)).inside;
    const double drag = in_water ? static_cast<double>(0.8f) : static_cast<double>(0.99f);
    e.velocity = {e.velocity.x * drag, (e.velocity.y - snowball_gravity) * drag, e.velocity.z * drag};
    const auto start = e.position;
    const Vec3 to{start.x + e.velocity.x, start.y + e.velocity.y, start.z + e.velocity.z};
    if (const auto hit = clip(ctx, start, to)) {
        moved(ctx, id, start, hit->at);
        target_hit(ctx, hit->block, hit->at, hit->axis, false);
        remove(ctx, id, hit->at, RemovalReason::despawned);
        return;
    }
    moved(ctx, id, start, to);
    if (const auto* ball = ctx.peek_entity(id)) entity_inside(ctx, *ball);
}

} // namespace circuit::gameplay::detail
