// SPDX-License-Identifier: AGPL-3.0-only
// Redstone torches, as both official servers run them (checked against their
// traces; minecraft.wiki "Redstone Torch"). A torch stands on the block below
// it or hangs on the side of a block, powers every neighbor but that block
// and the block above it strongly, and is turned off by power in that block:
// an update schedules it after 2 ticks when its state disagrees with its
// block, and the tick turns it off or on.
//
// Each turn-off counts. The eighth of one position within 60 ticks (the
// first may be exactly 60 ticks old) burns the torch out: it stays off and
// tries again after 160 ticks, relighting only when it is unpowered and its
// turn-offs have aged.
//
// Every state the torch takes, and its removal, updates the neighbors of
// each of its six neighbors.
#include "internal.hpp"

namespace circuit::gameplay::detail {
namespace {
constexpr std::uint32_t torch_delay = 2;
constexpr std::uint32_t burnout_delay = 160;
constexpr Tick turn_off_memory = 60;
constexpr std::size_t burnout_turn_offs = 8;

bool torch_powered(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto d = attachment(s);
    return signal_from(ctx, pos.relative(d), contracts::opposite(d)) > 0;
}
} // namespace

void torch_updates(RuleContext& ctx, BlockPos pos, BlockState torch) {
    for (const auto d : {Direction::down, Direction::up, Direction::north, Direction::south, Direction::west, Direction::east})
        notify_neighbors(ctx, pos.relative(d), std::nullopt, torch);
}

void check_torch(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.flag() == torch_powered(ctx, pos, s)) schedule(ctx, TickList::block, pos, s.type(), torch_delay, 0);
}

void tick_torch(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto now = ctx.now();
    const Tick since = now > turn_off_memory ? now - turn_off_memory : 0;
    const bool powered = torch_powered(ctx, pos, s);
    if (s.flag() && powered) {
        set_block(ctx, pos, s.with_flag(false));
        if (ctx.torch_turn_offs(pos, since, true) >= burnout_turn_offs)
            schedule(ctx, TickList::block, pos, s.type(), burnout_delay, 0);
    } else if (!s.flag() && !powered) {
        if (ctx.torch_turn_offs(pos, since, false) < burnout_turn_offs) set_block(ctx, pos, s.with_flag(true));
    } else {
        ctx.torch_turn_offs(pos, since, false);
    }
}

} // namespace circuit::gameplay::detail
