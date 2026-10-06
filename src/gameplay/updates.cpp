// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <array>

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
    if (replaced) on_placed(ctx, pos, next, prev.type());
    notify_neighbors(ctx, pos);
    notify_shapes(ctx, pos);
    return true;
}

std::uint8_t received_power(RuleContext& ctx, BlockPos pos) {
    std::uint8_t best = 0;
    for (auto d : contracts::all_directions) {
        const auto n = pos.relative(d);
        best = std::max(best, emitted_power(ctx, n, ctx.block(n), contracts::opposite(d)));
    }
    return best;
}

namespace detail {

namespace {
constexpr std::array<Direction, 6> shape_order{
    Direction::west, Direction::east, Direction::north, Direction::south, Direction::down, Direction::up};
constexpr std::uint32_t shape_depth_limit = 512;

using Kind = NeighborUpdater::Kind;

void enqueue(RuleContext& ctx, NeighborUpdater::Entry entry) {
    auto& u = ctx.updater();
    if (u.running) {
        if (u.count >= ctx.bundle().limits.max_chained_neighbor_updates) {
            // Version rule: entries beyond the limit are dropped; queued ones still run.
            if (!u.truncated) ctx.record(TraceKind::chain_truncated, entry.source, u.count);
            u.truncated = true;
            return;
        }
        ++u.count;
        u.layer.push_back(entry);
        return;
    }
    u.running = true;
    u.truncated = false;
    u.count = 1;
    u.stack.push_back(entry);
    while (!u.stack.empty()) {
        auto& top = u.stack.back();
        if (top.kind == Kind::neighbors) {
            if (top.next == update_order.size()) {
                u.stack.pop_back();
                continue;
            }
            const auto dir = update_order[top.next++];
            if (top.skip == static_cast<std::int8_t>(dir)) continue;
            const auto from = top.source;
            on_neighbor_changed(ctx, from.relative(dir), from);
        } else {
            const auto e = top;
            u.stack.pop_back();
            if (e.kind == Kind::single) {
                on_neighbor_changed(ctx, e.target, e.source);
            } else {
                u.shape_depth = e.depth;
                on_shape_changed(ctx, e.target, e.source);
                u.shape_depth = 0;
            }
        }
        for (auto it = u.layer.rbegin(); it != u.layer.rend(); ++it) u.stack.push_back(*it);
        u.layer.clear();
    }
    u.running = false;
}
} // namespace

void notify_neighbors(RuleContext& ctx, BlockPos source, std::optional<Direction> except) {
    enqueue(ctx, {.kind = Kind::neighbors, .source = source, .target = source, .next = 0,
                  .skip = except ? static_cast<std::int8_t>(*except) : std::int8_t{-1}, .depth = 0});
}

void notify_single(RuleContext& ctx, BlockPos target, BlockPos from) {
    enqueue(ctx, {.kind = Kind::single, .source = from, .target = target, .next = 0, .skip = -1, .depth = 0});
}

void notify_shapes(RuleContext& ctx, BlockPos source) {
    const auto depth = static_cast<std::uint16_t>(ctx.updater().shape_depth + 1);
    if (depth > shape_depth_limit) return;
    for (auto d : shape_order)
        enqueue(ctx, {.kind = Kind::shape, .source = source, .target = source.relative(d), .next = 0, .skip = -1, .depth = depth});
}

void set_block_quiet(RuleContext& ctx, BlockPos pos, BlockState next) {
    const auto prev = ctx.block(pos);
    if (prev.is_unloaded() || prev == next) return;
    ctx.write_block(pos, next);
    ctx.record(TraceKind::block_changed, pos, prev.raw(), next.raw());
    notify_shapes(ctx, pos);
}

void update_front(RuleContext& ctx, BlockPos pos, Direction output) {
    const auto front = pos.relative(output);
    notify_single(ctx, front, pos);
    notify_neighbors(ctx, front, contracts::opposite(output));
}

void on_shape_changed(RuleContext& ctx, BlockPos target, BlockPos from) {
    const auto s = ctx.block(target);
    switch (s.type()) {
    case BlockType::observer:
        if (target.relative(s.facing()) == from) schedule(ctx, TickList::block, target, BlockType::observer, 2, 0);
        break;
    case BlockType::chest: check_chest(ctx, target); break;
    default: break;
    }
}

void on_placed(RuleContext& ctx, BlockPos pos, BlockState state, BlockType replaced) {
    switch (state.type()) {
    case BlockType::redstone_wire: update_wire(ctx, pos); break;
    case BlockType::repeater: check_repeater(ctx, pos); break;
    case BlockType::redstone_lamp: check_lamp(ctx, pos); break;
    case BlockType::piston: case BlockType::sticky_piston: check_piston(ctx, pos); break;
    case BlockType::hopper: check_hopper(ctx, pos); break;
    case BlockType::water: schedule(ctx, TickList::fluid, pos, BlockType::water, 5, 0); break;
    case BlockType::chest: check_chest(ctx, pos); break;
    case BlockType::comparator: check_comparator(ctx, pos); break;
    case BlockType::observer:
        // An observer moved by a piston fires at its new position (observed in
        // target-version traces); other placements do not.
        if (replaced == BlockType::moving_piston) schedule(ctx, TickList::block, pos, BlockType::observer, 2, 0);
        break;
    default: break;
    }
}

void on_neighbor_changed(RuleContext& ctx, BlockPos target, [[maybe_unused]] BlockPos from) {
    const auto s = ctx.block(target);
    switch (s.type()) {
    case BlockType::redstone_wire: update_wire(ctx, target); break;
    case BlockType::repeater: check_repeater(ctx, target); break;
    case BlockType::redstone_lamp: check_lamp(ctx, target); break;
    case BlockType::piston: case BlockType::sticky_piston: check_piston(ctx, target); break;
    case BlockType::hopper: check_hopper(ctx, target); break;
    case BlockType::water: schedule(ctx, TickList::fluid, target, BlockType::water, 5, 0); break;
    case BlockType::comparator: check_comparator(ctx, target); break;
    default: break;
    }
}

std::uint8_t emitted_power(RuleContext& ctx, BlockPos pos, BlockState s, Direction toward) {
    switch (s.type()) {
    case BlockType::comparator: {
        if (toward != s.facing()) return 0;
        const auto* be = ctx.peek_block_entity(pos);
        return be ? be->signal : 0;
    }
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
