// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
constexpr std::uint32_t comparator_delay = 2;

// Components accepted as side inputs.
bool side_source(BlockType t) {
    return t == BlockType::redstone_wire || t == BlockType::repeater || t == BlockType::comparator
        || t == BlockType::redstone_block || t == BlockType::observer;
}
// Opaque blocks through which a comparator reads a container behind them.
bool reads_through(BlockType t) {
    return t == BlockType::stone || t == BlockType::obsidian || t == BlockType::redstone_lamp;
}

std::uint8_t rear_input(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto back = contracts::opposite(s.facing());
    const auto behind = pos.relative(back);
    const auto b = ctx.block(behind);
    if (is_container(b.type())) return container_signal(ctx, behind);
    auto power = emitted_power(ctx, behind, b, s.facing());
    if (reads_through(b.type())) {
        const auto further = behind.relative(back);
        if (is_container(ctx.block(further).type())) power = std::max(power, container_signal(ctx, further));
    }
    return power;
}

std::uint8_t side_input(RuleContext& ctx, BlockPos pos, BlockState s) {
    std::uint8_t best = 0;
    for (auto d : {contracts::clockwise(s.facing()), contracts::counter_clockwise(s.facing())}) {
        const auto at = pos.relative(d);
        const auto n = ctx.block(at);
        if (side_source(n.type())) best = std::max(best, emitted_power(ctx, at, n, contracts::opposite(d)));
    }
    return best;
}

std::uint8_t output(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto rear = rear_input(ctx, pos, s);
    const auto side = side_input(ctx, pos, s);
    if (s.level() == 1) return rear > side ? static_cast<std::uint8_t>(rear - side) : 0; // subtract
    return rear >= side ? rear : 0;                                                      // compare
}
} // namespace

void check_comparator(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::comparator)) return;
    const auto* be = ctx.peek_block_entity(pos);
    if (!be) return;
    const auto target = output(ctx, pos, s);
    // A comparator feeding another diode schedules at -1, otherwise at 0.
    if (target != be->signal || (target > 0) != s.flag())
        schedule(ctx, TickList::block, pos, BlockType::comparator, comparator_delay, feeds_diode(ctx, pos, s) ? -1 : 0);
}

void refresh_comparator(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    const auto next = output(ctx, pos, s);
    const auto previous = be->signal;
    if (next != previous) {
        be->signal = next;
        ctx.record(TraceKind::signal_changed, pos, previous, next);
    }
    const bool flag_change = (next > 0) != s.flag();
    if (flag_change) set_block_quiet(ctx, pos, s.with_flag(next > 0));
    if (flag_change || next != previous) update_front(ctx, pos, s.facing());
}

void tick_comparator(RuleContext& ctx, BlockPos pos, BlockState s) { refresh_comparator(ctx, pos, s); }

void container_changed(RuleContext& ctx, BlockPos pos) {
    for (auto d : spread_order) {
        const auto n = pos.relative(d);
        const auto ns = ctx.block(n);
        if (ns.is(BlockType::comparator)) {
            check_comparator(ctx, n);
        } else if (reads_through(ns.type())) {
            const auto m = n.relative(d);
            if (ctx.block(m).is(BlockType::comparator)) check_comparator(ctx, m);
        }
    }
}

} // namespace circuit::gameplay::detail
