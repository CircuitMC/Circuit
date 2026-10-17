// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/physics.hpp>
#include <circuit/gameplay/survival.hpp>
#include <algorithm>
#include <cmath>

// Minecarts off rails and boats without riders, as both official servers
// move them (checked against their traces; minecraft.wiki "Minecart",
// "Boat").
//
// A minecart falls by 0.04 (0.005 in water), keeps its horizontal speed
// within 0.4 a tick (0.2 in water), loses half its velocity if it stood on
// the ground, moves, and loses 5% of its velocity unless it lands. Water
// counts as the minecart's last tick ended (not in its first tick).
//
// A boat is pushed by currents, then it is in one of four states, judged
// from where it is: under water (its top below the surface), in water (its
// bottom below the surface of the water under it), on land (blocks under
// it), or in the air. A boat that comes out of the air into water stands on
// the surface (0.101 above the water's top less its height) without
// vertical speed. Otherwise it falls by 0.04 and keeps 90% of its
// horizontal speed (45% under water; on land the average slipperiness of
// the blocks under it); in water it rises in proportion to how deep it
// lies (4/65 a tick for each of its heights) and keeps 75% of its vertical
// speed; under water it rises by 0.01 and keeps 75%. Then it moves.
namespace circuit::gameplay::detail {
namespace {
using contracts::Vec3;
constexpr double boat_height = 0.5625;

double cell(double v) { return std::floor(v); }

bool is_water(BlockState s) { return s.is(BlockType::water); }

// The top of the water at a cell, in single precision as both official
// servers add it: the cell's height plus its water's.
float water_top(RuleContext& ctx, BlockPos p, BlockState s) {
    return static_cast<float>(p.y) + fluid_height(s, ctx.block(p.relative(Direction::up)));
}

enum class BoatStatus : std::uint8_t { in_water = 1, under_water = 2, under_flowing_water = 3, on_land = 4, in_air = 5 };

// The average slipperiness of the blocks whose shape meets the layer just
// under the box; 0 when there are none.
float ground_friction(RuleContext& ctx, const physics::Aabb& box) {
    const physics::Aabb under{box.min_x, box.min_y - 0.001, box.min_z, box.max_x, box.min_y, box.max_z};
    const auto& release = ctx.bundle().schema.content_release;
    float sum = 0.0f;
    int count = 0;
    for (auto x = static_cast<std::int32_t>(cell(under.min_x)); x <= static_cast<std::int32_t>(cell(under.max_x)); ++x)
        for (auto z = static_cast<std::int32_t>(cell(under.min_z)); z <= static_cast<std::int32_t>(cell(under.max_z)); ++z)
            for (auto y = static_cast<std::int32_t>(cell(under.min_y)); y <= static_cast<std::int32_t>(cell(under.max_y)); ++y) {
                if (!ctx.world().contains_y(y)) continue;
                const auto s = ctx.block({x, y, z});
                if (s.is_unloaded()) continue;
                bool meets = false;
                for (const auto& b : physics::collision_boxes(s, release))
                    if (physics::Aabb{x + b.min_x, y + b.min_y, z + b.min_z, x + b.max_x, y + b.max_y, z + b.max_z}.intersects(under)) meets = true;
                if (!meets) continue;
                sum += slipperiness(s);
                ++count;
            }
    return count == 0 ? 0.0f : sum / static_cast<float>(count);
}

// The surface of the water the box's bottom is in, if it is below it.
std::optional<float> water_level(RuleContext& ctx, const physics::Aabb& box) {
    std::optional<float> level;
    const auto y0 = static_cast<std::int32_t>(cell(box.min_y));
    const auto y1 = static_cast<std::int32_t>(std::ceil(box.min_y + 0.001));
    for (auto x = static_cast<std::int32_t>(cell(box.min_x)); x < static_cast<std::int32_t>(std::ceil(box.max_x)); ++x)
        for (auto z = static_cast<std::int32_t>(cell(box.min_z)); z < static_cast<std::int32_t>(std::ceil(box.max_z)); ++z)
            for (auto y = y0; y < y1; ++y) {
                if (!ctx.world().contains_y(y)) continue;
                const BlockPos p{x, y, z};
                const auto s = ctx.block(p);
                if (!is_water(s)) continue;
                const auto top = water_top(ctx, p, s);
                level = std::max(level.value_or(top), top);
            }
    if (level && box.min_y < static_cast<double>(*level)) return level;
    return std::nullopt;
}

// Whether water covers the box's top: under water (flowing water if any of
// it flows).
std::optional<BoatStatus> submerged(RuleContext& ctx, const physics::Aabb& box) {
    const double top = box.max_y + 0.001;
    const auto y0 = static_cast<std::int32_t>(cell(box.max_y));
    const auto y1 = static_cast<std::int32_t>(std::ceil(top));
    std::optional<BoatStatus> out;
    for (auto x = static_cast<std::int32_t>(cell(box.min_x)); x < static_cast<std::int32_t>(std::ceil(box.max_x)); ++x)
        for (auto z = static_cast<std::int32_t>(cell(box.min_z)); z < static_cast<std::int32_t>(std::ceil(box.max_z)); ++z)
            for (auto y = y0; y < y1; ++y) {
                if (!ctx.world().contains_y(y)) continue;
                const BlockPos p{x, y, z};
                const auto s = ctx.block(p);
                if (!is_water(s) || top >= static_cast<double>(water_top(ctx, p, s))) continue;
                if (s.level() != 0 || s.flag()) return BoatStatus::under_flowing_water;
                out = BoatStatus::under_water;
            }
    return out;
}

// The surface above the box: from the layer of its top, the first layer
// not full of water, at its water's height.
double surface_above(RuleContext& ctx, const physics::Aabb& box, double last_dy) {
    const auto y0 = static_cast<std::int32_t>(cell(box.max_y));
    const auto y1 = static_cast<std::int32_t>(std::ceil(box.max_y - last_dy));
    for (auto y = y0; y < y1; ++y) {
        float most = 0.0f;
        bool full = false;
        for (auto x = static_cast<std::int32_t>(cell(box.min_x)); x < static_cast<std::int32_t>(std::ceil(box.max_x)) && !full; ++x)
            for (auto z = static_cast<std::int32_t>(cell(box.min_z)); z < static_cast<std::int32_t>(std::ceil(box.max_z)) && !full; ++z) {
                const BlockPos p{x, y, z};
                if (!ctx.world().contains_y(y)) continue;
                const auto s = ctx.block(p);
                if (!is_water(s)) continue;
                most = std::max(most, fluid_height(s, ctx.block(p.relative(Direction::up))));
                full = most >= 1.0f;
            }
        if (!full) return static_cast<double>(static_cast<float>(y) + most);
    }
    return static_cast<double>(static_cast<float>(y1 + 1));
}

// A boat pushes the boats and minecarts its box (0.2 wider on each side,
// 0.01 lower at both ends) meets apart: each takes 0.05 (in single
// precision) along the horizontal direction between them, scaled down by
// the square root of their distance when it is beyond one block (the
// larger of the two axes' distances; none below 0.01). Another boat only
// when its bottom is below this one's top, a minecart only when its bottom
// is no higher than this one's.
void push_apart(RuleContext& ctx, EntityId id) {
    const auto* self = ctx.peek_entity(id);
    const auto box = entity_box(*self);
    const physics::Aabb reach{box.min_x - 0.2, box.min_y + 0.01, box.min_z - 0.2, box.max_x + 0.2, box.max_y - 0.01, box.max_z + 0.2};
    for (const auto other_id : entities_in_section_order(ctx, entities_intersecting(ctx, reach))) {
        if (other_id == id) continue;
        const auto* other = ctx.peek_entity(other_id);
        if (!other || (other->kind != EntityKind::boat && other->kind != EntityKind::minecart)) continue;
        const auto theirs = entity_box(*other);
        if (other->kind == EntityKind::boat ? !(theirs.min_y < box.max_y) : !(theirs.min_y <= box.min_y)) continue;
        double dx = other->position.x - self->position.x, dz = other->position.z - self->position.z;
        double d = std::max(std::abs(dx), std::abs(dz));
        if (d < static_cast<double>(0.01f)) continue;
        d = std::sqrt(d);
        dx /= d;
        dz /= d;
        const double scale = std::min(1.0, 1.0 / d);
        dx = dx * scale * static_cast<double>(0.05f);
        dz = dz * scale * static_cast<double>(0.05f);
        auto* me = ctx.entity(id);
        me->velocity = {me->velocity.x - dx, me->velocity.y, me->velocity.z - dz};
        auto* them = ctx.entity(other_id);
        them->velocity = {them->velocity.x + dx, them->velocity.y, them->velocity.z + dz};
        self = ctx.peek_entity(id);
    }
}

void relocate(RuleContext& ctx, EntityId id, Vec3 start, Vec3 to) {
    if (to == start) return;
    const auto from = contracts::chunk_of(contracts::block_of(start));
    const auto chunk = contracts::chunk_of(contracts::block_of(to));
    if (from != chunk)
        ctx.record(TraceKind::entity_changed_chunk, contracts::block_of(to), id.value, contracts::pack_chunk(from), contracts::pack_chunk(chunk));
    ctx.relocate_entity(id, to);
}
} // namespace

void run_minecart(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    // What water held it at the end of its last tick.
    const bool in_water = e.in_water;
    auto v = e.velocity;
    v.y -= in_water ? 0.005 : 0.04;
    const double limit = in_water ? 0.2 : 0.4;
    v.x = std::clamp(v.x, -limit, limit);
    v.z = std::clamp(v.z, -limit, limit);
    if (e.on_ground) v = {v.x * 0.5, v.y * 0.5, v.z * 0.5};
    const auto start = e.position;
    auto p = start;
    e.on_ground = move_entity(ctx, p, v, static_cast<double>(0.98f), static_cast<double>(0.7f));
    if (!e.on_ground) v = {v.x * 0.95, v.y * 0.95, v.z * 0.95};
    e.velocity = v;
    e.in_water = water_at(ctx, physics::box_at(p, static_cast<double>(0.98f), static_cast<double>(0.7f))).inside;
    relocate(ctx, id, start, p);
}

void run_boat(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    const auto box = entity_box(e);
    auto v = e.velocity;
    if (const auto water = water_at(ctx, box); water.inside) v = {v.x + water.push.x, v.y + water.push.y, v.z + water.push.z};
    BoatStatus status = BoatStatus::in_air;
    std::optional<float> level;
    float land = 0.0f;
    if (const auto under = submerged(ctx, box)) {
        status = *under;
    } else if ((level = water_level(ctx, box))) {
        status = BoatStatus::in_water;
    } else if ((land = ground_friction(ctx, box)) > 0.0f) {
        status = BoatStatus::on_land;
    }
    const auto start = e.position;
    auto p = start;
    // A boat just summoned or loaded has no state before (0).
    if (e.status == static_cast<std::uint8_t>(BoatStatus::in_air) && status != BoatStatus::in_air && status != BoatStatus::on_land) {
        // Out of the air into water: it stands on the surface.
        p.y = surface_above(ctx, box, e.last_dy) - boat_height + 0.101;
        v.y = 0.0;
        e.last_dy = 0.0;
        status = BoatStatus::in_water;
    } else {
        double gravity = -0.04, buoyancy = 0.0, friction = static_cast<double>(0.9f);
        switch (status) {
        case BoatStatus::in_water:
            buoyancy = (static_cast<double>(*level) - p.y) / boat_height;
            break;
        case BoatStatus::under_flowing_water: gravity = -7.0e-4; break;
        case BoatStatus::under_water: buoyancy = static_cast<double>(0.01f); friction = static_cast<double>(0.45f); break;
        case BoatStatus::in_air: break;
        case BoatStatus::on_land: friction = static_cast<double>(land); break;
        }
        v = {v.x * friction, v.y + gravity, v.z * friction};
        if (buoyancy > 0.0) v.y = (v.y + buoyancy * 0.06153846153846154) * 0.75;
    }
    const auto before = p;
    e.on_ground = move_entity(ctx, p, v, 1.375, boat_height);
    e.last_dy = p.y - before.y;
    e.velocity = v;
    e.status = static_cast<std::uint8_t>(status);
    relocate(ctx, id, start, p);
    push_apart(ctx, id);
}

} // namespace circuit::gameplay::detail
