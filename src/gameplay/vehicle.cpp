// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/physics.hpp>
#include <circuit/gameplay/sky.hpp>
#include <circuit/gameplay/survival.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>

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

namespace {
using blocks::RailShape;

// The two ends of a rail shape, as offsets from its cell (a raised end a
// block up is -1 at the other end: the shape's low end lies a block down
// from its high one, seen from the cell above).
struct Exits {
    std::array<int, 3> a, b;
};
Exits exits(RailShape r) {
    switch (r) {
    case RailShape::north_south: return {{0, 0, -1}, {0, 0, 1}};
    case RailShape::east_west: return {{-1, 0, 0}, {1, 0, 0}};
    case RailShape::ascending_east: return {{-1, -1, 0}, {1, 0, 0}};
    case RailShape::ascending_west: return {{-1, 0, 0}, {1, -1, 0}};
    case RailShape::ascending_north: return {{0, 0, -1}, {0, -1, 1}};
    case RailShape::ascending_south: return {{0, -1, -1}, {0, 0, 1}};
    case RailShape::south_east: return {{0, 0, 1}, {1, 0, 0}};
    case RailShape::south_west: return {{0, 0, 1}, {-1, 0, 0}};
    case RailShape::north_west: return {{0, 0, -1}, {-1, 0, 0}};
    case RailShape::north_east: return {{0, 0, -1}, {1, 0, 0}};
    }
    return {{0, 0, -1}, {0, 0, 1}};
}

double horizontal(Vec3 v) { return std::sqrt(v.x * v.x + v.z * v.z); }

// The rail at a position: in its cell or the cell below.
std::optional<BlockPos> rail_under(RuleContext& ctx, Vec3 p) {
    BlockPos cell_pos{static_cast<std::int32_t>(cell(p.x)), static_cast<std::int32_t>(cell(p.y)), static_cast<std::int32_t>(cell(p.z))};
    if (ctx.block(cell_pos.relative(Direction::down)).is(BlockType::rail)) cell_pos = cell_pos.relative(Direction::down);
    return ctx.block(cell_pos).is(BlockType::rail) ? std::optional(cell_pos) : std::nullopt;
}

// The point of the rail under a position nearest it along the rail, a
// sixteenth above the rail's cell (raised along a slope).
std::optional<Vec3> on_track(RuleContext& ctx, Vec3 p) {
    const auto pos = rail_under(ctx, p);
    if (!pos) return std::nullopt;
    const auto [a, b] = exits(static_cast<RailShape>(ctx.block(*pos).level()));
    const double x0 = pos->x + 0.5 + a[0] * 0.5, y0 = pos->y + 0.0625 + a[1] * 0.5, z0 = pos->z + 0.5 + a[2] * 0.5;
    const double x1 = pos->x + 0.5 + b[0] * 0.5, y1 = pos->y + 0.0625 + b[1] * 0.5, z1 = pos->z + 0.5 + b[2] * 0.5;
    const double dx = x1 - x0, dy = (y1 - y0) * 2.0, dz = z1 - z0;
    double t;
    if (dx == 0.0) t = p.z - pos->z;
    else if (dz == 0.0) t = p.x - pos->x;
    else t = ((p.x - x0) * dx + (p.z - z0) * dz) * 2.0;
    Vec3 out{x0 + dx * t, y0 + dy * t, z0 + dz * t};
    if (dy < 0.0) out.y += 1.0;
    else if (dy > 0.0) out.y += 0.5;
    return out;
}

// One tick along a rail at `pos`: the minecart follows the rail's line,
// gains speed down a slope, brakes on an unpowered powered rail and is
// pushed along by a powered one.
void along_rail(RuleContext& ctx, Entity& e, Vec3& v, Vec3& p, BlockPos pos, BlockState rail, bool in_water) {
    const auto start = on_track(ctx, p);
    const auto shape = static_cast<RailShape>(rail.level());
    const bool powered_kind = rail.variant() == static_cast<std::uint8_t>(blocks::RailKind::powered);
    const bool boost = powered_kind && rail.flag(), brake = powered_kind && !rail.flag();
    double y = pos.y;
    const double slope = in_water ? 0.0078125 * 0.2 : 0.0078125;
    switch (shape) {
    case RailShape::ascending_east: v.x -= slope; y += 1.0; break;
    case RailShape::ascending_west: v.x += slope; y += 1.0; break;
    case RailShape::ascending_north: v.z += slope; y += 1.0; break;
    case RailShape::ascending_south: v.z -= slope; y += 1.0; break;
    default: break;
    }
    const auto [a, b] = exits(shape);
    double ex = b[0] - a[0], ez = b[2] - a[2];
    const double length = std::sqrt(ex * ex + ez * ez);
    if (v.x * ex + v.z * ez < 0.0) ex = -ex, ez = -ez;
    const double speed = std::min(2.0, horizontal(v));
    v = {speed * ex / length, v.y, speed * ez / length};
    if (brake) {
        if (horizontal(v) < 0.03) v = {};
        else v = {v.x * 0.5, v.y * 0.0, v.z * 0.5};
    }
    const double x0 = pos.x + 0.5 + a[0] * 0.5, z0 = pos.z + 0.5 + a[2] * 0.5;
    const double x1 = pos.x + 0.5 + b[0] * 0.5, z1 = pos.z + 0.5 + b[2] * 0.5;
    const double dx = x1 - x0, dz = z1 - z0;
    double t;
    if (dx == 0.0) t = p.z - pos.z;
    else if (dz == 0.0) t = p.x - pos.x;
    else t = ((p.x - x0) * dx + (p.z - z0) * dz) * 2.0;
    p = {x0 + dx * t, y, z0 + dz * t};
    const double limit = in_water ? 0.2 : 0.4;
    Vec3 step{std::clamp(v.x, -limit, limit), 0.0, std::clamp(v.z, -limit, limit)};
    Vec3 moved{};
    e.on_ground = move_entity(ctx, p, v, step, static_cast<double>(0.98f), static_cast<double>(0.7f), &e, &moved);
    entity_fell(ctx, e, p, moved, e.on_ground);
    // Reaching a raised end's cell, it climbs to it.
    const auto at_end = [&](const std::array<int, 3>& end) {
        return end[1] != 0 && static_cast<std::int32_t>(cell(p.x)) - pos.x == end[0] && static_cast<std::int32_t>(cell(p.z)) - pos.z == end[2];
    };
    if (at_end(a)) p.y += a[1];
    else if (at_end(b)) p.y += b[1];
    // Slowdown, then the speed a change of height gives or takes.
    v = {v.x * 0.96, v.y * 0.0, v.z * 0.96};
    if (in_water) v = {v.x * 0.95, v.y * 0.95, v.z * 0.95};
    if (const auto end = on_track(ctx, p); end && start) {
        const double gain = (start->y - end->y) * 0.05;
        const double h = horizontal(v);
        if (h > 0.0) v = {v.x * ((h + gain) / h), v.y, v.z * ((h + gain) / h)};
        p.y = end->y;
    }
    // Into another cell, the speed turns toward it.
    const auto cx = static_cast<std::int32_t>(cell(p.x)), cz = static_cast<std::int32_t>(cell(p.z));
    if (cx != pos.x || cz != pos.z) {
        const double h = horizontal(v);
        v = {h * (cx - pos.x), v.y, h * (cz - pos.z)};
    }
    if (!boost) return;
    const double h = horizontal(v);
    if (h > 0.01) {
        v = {v.x + v.x / h * 0.06, v.y, v.z + v.z / h * 0.06};
        return;
    }
    // From rest a powered rail pushes away from a solid block at one end.
    if (shape == RailShape::east_west) {
        if (is_conductor(ctx.block(pos.relative(Direction::west)))) v.x = 0.02;
        else if (is_conductor(ctx.block(pos.relative(Direction::east)))) v.x = -0.02;
    } else if (shape == RailShape::north_south) {
        if (is_conductor(ctx.block(pos.relative(Direction::north)))) v.z = 0.02;
        else if (is_conductor(ctx.block(pos.relative(Direction::south)))) v.z = -0.02;
    }
}
} // namespace

namespace {
// Java's float remainder keeps the dividend's sign, as std::fmod does.
float wrap_degrees(float f) {
    float r = std::fmod(f, 360.0f);
    if (r >= 180.0f) r -= 360.0f;
    if (r < -180.0f) r += 360.0f;
    return r;
}

Vec3 normalized(double x, double z) {
    const double d = std::sqrt(x * x + z * z);
    return d < 1.0e-5 ? Vec3{} : Vec3{x / d, 0.0, z / d};
}

// Minecart `pushed` meets minecart `pusher`: when the direction to the
// pusher lies within about 37 degrees of the pushed one's yaw, both keep a
// fifth of their velocity and take the average of the two, pushed apart by
// 0.05 along the line between them (less beyond a block apart).
void push_minecart(RuleContext& ctx, EntityId pushed, EntityId pusher) {
    const auto* b = ctx.peek_entity(pushed);
    const auto* a = ctx.peek_entity(pusher);
    if (!a || !b) return;
    double dx = a->position.x - b->position.x, dz = a->position.z - b->position.z;
    double d = dx * dx + dz * dz;
    if (d < static_cast<double>(1.0e-4f)) return;
    d = std::sqrt(d);
    dx /= d;
    dz /= d;
    const double scale = std::min(1.0 / d, 1.0);
    dx = dx * scale * static_cast<double>(0.1f) * 0.5;
    dz = dz * scale * static_cast<double>(0.1f) * 0.5;
    const auto toward = normalized(a->position.x - b->position.x, a->position.z - b->position.z);
    const float radians = b->yaw * static_cast<float>(std::numbers::pi / 180.0);
    const auto facing = normalized(static_cast<double>(table_cos(radians)), static_cast<double>(table_sin(radians)));
    if (std::abs(toward.x * facing.x + toward.z * facing.z) < static_cast<double>(0.8f)) return;
    const auto vb = b->velocity, va = a->velocity;
    const double ax = (va.x + vb.x) / 2.0, az = (va.z + vb.z) / 2.0;
    auto* bw = ctx.entity(pushed);
    bw->velocity = {vb.x * 0.2 + (ax - dx), vb.y, vb.z * 0.2 + (az - dz)};
    auto* aw = ctx.entity(pusher);
    aw->velocity = {va.x * 0.2 + (ax + dx), va.y, va.z * 0.2 + (az + dz)};
}

// After its move a minecart turns its yaw toward where it came from (half a
// turn more when flipped) and flips when the yaw would swing by 170 degrees
// or more. Then it pushes the minecarts its box, 0.2 wider sideways, meets;
// moving faster than 0.1 it would pick up the boats there, which is not
// modeled (riding).
void after_move(RuleContext& ctx, EntityId id, Vec3 start, float yaw_before) {
    auto* e = ctx.entity(id);
    const double dx = start.x - e->position.x, dz = start.z - e->position.z;
    if (dx * dx + dz * dz > 0.001) {
        e->yaw = static_cast<float>(std::atan2(dz, dx) * 180.0 / std::numbers::pi);
        if (e->flipped) e->yaw += 180.0f;
    }
    const double swing = wrap_degrees(e->yaw - yaw_before);
    if (swing < -170.0 || swing >= 170.0) {
        e->yaw += 180.0f;
        e->flipped = !e->flipped;
    }
    e->yaw = std::fmod(e->yaw, 360.0f);
    const auto reach = entity_box(*e).inflate(0.2, 0.0, 0.2);
    for (const auto other : entities_in_section_order(ctx, entities_intersecting(ctx, reach))) {
        if (other == id) continue;
        const auto* o = ctx.peek_entity(other);
        if (o && o->kind == EntityKind::minecart) push_minecart(ctx, other, id);
    }
}
} // namespace

void run_minecart(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    // What water held it at the end of its last tick.
    const bool in_water = e.in_water;
    auto v = e.velocity;
    v.y -= in_water ? 0.005 : 0.04;
    const auto start = e.position;
    auto p = start;
    if (const auto pos = rail_under(ctx, p)) {
        along_rail(ctx, e, v, p, *pos, ctx.block(*pos), in_water);
    } else {
        const double limit = in_water ? 0.2 : 0.4;
        v.x = std::clamp(v.x, -limit, limit);
        v.z = std::clamp(v.z, -limit, limit);
        if (e.on_ground) v = {v.x * 0.5, v.y * 0.5, v.z * 0.5};
        Vec3 moved{};
        e.on_ground = move_entity(ctx, p, v, static_cast<double>(0.98f), static_cast<double>(0.7f), &e, &moved);
        entity_fell(ctx, e, p, moved, e.on_ground);
        if (!e.on_ground) v = {v.x * 0.95, v.y * 0.95, v.z * 0.95};
        // A minecart that landed on a slime block loses horizontal speed
        // there once more than other entities do (both official servers;
        // slime_landing).
        const auto under = contracts::block_of(Vec3{p.x, p.y - 0.2, p.z});
        if (e.on_ground && std::abs(v.y) < 0.1 && ctx.world().contains_y(under.y) && ctx.block(under).is(BlockType::slime_block)) {
            const double keep = 0.4 + std::abs(v.y) * 0.2;
            v = {v.x * keep, v.y, v.z * keep};
        }
    }
    e.velocity = v;
    e.in_water = water_at(ctx, physics::box_at(p, static_cast<double>(0.98f), static_cast<double>(0.7f))).inside;
    const float yaw_before = e.yaw;
    relocate(ctx, id, start, p);
    after_move(ctx, id, start, yaw_before);
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
    e.on_ground = move_entity(ctx, p, v, 1.375, boat_height, &e);
    e.last_dy = p.y - before.y;
    e.velocity = v;
    e.status = static_cast<std::uint8_t>(status);
    relocate(ctx, id, start, p);
    push_apart(ctx, id);
}

} // namespace circuit::gameplay::detail
