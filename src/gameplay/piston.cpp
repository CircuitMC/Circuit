// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <deque>
#include <optional>
#include <set>

namespace circuit::gameplay::detail {
namespace {
constexpr std::size_t push_limit = 12;
constexpr std::uint8_t move_ticks = 2;

struct Structure {
    std::vector<BlockPos> moved;     // In discovery order.
    std::vector<BlockPos> destroyed; // Destroyed (without drops) where a push line reaches them.
};

// Discovers the blocks that move when `start` is pushed or pulled in `dir`.
// Fails if an immovable block (including an unloaded or out-of-world cell)
// lies in a push line or more than 12 blocks would move. A destroyable block
// ends its push line. Slime blocks attach their movable neighbors.
std::optional<Structure> resolve(RuleContext& ctx, BlockPos start, Direction dir, BlockPos piston) {
    Structure out;
    std::set<BlockPos> seen;
    std::deque<BlockPos> queue{start};
    while (!queue.empty()) {
        const auto p = queue.front();
        queue.pop_front();
        if (seen.contains(p)) continue;
        const auto s = ctx.block(p);
        if (s.is_air()) continue;
        if (p == piston) return std::nullopt;
        seen.insert(p);
        if (destroyed_by_piston(s.type())) {
            out.destroyed.push_back(p);
            continue;
        }
        if (!is_movable(s)) return std::nullopt;
        out.moved.push_back(p);
        if (out.moved.size() > push_limit) return std::nullopt;
        queue.push_back(p.relative(dir));
        if (s.is(BlockType::slime_block)) {
            for (auto d : update_order) {
                if (d == dir) continue;
                const auto q = p.relative(d);
                if (q == piston) continue;
                const auto qs = ctx.block(q);
                if (!qs.is_air() && is_movable(qs)) queue.push_back(q);
            }
        }
    }
    return out;
}

// Writes of one piston action. Other roots cannot observe the intermediate
// states; changes are recorded and propagated when the action finishes.
class Edit {
public:
    explicit Edit(RuleContext& ctx) : ctx_(ctx) {}
    void write(BlockPos p, BlockState s) {
        if (!touched_.contains(p)) {
            touched_.insert(p);
            original_.push_back({p, ctx_.block(p)});
        }
        ctx_.write_block(p, s);
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
    void moving(BlockPos p, BlockState carried, Motion motion, bool sticky) {
        if (has_block_entity(ctx_.block(p).type())) ctx_.remove_block_entity(p);
        write(p, blocks::moving_piston(motion.facing, sticky));
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
    void move(const Structure& structure, Direction dir, Motion motion, bool sticky) {
        std::vector<BlockState> states;
        for (auto p : structure.moved) states.push_back(ctx_.block(p));
        for (auto p : structure.destroyed) destroy(p);
        for (auto p : structure.moved) write(p, blocks::air());
        for (std::size_t i = 0; i < structure.moved.size(); ++i)
            moving(structure.moved[i].relative(dir), states[i], motion, sticky);
    }
    void finish() {
        std::vector<BlockPos> changed;
        for (const auto& [p, before] : original_) {
            const auto after = ctx_.block(p);
            if (after == before) continue;
            ctx_.record(TraceKind::block_changed, p, before.raw(), after.raw());
            changed.push_back(p);
        }
        for (auto p : placed_) on_placed(ctx_, p, ctx_.block(p));
        for (auto p : changed) notify_neighbors(ctx_, p);
    }

private:
    RuleContext& ctx_;
    std::set<BlockPos> touched_;
    std::vector<std::pair<BlockPos, BlockState>> original_;
    std::vector<BlockPos> placed_;
};

bool extend(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto facing = s.facing();
    const auto head_pos = pos.relative(facing);
    const bool sticky = s.is(BlockType::sticky_piston);
    const auto structure = resolve(ctx, head_pos, facing, pos);
    if (!structure) return false;
    Edit edit(ctx);
    edit.move(*structure, facing, {facing, true, false}, sticky);
    edit.moving(head_pos, BlockState::of(BlockType::piston_head).with_facing(facing).with_flag(sticky), {facing, true, true}, sticky);
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
    const bool sticky = s.is(BlockType::sticky_piston);
    Edit edit(ctx);
    bool dropped = false;
    if (sticky) {
        // Retracting before the push finished: the block is left where it was going.
        if (const auto carried = pushed_block(ctx, target, facing)) {
            edit.finish_moving(target, *carried);
            dropped = true;
        }
    }
    const auto head = ctx.block(head_pos);
    bool head_moving = false;
    if (head.is(BlockType::moving_piston))
        if (const auto* be = ctx.peek_block_entity(head_pos)) {
            const auto m = Motion::unpack(be->motion);
            head_moving = m.source && m.extending && m.facing == facing;
        }
    if ((head.is(BlockType::piston_head) && head.facing() == facing) || head_moving) edit.clear(head_pos);
    edit.moving(pos, s.with_flag(false), {facing, false, true}, sticky);
    if (sticky && !dropped && ctx.block(head_pos).is_air()) {
        const auto ts = ctx.block(target);
        if (!ts.is_air() && is_movable(ts))
            if (const auto structure = resolve(ctx, target, contracts::opposite(facing), pos))
                edit.move(*structure, contracts::opposite(facing), {facing, false, false}, sticky);
    }
    edit.finish();
}
} // namespace

bool piston_powered(RuleContext& ctx, BlockPos pos, Direction facing) {
    for (auto d : contracts::all_directions) {
        if (d == facing) continue;
        const auto n = pos.relative(d);
        if (emitted_power(ctx, n, ctx.block(n), contracts::opposite(d)) > 0) return true;
    }
    // Quasi-connectivity: power reaching the cell above also activates the piston.
    const auto above = pos.relative(Direction::up);
    for (auto d : contracts::all_directions) {
        const auto n = above.relative(d);
        if (n == pos) continue;
        if (emitted_power(ctx, n, ctx.block(n), contracts::opposite(d)) > 0) return true;
    }
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
        if (cell != pos && is_solid(ctx.block(cell).type())) continue;
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
