// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
bool is_source(BlockState s) { return s.is(BlockType::water) && s.level() == 0 && !s.flag(); }
// Spread distance of water: sources and falling water act as distance 0.
int distance(BlockState s) { return s.flag() ? 0 : s.level(); }
} // namespace

void tick_water(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto current = s;
    if (!is_source(s)) {
        BlockState next;
        if (ctx.block(pos.relative(Direction::up)).is(BlockType::water)) {
            next = blocks::water_falling();
        } else {
            int best = 8;
            for (auto d : spread_order) {
                const auto n = ctx.block(pos.relative(d));
                if (n.is(BlockType::water)) best = std::min(best, distance(n));
            }
            next = best + 1 <= 7 ? blocks::water_flowing(static_cast<std::uint8_t>(best + 1)) : blocks::air();
        }
        if (next != s) {
            set_block(ctx, pos, next);
            if (next.is_air()) return;
            current = next;
        }
    }
    const auto below = pos.relative(Direction::down);
    const auto under = ctx.block(below);
    if (under.is_air()) {
        set_block(ctx, below, blocks::water_falling());
        return;
    }
    if (under.is(BlockType::water)) return;
    const int spread = distance(current) + 1;
    if (spread > 7) return;
    for (auto d : spread_order) {
        const auto n = pos.relative(d);
        if (ctx.block(n).is_air()) set_block(ctx, n, blocks::water_flowing(static_cast<std::uint8_t>(spread)));
    }
}

void wheat_random_tick(RuleContext& ctx, BlockPos pos, BlockState s, Rng& stream) {
    if (s.level() >= 7) return;
    if (stream.bounded(3) == 0) set_block(ctx, pos, s.with_level(static_cast<std::uint8_t>(s.level() + 1)));
}

} // namespace circuit::gameplay::detail
