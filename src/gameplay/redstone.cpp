// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
std::uint8_t wire_input(RuleContext& ctx, BlockPos pos) {
    int best = 0;
    for (auto d : contracts::all_directions) {
        const auto at = pos.relative(d);
        const auto n = ctx.block(at);
        if (n.is(BlockType::redstone_wire)) {
            if (contracts::is_horizontal(d)) best = std::max(best, static_cast<int>(n.level()) - 1);
        } else {
            best = std::max(best, static_cast<int>(emitted_power(ctx, at, n, contracts::opposite(d))));
        }
    }
    return static_cast<std::uint8_t>(best);
}

bool repeater_input(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto behind = pos.relative(contracts::opposite(s.facing()));
    return emitted_power(ctx, behind, ctx.block(behind), s.facing()) > 0;
}

std::uint32_t repeater_delay(BlockState s) { return static_cast<std::uint32_t>(s.delay()) * 2; }

// Documented, and observed in target-version traces: a repeater feeding another
// diode schedules at -3, one turning off at -2, otherwise -1.
std::int8_t repeater_priority(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (feeds_diode(ctx, pos, s)) return -3;
    return s.flag() ? -2 : -1;
}
// The turn-off scheduled by pulse extension is not raised for a diode in front.
constexpr std::int8_t pulse_end_priority = -2;
} // namespace

bool feeds_diode(RuleContext& ctx, BlockPos pos, BlockState s) {
    // Diodes face their output side.
    const auto front = ctx.block(pos.relative(s.facing()));
    return (front.is(BlockType::repeater) || front.is(BlockType::comparator))
        && front.facing() != contracts::opposite(s.facing());
}

void update_wire(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::redstone_wire)) return;
    const auto power = wire_input(ctx, pos);
    if (power == s.level()) return;
    // A power change updates the neighbors of the dust and of each of its six
    // neighbors, in hash-set order (observed in target-version traces).
    const auto next = s.with_level(power);
    ctx.write_block(pos, next);
    ctx.record(TraceKind::block_changed, pos, s.raw(), next.raw());
    notify_shapes(ctx, pos);
    std::vector<BlockPos> around{pos};
    for (auto d : contracts::all_directions) around.push_back(pos.relative(d));
    for (const auto p : hash_order(around, around.size())) notify_neighbors(ctx, p);
}

void check_repeater(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::repeater)) return;
    if (repeater_input(ctx, pos, s) != s.flag())
        schedule(ctx, TickList::block, pos, BlockType::repeater, repeater_delay(s), repeater_priority(ctx, pos, s));
}

void tick_repeater(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool input = repeater_input(ctx, pos, s);
    if (s.flag() && !input) {
        set_block_quiet(ctx, pos, s.with_flag(false));
        update_front(ctx, pos, s.facing());
    } else if (!s.flag()) {
        set_block_quiet(ctx, pos, s.with_flag(true));
        update_front(ctx, pos, s.facing());
        // A pulse shorter than the delay is extended to the delay.
        if (!input) schedule(ctx, TickList::block, pos, BlockType::repeater, repeater_delay(s), pulse_end_priority);
    }
}

void check_lamp(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::redstone_lamp)) return;
    const bool powered = received_power(ctx, pos) > 0;
    if (powered && !s.flag()) set_block_quiet(ctx, pos, s.with_flag(true));
    else if (!powered && s.flag()) schedule(ctx, TickList::block, pos, BlockType::redstone_lamp, 4, 0);
}

void tick_lamp(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.flag() && received_power(ctx, pos) == 0) set_block_quiet(ctx, pos, s.with_flag(false));
}

void tick_observer(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!s.flag()) {
        set_block_quiet(ctx, pos, s.with_flag(true));
        update_front(ctx, pos, contracts::opposite(s.facing()));
        schedule(ctx, TickList::block, pos, BlockType::observer, 2, 0);
    } else {
        set_block_quiet(ctx, pos, s.with_flag(false));
        update_front(ctx, pos, contracts::opposite(s.facing()));
    }
}

} // namespace circuit::gameplay::detail
