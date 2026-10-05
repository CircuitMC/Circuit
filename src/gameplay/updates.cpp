// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay {
using namespace detail;

bool set_block(RuleContext& ctx, BlockPos pos, BlockState next) {
    const auto prev = ctx.block(pos);
    if (prev.is_unloaded() || next.is_unloaded() || prev == next) return false;
    const bool replaced = prev.type() != next.type();
    if (replaced && has_block_entity(prev.type())) ctx.remove_block_entity(pos);
    ctx.write_block(pos, next);
    ctx.record(TraceKind::block_changed, pos, prev.raw(), next.raw());
    if (replaced && has_block_entity(next.type())) ctx.create_block_entity(pos, next.type());
    if (replaced) on_placed(ctx, pos, next);
    notify_neighbors(ctx, pos);
    return true;
}

std::uint8_t received_power(RuleContext& ctx, BlockPos pos) {
    std::uint8_t best = 0;
    for (auto d : contracts::all_directions)
        best = std::max(best, emitted_power(ctx.block(pos.relative(d)), contracts::opposite(d)));
    return best;
}

namespace detail {

void notify_neighbors(RuleContext& ctx, BlockPos source) {
    auto& u = ctx.updater();
    if (u.running) {
        u.layer.push_back(source);
        return;
    }
    u.running = true;
    u.delivered = 0;
    u.stack.push_back({source, 0});
    const auto limit = ctx.bundle().limits.max_chained_neighbor_updates;
    while (!u.stack.empty()) {
        auto& top = u.stack.back();
        if (top.next == update_order.size()) {
            u.stack.pop_back();
            continue;
        }
        const auto from = top.source;
        const auto dir = update_order[top.next++];
        if (u.delivered >= limit) {
            // Version rule: the remainder of this chain is discarded.
            ctx.record(TraceKind::chain_truncated, from, u.delivered);
            u.stack.clear();
            u.layer.clear();
            break;
        }
        ++u.delivered;
        on_neighbor_changed(ctx, from.relative(dir), from);
        for (auto it = u.layer.rbegin(); it != u.layer.rend(); ++it) u.stack.push_back({*it, 0});
        u.layer.clear();
    }
    u.running = false;
}

void on_placed(RuleContext& ctx, BlockPos pos, BlockState state) {
    switch (state.type()) {
    case BlockType::redstone_wire: update_wire(ctx, pos); break;
    case BlockType::repeater: check_repeater(ctx, pos); break;
    case BlockType::redstone_lamp: check_lamp(ctx, pos); break;
    case BlockType::piston: case BlockType::sticky_piston: check_piston(ctx, pos); break;
    case BlockType::hopper: check_hopper(ctx, pos); break;
    case BlockType::water: schedule(ctx, TickList::fluid, pos, BlockType::water, 5, 0); break;
    default: break;
    }
}

void on_neighbor_changed(RuleContext& ctx, BlockPos target, BlockPos from) {
    const auto s = ctx.block(target);
    switch (s.type()) {
    case BlockType::redstone_wire: update_wire(ctx, target); break;
    case BlockType::repeater: check_repeater(ctx, target); break;
    case BlockType::redstone_lamp: check_lamp(ctx, target); break;
    case BlockType::observer:
        if (target.relative(s.facing()) == from) schedule(ctx, TickList::block, target, BlockType::observer, 2, 0);
        break;
    case BlockType::piston: case BlockType::sticky_piston: check_piston(ctx, target); break;
    case BlockType::hopper: check_hopper(ctx, target); break;
    case BlockType::water: schedule(ctx, TickList::fluid, target, BlockType::water, 5, 0); break;
    default: break;
    }
}

std::uint8_t emitted_power(BlockState s, Direction toward) noexcept {
    switch (s.type()) {
    case BlockType::lever: return s.flag() ? 15 : 0;
    case BlockType::redstone_block: return 15;
    case BlockType::repeater: return s.flag() && toward == s.facing() ? 15 : 0;
    case BlockType::observer: return s.flag() && toward == contracts::opposite(s.facing()) ? 15 : 0;
    case BlockType::redstone_wire: return toward != Direction::up ? s.level() : 0;
    default: return 0;
    }
}

bool schedule(RuleContext& ctx, TickList list, BlockPos pos, BlockType block, std::uint32_t delay, std::int8_t priority) {
    if (ctx.has_tick(list, pos, block)) return false;
    const Tick due = ctx.now() + delay;
    const auto seq = ctx.record(TraceKind::tick_scheduled, pos, tick_code(list, block), due,
                                static_cast<std::uint64_t>(priority + 128));
    ctx.add_tick(list, pos, block, due, priority, seq);
    return true;
}

bool queue_block_event(RuleContext& ctx, const BlockEvent& event) {
    if (ctx.has_block_event(event)) return false;
    const auto seq = ctx.record(TraceKind::block_event_queued, event.pos, static_cast<std::uint64_t>(event.block),
                                event.event, event.param);
    ctx.add_block_event(event, seq);
    return true;
}

} // namespace detail
} // namespace circuit::gameplay
