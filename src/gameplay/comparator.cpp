// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
constexpr std::uint32_t comparator_delay = 2;

// Rear input: a container behind, or the power behind (dust whatever its
// shape); a container behind a conductor replaces a weaker signal.
std::uint8_t rear_input(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto back = contracts::opposite(s.facing());
    const auto behind = pos.relative(back);
    const auto b = ctx.block(behind);
    if (is_container(b.type())) return container_signal(ctx, behind);
    auto power = diode_input(ctx, pos, s);
    if (power < 15 && is_conductor(b.type())) {
        const auto further = behind.relative(back);
        if (is_container(ctx.block(further).type())) power = container_signal(ctx, further);
    }
    return power;
}

// Side inputs: redstone blocks, dust of any shape, and the strong power of
// diodes and observers facing in. No power through blocks.
std::uint8_t side_input(RuleContext& ctx, BlockPos pos, BlockState s) {
    std::uint8_t best = 0;
    for (auto d : {contracts::clockwise(s.facing()), contracts::counter_clockwise(s.facing())}) {
        const auto at = pos.relative(d);
        const auto n = ctx.block(at);
        std::uint8_t v = 0;
        if (n.is(BlockType::redstone_block)) v = 15;
        else if (n.is(BlockType::redstone_wire)) v = n.level();
        else v = direct(ctx, at, n, contracts::opposite(d), true);
        best = std::max(best, v);
    }
    return best;
}

} // namespace

std::uint8_t comparator_output(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto rear = rear_input(ctx, pos, s);
    const auto side = side_input(ctx, pos, s);
    if (s.level() == 1) return rear > side ? static_cast<std::uint8_t>(rear - side) : 0; // subtract
    return rear >= side ? rear : 0;                                                      // compare
}

void check_comparator(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::comparator)) return;
    const auto* be = ctx.peek_block_entity(pos);
    if (!be) return;
    const auto target = comparator_output(ctx, pos, s);
    // A comparator feeding another diode schedules at -1, otherwise at 0.
    if (target != be->signal || (target > 0) != s.flag())
        schedule(ctx, TickList::block, pos, BlockType::comparator, comparator_delay, feeds_diode(ctx, pos, s) ? -1 : 0);
}

void refresh_comparator(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    const auto next = comparator_output(ctx, pos, s);
    const auto previous = be->signal;
    if (next != previous) {
        be->signal = next;
        ctx.record(TraceKind::signal_changed, pos, previous, next);
    }
    // In compare mode the front is updated even when the signal stays; a
    // change of the powered state updates it once more (see on_placed).
    if (next == previous && s.level() == 1) return;
    if ((next > 0) != s.flag()) set_block_quiet(ctx, pos, s.with_flag(next > 0));
    update_front(ctx, pos, s.facing());
}

void tick_comparator(RuleContext& ctx, BlockPos pos, BlockState s) { refresh_comparator(ctx, pos, s); }

void container_changed(RuleContext& ctx, BlockPos pos) {
    for (auto d : spread_order) {
        const auto n = pos.relative(d);
        const auto ns = ctx.block(n);
        if (ns.is(BlockType::comparator)) {
            check_comparator(ctx, n);
        } else if (is_conductor(ns.type())) {
            const auto m = n.relative(d);
            if (ctx.block(m).is(BlockType::comparator)) check_comparator(ctx, m);
        }
    }
}

} // namespace circuit::gameplay::detail
