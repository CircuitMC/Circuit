// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <map>

namespace circuit::gameplay::detail {
namespace {
// Water as the target version describes it in amounts: a source or falling
// water has 8, flowing water at distance d has 8 - d. Observed in
// water_channel, water_drop and water_washes.
constexpr std::array<Direction, 4> plane{Direction::north, Direction::east, Direction::south, Direction::west};
constexpr int slope_find_distance = 4;

bool is_source(BlockState s) { return s.is(BlockType::water) && s.level() == 0 && !s.flag(); }
int amount(BlockState s) {
    if (!s.is(BlockType::water)) return 0;
    return s.level() == 0 ? 8 : 8 - s.level(); // sources and falling water: 8
}
// Blocks flowing water replaces, dropping their items.
bool washed_away(BlockType t) {
    return t == BlockType::redstone_wire || t == BlockType::lever || t == BlockType::repeater || t == BlockType::comparator
        || t == BlockType::wheat;
}
bool holds_water(BlockState s) { return s.is_air() || s.is(BlockType::water) || washed_away(s.type()); }
// Water may flow into the block: it holds water and is no source.
bool passable(BlockState s) { return holds_water(s) && !is_source(s); }
// Only cells without water take new flowing water from a side; flowing water
// changes through its own ticks.
bool replaceable_by_flow(BlockState s) { return !s.is(BlockType::water); }

// Water a cell would hold from its neighbors: a source between two sources on
// solid ground or above a source, falling water below water, else one less
// than its strongest neighbor.
BlockState new_liquid(RuleContext& ctx, BlockPos pos) {
    int best = 0, sources = 0;
    for (const auto d : plane) {
        const auto n = ctx.block(pos.relative(d));
        if (!n.is(BlockType::water)) continue;
        if (is_source(n)) ++sources;
        best = std::max(best, amount(n));
    }
    if (sources >= 2) {
        const auto below = ctx.block(pos.relative(Direction::down));
        if (is_solid(below) || is_source(below)) return blocks::water_source();
    }
    if (ctx.block(pos.relative(Direction::up)).is(BlockType::water)) return blocks::water_falling();
    const int k = best - 1;
    if (k <= 0) return blocks::air();
    return blocks::water_flowing(static_cast<std::uint8_t>(8 - k));
}

// Water could flow down from `pos`.
bool is_hole(RuleContext& ctx, BlockPos pos) { return holds_water(ctx.block(pos.relative(Direction::down))); }

// Steps from `pos` to the nearest hole within reach, not going back the way it came.
int slope_distance(RuleContext& ctx, BlockPos pos, int depth, Direction from) {
    int best = 1000;
    for (const auto d : plane) {
        if (d == from) continue;
        const auto next = pos.relative(d);
        if (!passable(ctx.block(next))) continue;
        if (is_hole(ctx, next)) return depth;
        if (depth >= slope_find_distance) continue;
        best = std::min(best, slope_distance(ctx, next, depth + 1, contracts::opposite(d)));
    }
    return best;
}

void spread_to(RuleContext& ctx, BlockPos pos, BlockState water) {
    const auto there = ctx.block(pos);
    if (washed_away(there.type())) drop_items(ctx, pos, there);
    set_block(ctx, pos, water);
}

// Sides water spreads to: toward the nearest holes if any is in reach, else all.
// Spreads in the order north, south, west, east.
void spread_to_sides(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!s.flag() && amount(s) - 1 <= 0) return;
    int best = 1000;
    std::map<Direction, BlockState> out; // Direction order: north, south, west, east
    for (const auto d : plane) {
        const auto p = pos.relative(d);
        const auto there = ctx.block(p);
        if (!passable(there)) continue;
        const auto fresh = new_liquid(ctx, p);
        const int j = is_hole(ctx, p) ? 0 : slope_distance(ctx, p, 1, contracts::opposite(d));
        if (j < best) out.clear();
        if (j <= best) {
            if (replaceable_by_flow(there)) out[d] = fresh;
            best = j;
        }
    }
    for (const auto& [d, water] : out) spread_to(ctx, pos.relative(d), water);
}

void spread(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto below = pos.relative(Direction::down);
    const auto under = ctx.block(below);
    if (passable(under) && replaceable_by_flow(under)) {
        spread_to(ctx, below, new_liquid(ctx, below));
        int sources = 0;
        for (const auto d : plane) sources += is_source(ctx.block(pos.relative(d)));
        if (sources >= 3) spread_to_sides(ctx, pos, s);
        return;
    }
    if (is_source(s) || !is_hole(ctx, pos)) spread_to_sides(ctx, pos, s);
}
} // namespace

void tick_water(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto current = s;
    if (!is_source(s)) {
        const auto next = new_liquid(ctx, pos);
        if (next.is_air()) {
            set_block(ctx, pos, next);
            return;
        }
        if (next != s) {
            set_block(ctx, pos, next); // schedules the next fluid tick (on_placed)
            current = next;
        }
    }
    spread(ctx, pos, current);
}

void wheat_random_tick(RuleContext& ctx, BlockPos pos, BlockState s, Rng& stream) {
    if (s.level() >= 7) return;
    if (stream.bounded(3) == 0) set_block(ctx, pos, s.with_level(static_cast<std::uint8_t>(s.level() + 1)));
}

} // namespace circuit::gameplay::detail
