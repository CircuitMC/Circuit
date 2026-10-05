// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <deque>
#include <optional>
#include <set>

namespace circuit::gameplay::detail {
namespace {
constexpr std::size_t push_limit = 12;

// Discovers the blocks that move when `start` is pushed or pulled in `dir`.
// Fails if an immovable block (including an unloaded or out-of-world cell)
// lies in a push line or the structure exceeds the push limit. Slime blocks
// attach their movable neighbors; immovable neighbors simply stay.
std::optional<std::vector<BlockPos>> resolve(RuleContext& ctx, BlockPos start, Direction dir, BlockPos piston) {
    std::vector<BlockPos> order;
    std::set<BlockPos> seen;
    std::deque<BlockPos> queue{start};
    while (!queue.empty()) {
        const auto p = queue.front();
        queue.pop_front();
        if (seen.contains(p)) continue;
        const auto s = ctx.block(p);
        if (s.is_air()) continue;
        if (p == piston || !is_movable(s)) return std::nullopt;
        seen.insert(p);
        order.push_back(p);
        if (order.size() > push_limit) return std::nullopt;
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
    return order;
}

// Raw writes of one piston action. Other roots cannot observe the
// intermediate states; changes are recorded and propagated afterwards.
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
    void move(const std::vector<BlockPos>& structure, Direction dir) {
        std::vector<BlockState> states;
        for (auto p : structure) states.push_back(ctx_.block(p));
        for (auto p : structure) write(p, blocks::air());
        for (std::size_t i = 0; i < structure.size(); ++i) {
            const auto destination = structure[i].relative(dir);
            write(destination, states[i]);
            placed_.push_back(destination);
        }
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
    const auto structure = resolve(ctx, head_pos, facing, pos);
    if (!structure) return false;
    Edit edit(ctx);
    edit.move(*structure, facing);
    edit.write(head_pos, BlockState::of(BlockType::piston_head).with_facing(facing).with_flag(s.is(BlockType::sticky_piston)));
    edit.write(pos, s.with_flag(true));
    edit.finish();
    return true;
}

void retract(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto facing = s.facing();
    const auto head_pos = pos.relative(facing);
    Edit edit(ctx);
    const auto head = ctx.block(head_pos);
    if (head.is(BlockType::piston_head) && head.facing() == facing) edit.write(head_pos, blocks::air());
    edit.write(pos, s.with_flag(false));
    if (s.is(BlockType::sticky_piston) && ctx.block(head_pos).is_air()) {
        const auto target = pos.relative(facing, 2);
        const auto ts = ctx.block(target);
        if (!ts.is_air() && is_movable(ts)) {
            // A sticky piston that cannot pull its structure still retracts.
            if (const auto structure = resolve(ctx, target, contracts::opposite(facing), pos))
                edit.move(*structure, contracts::opposite(facing));
        }
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
    // A piston that acted this tick settles until its re-check tick.
    if (ctx.has_tick(TickList::block, pos, s.type())) return;
    const bool powered = piston_powered(ctx, pos, s.facing());
    const auto param = static_cast<std::uint8_t>(s.facing());
    if (powered && !s.flag()) queue_block_event(ctx, {pos, s.type(), piston_event::extend, param});
    else if (!powered && s.flag()) queue_block_event(ctx, {pos, s.type(), piston_event::retract, param});
}

void run_piston_event(RuleContext& ctx, const BlockEvent& event, BlockState s) {
    const bool powered = piston_powered(ctx, event.pos, s.facing());
    if (event.event == piston_event::extend) {
        if (s.flag() || !powered) return;
        if (!extend(ctx, event.pos, s)) {
            // A blocked piston waits for the next neighbor update.
            ctx.record(TraceKind::piston_blocked, event.pos);
            return;
        }
    } else if (event.event == piston_event::retract) {
        if (!s.flag() || powered) return;
        retract(ctx, event.pos, s);
    } else {
        return;
    }
    // At most one action per piston and tick: the power check resumes next tick.
    schedule(ctx, TickList::block, event.pos, s.type(), 1, 0);
}

} // namespace circuit::gameplay::detail
