// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <array>

namespace circuit::gameplay {
using namespace detail;

bool set_block(RuleContext& ctx, BlockPos pos, BlockState next) {
    const auto prev = ctx.block(pos);
    if (prev.is_unloaded() || next.is_unloaded() || prev == next) return false;
    const bool replaced = !same_block(prev, next);
    std::vector<contracts::ItemStack> contents;
    if (replaced && has_block_entity(prev.type())) {
        if (is_container(prev.type()))
            if (const auto* be = ctx.peek_block_entity(pos)) contents = be->slots;
        ctx.remove_block_entity(pos);
    } else if (replaced && prev.is(BlockType::other) && ctx.peek_block_entity(pos)) {
        ctx.remove_block_entity(pos); // the saved data of a block without rules goes with it
    }
    ctx.write_block(pos, next);
    ctx.record(TraceKind::block_changed, pos, prev.raw(), next.raw());
    // Removal and placement reactions come first, then the neighbor updates,
    // then the shape updates (the target version's order). A removed
    // container drops its contents.
    if (replaced) {
        drop_contents(ctx, pos, contents);
        on_removed(ctx, pos, prev);
    }
    if (replaced && has_block_entity(next.type())) ctx.create_block_entity(pos, next.type());
    on_placed(ctx, pos, next, prev);
    notify_neighbors(ctx, pos);
    shape_updates(ctx, pos, prev, next);
    return true;
}

std::uint8_t received_power(RuleContext& ctx, BlockPos pos) { return best_neighbor_signal(ctx, pos, true); }

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

// Direction from `target` to its neighbor `from`.
std::optional<Direction> direction_to(BlockPos target, BlockPos from) {
    for (const auto d : contracts::all_directions)
        if (target.relative(d) == from) return d;
    return std::nullopt;
}

// A block that can no longer stand breaks with its drops; diodes also update
// around the cell they left.
void break_unsupported(RuleContext& ctx, BlockPos pos, BlockState s) {
    destroy_block(ctx, pos);
    if (s.is(BlockType::repeater) || s.is(BlockType::comparator))
        for (const auto d : {Direction::down, Direction::up, Direction::north, Direction::south, Direction::west, Direction::east})
            notify_neighbors(ctx, pos.relative(d));
}
} // namespace

void notify_neighbors(RuleContext& ctx, BlockPos source, std::optional<Direction> except) {
    enqueue(ctx, {.kind = Kind::neighbors, .source = source, .target = source, .next = 0,
                  .skip = except ? static_cast<std::int8_t>(*except) : std::int8_t{-1}, .depth = 0});
}

void notify_single(RuleContext& ctx, BlockPos target, BlockPos from) {
    enqueue(ctx, {.kind = Kind::single, .source = from, .target = target, .next = 0, .skip = -1, .depth = 0});
}

void notify_shape(RuleContext& ctx, BlockPos target, BlockPos from) {
    const auto depth = static_cast<std::uint16_t>(ctx.updater().shape_depth + 1);
    if (depth > shape_depth_limit) return;
    enqueue(ctx, {.kind = Kind::shape, .source = from, .target = target, .next = 0, .skip = -1, .depth = depth});
}

void notify_shapes(RuleContext& ctx, BlockPos source) {
    for (auto d : shape_order) notify_shape(ctx, source.relative(d), source);
}

void shape_updates(RuleContext& ctx, BlockPos pos, BlockState prev, BlockState next) {
    indirect_shapes(ctx, pos, prev);
    notify_shapes(ctx, pos);
    indirect_shapes(ctx, pos, next);
}

void set_block_quiet(RuleContext& ctx, BlockPos pos, BlockState next) {
    const auto prev = ctx.block(pos);
    if (prev.is_unloaded() || prev == next) return;
    ctx.write_block(pos, next);
    ctx.record(TraceKind::block_changed, pos, prev.raw(), next.raw());
    // A diode's front is updated before the shape updates (vertical_slice).
    on_placed(ctx, pos, next, prev);
    shape_updates(ctx, pos, prev, next);
}

void destroy_block(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (s.is_air() || s.is_unloaded()) return;
    // Drops first, then the block goes with full updates.
    drop_items(ctx, pos, s);
    set_block(ctx, pos, blocks::air());
}

bool head_has_base(RuleContext& ctx, BlockPos pos, BlockState head) {
    const auto base = ctx.block(pos.relative(contracts::opposite(head.facing())));
    if (is_piston(base.type())) return base.flag() && base.facing() == head.facing();
    return base.is(BlockType::moving_piston) && base.facing() == head.facing();
}

void update_front(RuleContext& ctx, BlockPos pos, Direction output) {
    const auto front = pos.relative(output);
    notify_single(ctx, front, pos);
    notify_neighbors(ctx, front, contracts::opposite(output));
}

void on_shape_changed(RuleContext& ctx, BlockPos target, BlockPos from) {
    const auto s = ctx.block(target);
    const auto d = direction_to(target, from);
    if (!d) return;
    switch (s.type()) {
    case BlockType::observer:
        if (s.facing() == *d) schedule(ctx, TickList::block, target, BlockType::observer, 2, 0);
        break;
    case BlockType::chest: check_chest(ctx, target); break;
    case BlockType::redstone_wire: wire_shape_update(ctx, target, *d); break;
    case BlockType::repeater: repeater_shape_update(ctx, target, *d); break;
    case BlockType::comparator:
        if (*d == Direction::down && !can_survive(ctx, target, s)) destroy_block(ctx, target);
        break;
    case BlockType::lever:
        if (*d == lever_attachment(s) && !can_survive(ctx, target, s)) destroy_block(ctx, target);
        break;
    case BlockType::piston_head:
        // A head without its base disappears.
        if (*d == contracts::opposite(s.facing()) && !head_has_base(ctx, target, s)) set_block(ctx, target, blocks::air());
        break;
    case BlockType::other:
        if (falls(s)) schedule_fall(ctx, target);
        break;
    default: break;
    }
}

void on_placed(RuleContext& ctx, BlockPos pos, BlockState state, BlockState previous) {
    const bool fresh = previous.type() != state.type();
    const bool moved_in = previous.is(BlockType::moving_piston);
    switch (state.type()) {
    case BlockType::redstone_wire:
        if (fresh) wire_placed(ctx, pos);
        break;
    // Every state change of a diode updates the block in front.
    case BlockType::repeater: case BlockType::comparator: update_front(ctx, pos, state.facing()); break;
    // A block a piston moved here reacts to its new neighbors.
    case BlockType::redstone_lamp:
        if (moved_in) check_lamp(ctx, pos);
        break;
    case BlockType::piston: case BlockType::sticky_piston:
        if (fresh) check_piston(ctx, pos);
        break;
    case BlockType::hopper:
        if (fresh) check_hopper(ctx, pos);
        break;
    // Every change of water schedules its next fluid tick.
    case BlockType::water: schedule(ctx, TickList::fluid, pos, BlockType::water, 5, 0); break;
    case BlockType::chest:
        if (fresh) check_chest(ctx, pos);
        break;
    case BlockType::observer:
        // An observer moved by a piston fires at its new position (observed in
        // target-version traces); other placements do not.
        if (moved_in) schedule(ctx, TickList::block, pos, BlockType::observer, 2, 0);
        break;
    case BlockType::other:
        if (falls(state)) schedule_fall(ctx, pos);
        // TNT placed or moved in where redstone reaches it is primed.
        else if (is_tnt(state) && !is_tnt(previous)) check_tnt(ctx, pos);
        break;
    default: break;
    }
}

void on_removed(RuleContext& ctx, BlockPos pos, BlockState previous) {
    switch (previous.type()) {
    case BlockType::redstone_wire: wire_removed(ctx, pos, previous); break;
    case BlockType::repeater: case BlockType::comparator: update_front(ctx, pos, previous.facing()); break;
    case BlockType::observer:
        if (previous.flag()) update_front(ctx, pos, contracts::opposite(previous.facing()));
        break;
    case BlockType::lever:
        if (previous.flag()) {
            notify_neighbors(ctx, pos);
            notify_neighbors(ctx, pos.relative(lever_attachment(previous)));
        }
        break;
    default: break;
    }
}

void on_neighbor_changed(RuleContext& ctx, BlockPos target, [[maybe_unused]] BlockPos from) {
    const auto s = ctx.block(target);
    switch (s.type()) {
    case BlockType::redstone_wire:
        if (!can_survive(ctx, target, s)) destroy_block(ctx, target);
        else update_wire(ctx, target);
        break;
    case BlockType::repeater:
        if (!can_survive(ctx, target, s)) break_unsupported(ctx, target, s);
        else check_repeater(ctx, target);
        break;
    case BlockType::comparator:
        if (!can_survive(ctx, target, s)) break_unsupported(ctx, target, s);
        else check_comparator(ctx, target);
        break;
    case BlockType::redstone_lamp: check_lamp(ctx, target); break;
    case BlockType::piston: case BlockType::sticky_piston: check_piston(ctx, target); break;
    case BlockType::hopper: check_hopper(ctx, target); break;
    case BlockType::water: schedule(ctx, TickList::fluid, target, BlockType::water, 5, 0); break;
    case BlockType::other:
        if (is_tnt(s)) check_tnt(ctx, target);
        break;
    default: break;
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
