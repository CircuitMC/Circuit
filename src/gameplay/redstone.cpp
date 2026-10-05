// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
std::uint8_t wire_input(RuleContext& ctx, BlockPos pos) {
    int best = 0;
    for (auto d : contracts::all_directions) {
        const auto n = ctx.block(pos.relative(d));
        if (n.is(BlockType::redstone_wire)) {
            if (contracts::is_horizontal(d)) best = std::max(best, static_cast<int>(n.level()) - 1);
        } else {
            best = std::max(best, static_cast<int>(emitted_power(n, contracts::opposite(d))));
        }
    }
    return static_cast<std::uint8_t>(best);
}

bool repeater_input(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto behind = pos.relative(contracts::opposite(s.facing()));
    return emitted_power(ctx.block(behind), s.facing()) > 0;
}

std::uint32_t repeater_delay(BlockState s) { return static_cast<std::uint32_t>(s.delay()) * 2; }
constexpr std::int8_t repeater_priority = -1;
} // namespace

void update_wire(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::redstone_wire)) return;
    const auto power = wire_input(ctx, pos);
    if (power != s.level()) set_block(ctx, pos, s.with_level(power));
}

void check_repeater(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::repeater)) return;
    if (repeater_input(ctx, pos, s) != s.flag())
        schedule(ctx, TickList::block, pos, BlockType::repeater, repeater_delay(s), repeater_priority);
}

void tick_repeater(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool input = repeater_input(ctx, pos, s);
    if (s.flag() && !input) {
        set_block(ctx, pos, s.with_flag(false));
    } else if (!s.flag()) {
        set_block(ctx, pos, s.with_flag(true));
        // A pulse shorter than the delay is extended to the delay.
        if (!input) schedule(ctx, TickList::block, pos, BlockType::repeater, repeater_delay(s), repeater_priority);
    }
}

void check_lamp(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::redstone_lamp)) return;
    const bool powered = received_power(ctx, pos) > 0;
    if (powered && !s.flag()) set_block(ctx, pos, s.with_flag(true));
    else if (!powered && s.flag()) schedule(ctx, TickList::block, pos, BlockType::redstone_lamp, 4, 0);
}

void tick_lamp(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.flag() && received_power(ctx, pos) == 0) set_block(ctx, pos, s.with_flag(false));
}

void tick_observer(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!s.flag()) {
        set_block(ctx, pos, s.with_flag(true));
        schedule(ctx, TickList::block, pos, BlockType::observer, 2, 0);
    } else {
        set_block(ctx, pos, s.with_flag(false));
    }
}

} // namespace circuit::gameplay::detail
