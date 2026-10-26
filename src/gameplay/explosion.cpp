// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/physics.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <unordered_set>
#include <vector>

// Explosions, as both official servers run TNT's (checked against their
// traces; minecraft.wiki "Explosion").
//
// Rays leave the center towards the 1,352 points on the surface of a 16x16x16
// grid. Each starts with an intensity of the power times 0.7 to 1.3 (one
// draw of the level random, in single precision) and steps 0.3 blocks at a
// time: a cell that is not empty (its block or fluid) takes (resistance +
// 0.3) * 0.3 from the intensity, a cell reached with intensity left joins
// the explosion (air too), and each step takes 0.22500001 more. The cells
// are visited in the order a Java hash set of them iterates, shuffled by the
// level random. Before that, entities within twice the power are hurt and
// pushed in proportion to their distance and to how much of them the center
// sees; then each cell is cleared in order: TNT becomes primed TNT with a
// short random fuse, other blocks drop their items (merged by kind into
// stacks of at most 16, which fall at the first cell of their kind once
// every cell is cleared) and become air with the usual updates.
namespace circuit::gameplay {
namespace {
// Blast resistance of the registry block a modeled type stands for.
float modeled_resistance(BlockType t) {
    static const auto table = [] {
        std::array<float, 256> out{};
        for (std::size_t i = 1; i < static_cast<std::size_t>(BlockType::count_); ++i) {
            const auto type = static_cast<BlockType>(i);
            if (type == BlockType::other) continue;
            const auto text = block_notation(BlockState::of(type));
            if (!text.starts_with("minecraft:")) continue;
            const auto open = text.find('[');
            const auto name = text.substr(10, open == std::string::npos ? std::string::npos : open - 10);
            if (const auto block = registry::find_block("1.21.8", name)) out[i] = registry::block(*block).resistance;
        }
        return out;
    }();
    return table[static_cast<std::uint8_t>(t)];
}
} // namespace

float blast_resistance(BlockState s) {
    if (s.is(BlockType::other)) {
        const auto block = registry::block_of(s.registry_state());
        float r = block ? registry::block(*block).resistance : 0.0f;
        // A waterlogged block resists as its water does.
        for (const auto& [key, value] : registry::properties(s.registry_state()))
            if (key == "waterlogged" && value == "true") r = std::max(r, 100.0f);
        return r;
    }
    // Kinds of one type may resist differently (iron and oak doors).
    if (has_variant(s.type()))
        if (const auto state = physics::registry_state_of(s))
            if (const auto block = registry::block_of(*state)) return registry::block(*block).resistance;
    return modeled_resistance(s.type());
}
} // namespace circuit::gameplay

namespace circuit::gameplay::detail {
namespace {
using contracts::Vec3;

struct PosHash {
    std::size_t operator()(BlockPos p) const noexcept {
        return std::hash<std::int64_t>{}((static_cast<std::int64_t>(p.x) * 73856093) ^ (static_cast<std::int64_t>(p.y) * 19349663)
                                         ^ (static_cast<std::int64_t>(p.z) * 83492791));
    }
};

bool empty_cell(BlockState s) { return s.is_air(); }

// The cells the rays reach, in the order first reached.
std::vector<BlockPos> reached(RuleContext& ctx, Vec3 center, float power) {
    return ctx.random_draws([&](LevelRandom& random) {
        std::vector<BlockPos> out;
        std::unordered_set<BlockPos, PosHash> seen;
        for (int j = 0; j < 16; ++j)
            for (int k = 0; k < 16; ++k)
                for (int l = 0; l < 16; ++l) {
                    if (j != 0 && j != 15 && k != 0 && k != 15 && l != 0 && l != 15) continue;
                    double dx = j / 15.0 * 2.0 - 1.0, dy = k / 15.0 * 2.0 - 1.0, dz = l / 15.0 * 2.0 - 1.0;
                    const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
                    dx /= length, dy /= length, dz /= length;
                    float intensity = power * (0.7f + random.next_float() * 0.6f);
                    double x = center.x, y = center.y, z = center.z;
                    while (intensity > 0.0f) {
                        const BlockPos p{static_cast<std::int32_t>(std::floor(x)), static_cast<std::int32_t>(std::floor(y)),
                                         static_cast<std::int32_t>(std::floor(z))};
                        if (!ctx.world().contains_y(p.y)) break;
                        const auto s = ctx.block(p);
                        if (s.is_unloaded()) break;
                        if (!empty_cell(s)) intensity -= (blast_resistance(s) + 0.3f) * 0.3f;
                        if (intensity > 0.0f && seen.insert(p).second) out.push_back(p);
                        x += dx * 0.30000001192092896;
                        y += dy * 0.30000001192092896;
                        z += dz * 0.30000001192092896;
                        intensity -= 0.22500001f;
                    }
                }
        return out;
    });
}

// Whether the segment from `a` to `b` passes through the inside of a box
// (touching a face, or starting on one and leaving, does not count).
bool crosses(Vec3 a, Vec3 b, const physics::Aabb& box) {
    double t0 = 0.0, t1 = 1.0;
    const std::array<std::array<double, 4>, 3> axes{{{a.x, b.x - a.x, box.min_x, box.max_x},
                                                     {a.y, b.y - a.y, box.min_y, box.max_y},
                                                     {a.z, b.z - a.z, box.min_z, box.max_z}}};
    for (const auto& [start, delta, low, high] : axes) {
        if (delta == 0.0) {
            if (start <= low || start >= high) return false;
            continue;
        }
        double ta = (low - start) / delta, tb = (high - start) / delta;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 >= t1) return false;
    }
    return true;
}

// Whether a block's collision shape stands between `from` and `to`: the
// cells the segment passes through, each with its block's whole shape.
bool obstructed(RuleContext& ctx, Vec3 from, Vec3 to) {
    const auto& release = ctx.bundle().schema.content_release;
    const Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
    std::array<std::int32_t, 3> cell{static_cast<std::int32_t>(std::floor(from.x)), static_cast<std::int32_t>(std::floor(from.y)),
                                     static_cast<std::int32_t>(std::floor(from.z))};
    const std::array<std::int32_t, 3> last{static_cast<std::int32_t>(std::floor(to.x)), static_cast<std::int32_t>(std::floor(to.y)),
                                           static_cast<std::int32_t>(std::floor(to.z))};
    const std::array<double, 3> start{from.x, from.y, from.z}, delta{d.x, d.y, d.z};
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
        if (ctx.world().contains_y(p.y)) {
            const auto s = ctx.block(p);
            if (!s.is_unloaded() && !s.is_air())
                for (const auto& b : physics::collision_boxes(s, release)) {
                    const physics::Aabb box{p.x + b.min_x, p.y + b.min_y, p.z + b.min_z, p.x + b.max_x, p.y + b.max_y, p.z + b.max_z};
                    if (crosses(from, to, box)) return true;
                }
        }
        if (cell == last) return false;
        const int axis = t_max[0] <= t_max[1] ? (t_max[0] <= t_max[2] ? 0 : 2) : (t_max[1] <= t_max[2] ? 1 : 2);
        if (t_max[axis] > 1.0) return false;
        cell[axis] += step[axis];
        t_max[axis] += t_delta[axis];
    }
    return false;
}

// The share of sample points in an entity's box that the center sees
// (minecraft.wiki "Explosion": a grid of about 2 * size + 1 points per
// axis, offset on the horizontal axes).
float exposure(RuleContext& ctx, Vec3 center, const physics::Aabb& box) {
    const double dx = 1.0 / ((box.max_x - box.min_x) * 2.0 + 1.0);
    const double dy = 1.0 / ((box.max_y - box.min_y) * 2.0 + 1.0);
    const double dz = 1.0 / ((box.max_z - box.min_z) * 2.0 + 1.0);
    const double ox = (1.0 - std::floor(1.0 / dx) * dx) / 2.0;
    const double oz = (1.0 - std::floor(1.0 / dz) * dz) / 2.0;
    int seen = 0, total = 0;
    for (double a = 0.0; a <= 1.0; a += dx)
        for (double b = 0.0; b <= 1.0; b += dy)
            for (double c = 0.0; c <= 1.0; c += dz) {
                const Vec3 p{box.min_x + a * (box.max_x - box.min_x) + ox, box.min_y + b * (box.max_y - box.min_y),
                             box.min_z + c * (box.max_z - box.min_z) + oz};
                if (!obstructed(ctx, p, center)) ++seen;
                ++total;
            }
    return total == 0 ? 0.0f : static_cast<float>(seen) / static_cast<float>(total);
}

// The height of an entity's eyes above its feet.
double eye_height(const Entity& e) {
    const auto box = entity_box(e);
    const auto height = static_cast<float>(box.max_y - box.min_y);
    return static_cast<double>(height * 0.85f);
}

// Entities within twice the power: hurt and pushed.
void hurt_entities(RuleContext& ctx, Vec3 center, float power) {
    const double reach = static_cast<double>(power) * 2.0;
    const physics::Aabb area{std::floor(center.x - reach - 1.0), std::floor(center.y - reach - 1.0), std::floor(center.z - reach - 1.0),
                             std::floor(center.x + reach + 1.0), std::floor(center.y + reach + 1.0), std::floor(center.z + reach + 1.0)};
    for (const auto id : entities_in_section_order(ctx, entities_intersecting(ctx, area))) {
        auto* e = ctx.entity(id);
        if (!e) continue;
        const Vec3 at = e->position;
        const double distance = std::sqrt((at.x - center.x) * (at.x - center.x) + (at.y - center.y) * (at.y - center.y)
                                          + (at.z - center.z) * (at.z - center.z))
                              / reach;
        if (distance > 1.0) continue;
        // Primed TNT is pushed away from its feet, everything else from its eyes.
        Vec3 direction{at.x - center.x, (e->kind == EntityKind::primed_tnt ? at.y : at.y + eye_height(*e)) - center.y, at.z - center.z};
        const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (length == 0.0) continue;
        direction = {direction.x / length, direction.y / length, direction.z / length};
        const double impact = (1.0 - distance) * static_cast<double>(exposure(ctx, center, entity_box(*e)));
        const auto damage = static_cast<float>((impact * impact + impact) / 2.0 * 7.0 * reach + 1.0);
        if ((e->kind == EntityKind::item && e->stack.item != items::nether_star) || e->kind == EntityKind::experience_orb) {
            // Items and experience orbs have 5 health, kept in whole points;
            // the explosion destroys one left with none.
            e->health = static_cast<std::int32_t>(static_cast<float>(e->health) - damage);
            if (e->health <= 0) {
                ctx.record(TraceKind::entity_removed, contracts::block_of(at), id.value, static_cast<std::uint64_t>(RemovalReason::exploded));
                ctx.remove_entity(id);
                continue;
            }
        }
        e->velocity = {e->velocity.x + direction.x * impact, e->velocity.y + direction.y * impact, e->velocity.z + direction.z * impact};
    }
}

// A stack an explosion drops, merged with earlier ones of its kind.
struct Drop {
    ItemStack stack;
    BlockPos pos;
};
void add_drop(std::vector<Drop>& drops, ItemStack stack, BlockPos pos) {
    constexpr std::uint8_t limit = 16;
    for (auto& d : drops) {
        if (stack.empty()) return;
        if (d.stack.item != stack.item || d.stack.damage != stack.damage || d.stack.components != stack.components
            || items::max_stack(stack) <= 1 || d.stack.count >= limit)
            continue;
        const auto moved = static_cast<std::uint8_t>(std::min<int>(stack.count, limit - d.stack.count));
        d.stack.count = static_cast<std::uint8_t>(d.stack.count + moved);
        stack.count = static_cast<std::uint8_t>(stack.count - moved);
    }
    if (!stack.empty()) drops.push_back({std::move(stack), pos});
}
} // namespace

void explode(RuleContext& ctx, Vec3 center, float power) {
    ctx.record(TraceKind::explosion, contracts::block_of(center), contracts::double_bits(center.x), contracts::double_bits(center.y),
               contracts::double_bits(center.z));
    auto cells = reached(ctx, center, power);
    hurt_entities(ctx, center, power);
    cells = hash_order(cells, cells.size());
    ctx.random_draws([&](LevelRandom& random) {
        for (auto i = static_cast<std::int32_t>(cells.size()); i > 1; --i)
            std::swap(cells[static_cast<std::size_t>(i - 1)], cells[static_cast<std::size_t>(random.next_int(i))]);
        return 0;
    });
    std::vector<Drop> drops;
    for (const auto p : cells) {
        const auto s = ctx.block(p);
        if (s.is_air() || s.is_unloaded()) continue;
        if (is_tnt(s)) {
            set_block(ctx, p, blocks::air());
            prime_by_explosion(ctx, p);
            continue;
        }
        // Stacks of none are not compared yet in explosions: they drop nothing.
        // Primed TNT is the explosion's entity: snow drops its snowballs (snow_explosion).
        for (auto& stack : block_drops(ctx, p, s, {}, true, true))
            if (stack.count > 0) add_drop(drops, std::move(stack), p);
        set_block(ctx, p, blocks::air());
    }
    for (auto& d : drops) pop_resource(ctx, d.pos, std::move(d.stack));
}

} // namespace circuit::gameplay::detail
