// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <optional>
#include <set>

namespace circuit::gameplay::detail {
namespace {
constexpr std::size_t push_limit = 12;
constexpr std::uint8_t move_ticks = 2;

bool sticky(BlockState s) { return s.is(BlockType::slime_block); }

// Structure discovery of the target version as observed in harness traces:
// a line runs from the sticky chain behind its origin through the origin and
// forward until air or a destroyable block; every sticky block in the list
// then branches into lines from its neighbors off the movement axis, in the
// game's direction order (down, up, north, south, west, east).
class Resolver {
public:
    Resolver(RuleContext& ctx, BlockPos piston, Direction movement) : ctx_(ctx), piston_(piston), movement_(movement) {}

    bool resolve(BlockPos start) {
        const auto s = ctx_.block(start);
        if (s.is_air()) return true;
        if (!is_movable(s) || start == piston_) {
            if (destroyed_by_piston(s.type())) {
                destroyed.push_back(start);
                return true;
            }
            return false;
        }
        if (!line(start)) return false;
        for (std::size_t i = 0; i < moved.size(); ++i)
            if (sticky(ctx_.block(moved[i])) && !branches(moved[i])) return false;
        return true;
    }

    std::vector<BlockPos> moved;
    std::vector<BlockPos> destroyed;

private:
    bool contains(BlockPos p) const { return std::ranges::find(moved, p) != moved.end(); }

    bool line(BlockPos origin) {
        auto state = ctx_.block(origin);
        if (state.is_air() || !is_movable(state) || origin == piston_ || contains(origin)) return true;
        std::size_t count = 1;
        if (count + moved.size() > push_limit) return false;
        // Sticky blocks drag the chain behind them.
        while (sticky(state)) {
            const auto behind = origin.relative(contracts::opposite(movement_), static_cast<std::int32_t>(count));
            const auto bs = ctx_.block(behind);
            if (bs.is_air() || !is_movable(bs) || behind == piston_) break;
            if (++count + moved.size() > push_limit) return false;
            state = bs;
        }
        for (std::size_t k = count; k-- > 0;)
            moved.push_back(origin.relative(contracts::opposite(movement_), static_cast<std::int32_t>(k)));
        for (std::int32_t j = 1;; ++j) {
            const auto ahead = origin.relative(movement_, j);
            const auto as = ctx_.block(ahead);
            if (as.is_air()) return true;
            if (destroyed_by_piston(as.type())) {
                destroyed.push_back(ahead);
                return true;
            }
            if (!is_movable(as) || ahead == piston_) return false;
            if (contains(ahead)) return true;
            if (moved.size() + 1 > push_limit) return false;
            moved.push_back(ahead);
        }
    }

    bool branches(BlockPos p) {
        for (auto d : contracts::all_directions) {
            if (d == movement_ || d == contracts::opposite(movement_)) continue;
            const auto n = p.relative(d);
            const auto ns = ctx_.block(n);
            if (ns.is_air() || !is_movable(ns) || n == piston_) continue;
            if (!line(n)) return false;
        }
        return true;
    }

    RuleContext& ctx_;
    BlockPos piston_;
    Direction movement_;
};

// Writes of one piston action, in the target version's order. Every write
// sends shape updates at once (observed in target-version traces); neighbor
// updates for the changed positions follow when the action finishes, in
// first-write order.
class Edit {
public:
    explicit Edit(RuleContext& ctx) : ctx_(ctx) {}
    // Every write is recorded as it happens, including intermediate states.
    void write(BlockPos p, BlockState s) {
        const auto before = ctx_.block(p);
        if (before == s) return;
        ctx_.write_block(p, s);
        ctx_.record(TraceKind::block_changed, p, before.raw(), s.raw());
        if (touched_.insert(p).second) changed_.push_back(p);
        notify_shapes(ctx_, p);
    }
    // Replaces a block, and its block entity if any, with air.
    void clear(BlockPos p) {
        if (has_block_entity(ctx_.block(p).type())) ctx_.remove_block_entity(p);
        write(p, blocks::air());
    }
    // A block destroyed by the move: removed, and its drops spawned.
    void destroy(BlockPos p) {
        const auto s = ctx_.block(p);
        clear(p);
        drop_items(ctx_, p, s);
    }
    // Places a moving piston that turns into `carried` after the move.
    void moving(BlockPos p, BlockState carried, Motion motion, bool sticky_type) {
        if (has_block_entity(ctx_.block(p).type())) ctx_.remove_block_entity(p);
        write(p, blocks::moving_piston(motion.facing, sticky_type));
        ctx_.create_block_entity(p, BlockType::moving_piston);
        auto* be = ctx_.block_entity(p);
        be->moving = carried.raw();
        be->motion = motion.pack();
        be->progress = 0;
    }
    // Ends a move at once: the carried block appears in place.
    void finish_moving(BlockPos p, BlockState carried) {
        ctx_.remove_block_entity(p);
        write(p, carried);
        placed_.push_back(p);
    }
    // Moves a resolved structure one block along `dir`: destroyed blocks first,
    // then moving blocks from the last discovered to the first, then `head`
    // (if any), then vacated cells in hash order.
    void move(const Resolver& structure, Direction dir, Motion motion, std::optional<std::pair<BlockPos, BlockState>> head,
              bool head_sticky) {
        std::vector<BlockState> states;
        for (auto p : structure.moved) states.push_back(ctx_.block(p));
        for (auto it = structure.destroyed.rbegin(); it != structure.destroyed.rend(); ++it) destroy(*it);
        std::set<BlockPos> occupied;
        for (std::size_t i = structure.moved.size(); i-- > 0;) {
            const auto destination = structure.moved[i].relative(dir);
            moving(destination, states[i], motion, false);
            occupied.insert(destination);
        }
        if (head) {
            moving(head->first, head->second, {motion.facing, motion.extending, true}, head_sticky);
            occupied.insert(head->first);
        }
        std::vector<BlockPos> vacated;
        for (auto p : structure.moved)
            if (!occupied.contains(p)) vacated.push_back(p);
        for (auto p : hash_order(vacated, structure.moved.size())) write(p, blocks::air());
    }
    void finish() {
        for (auto p : placed_) on_placed(ctx_, p, ctx_.block(p), blocks::moving_piston(Direction::down, false));
        for (auto p : changed_) notify_neighbors(ctx_, p);
    }

private:
    RuleContext& ctx_;
    std::set<BlockPos> touched_;
    std::vector<BlockPos> changed_;
    std::vector<BlockPos> placed_;
};

bool extend(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto facing = s.facing();
    const auto head_pos = pos.relative(facing);
    const bool sticky_piston = s.is(BlockType::sticky_piston);
    Resolver structure(ctx, pos, facing);
    if (!structure.resolve(head_pos)) return false;
    Edit edit(ctx);
    edit.move(structure, facing, {facing, true, false},
              std::pair{head_pos, BlockState::of(BlockType::piston_head).with_facing(facing).with_flag(sticky_piston)},
              sticky_piston);
    edit.write(pos, s.with_flag(true));
    edit.finish();
    return true;
}

// A moving piston at `p` that this piston (facing `facing`) is still pushing.
std::optional<BlockState> pushed_block(RuleContext& ctx, BlockPos p, Direction facing) {
    if (!ctx.block(p).is(BlockType::moving_piston)) return std::nullopt;
    const auto* be = ctx.peek_block_entity(p);
    if (!be) return std::nullopt;
    const auto m = Motion::unpack(be->motion);
    if (!m.extending || m.source || m.facing != facing) return std::nullopt;
    return BlockState::decode(be->moving).value_or(blocks::air());
}

void retract(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto facing = s.facing();
    const auto head_pos = pos.relative(facing);
    const auto target = pos.relative(facing, 2);
    const bool sticky_piston = s.is(BlockType::sticky_piston);
    Edit edit(ctx);
    // A head still extending is removed first.
    if (ctx.block(head_pos).is(BlockType::moving_piston))
        if (const auto* be = ctx.peek_block_entity(head_pos)) {
            const auto m = Motion::unpack(be->motion);
            if (m.source && m.extending && m.facing == facing) edit.clear(head_pos);
        }
    edit.moving(pos, s.with_flag(false), {facing, false, true}, sticky_piston);
    const auto head = ctx.block(head_pos);
    if (head.is(BlockType::piston_head) && head.facing() == facing) edit.write(head_pos, blocks::air());
    if (!sticky_piston) {
        edit.finish();
        return;
    }
    // Retracting before the push finished: the block is left where it was going.
    if (const auto carried = pushed_block(ctx, target, facing)) {
        edit.finish_moving(target, *carried);
    } else if (ctx.block(head_pos).is_air()) {
        const auto ts = ctx.block(target);
        if (!ts.is_air() && is_movable(ts)) {
            Resolver structure(ctx, pos, contracts::opposite(facing));
            if (structure.resolve(target)) edit.move(structure, contracts::opposite(facing), {facing, false, false}, std::nullopt, false);
        }
    }
    edit.finish();
}
} // namespace

bool piston_powered(RuleContext& ctx, BlockPos pos, Direction facing) {
    constexpr std::array<Direction, 6> order{Direction::down, Direction::up, Direction::north,
                                             Direction::south, Direction::west, Direction::east};
    for (auto d : order)
        if (d != facing && signal_from(ctx, pos.relative(d), contracts::opposite(d)) > 0) return true;
    // Quasi-connectivity: power reaching the cell above also activates the piston.
    const auto above = pos.relative(Direction::up);
    for (auto d : order)
        if (d != Direction::down && signal_from(ctx, above.relative(d), contracts::opposite(d)) > 0) return true;
    return false;
}

void check_piston(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!is_piston(s.type())) return;
    const bool powered = piston_powered(ctx, pos, s.facing());
    const auto param = static_cast<std::uint8_t>(s.facing());
    if (powered && !s.flag()) queue_block_event(ctx, {pos, s.type(), piston_event::extend, param});
    else if (!powered && s.flag()) queue_block_event(ctx, {pos, s.type(), piston_event::retract, param});
}

void run_piston_event(RuleContext& ctx, const BlockEvent& event, BlockState s) {
    const bool powered = piston_powered(ctx, event.pos, s.facing());
    if (event.event == piston_event::extend) {
        if (s.flag() || !powered) return;
        // A blocked piston waits for the next neighbor update.
        if (!extend(ctx, event.pos, s)) ctx.record(TraceKind::piston_blocked, event.pos);
    } else if (event.event == piston_event::retract) {
        if (!s.flag() || powered) return;
        retract(ctx, event.pos, s);
    }
}

namespace {
constexpr std::int64_t push_step = contracts::fixed_one / 2;

// Moves item entities in the moving block's cell half a block along the
// motion. A retracting base does not move, so it pushes nothing.
void push_entities(RuleContext& ctx, BlockPos pos, Motion motion) {
    if (motion.source && !motion.extending) return;
    const auto o = contracts::offset(motion.extending ? motion.facing : contracts::opposite(motion.facing));
    for (const auto id : ctx.entities_in_cell(pos)) {
        const auto* e = ctx.peek_entity(id);
        if (!e) continue;
        auto next = e->position;
        next.x += o.dx * push_step;
        next.y += o.dy * push_step;
        next.z += o.dz * push_step;
        const auto cell = contracts::block_of(next);
        if (!ctx.chunk_generation(contracts::chunk_of(cell))) continue;
        if (cell != pos && is_solid(ctx.block(cell))) continue;
        const auto from = contracts::chunk_of(contracts::block_of(e->position));
        const auto to = contracts::chunk_of(cell);
        if (from != to)
            ctx.record(TraceKind::entity_changed_chunk, cell, id.value, contracts::pack_chunk(from), contracts::pack_chunk(to));
        ctx.relocate_entity(id, next);
    }
}
} // namespace

void tick_moving_piston(RuleContext& ctx, BlockPos pos) {
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    if (be->progress < move_ticks) {
        const auto motion = Motion::unpack(be->motion);
        ++be->progress;
        push_entities(ctx, pos, motion); // `be` must not be used after entities move.
        return;
    }
    const auto carried = BlockState::decode(be->moving).value_or(blocks::air());
    set_block(ctx, pos, carried); // Removes the moving piston's block entity.
}

} // namespace circuit::gameplay::detail
