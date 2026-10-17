// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/physics.hpp>
#include <algorithm>
#include <optional>
#include <set>

namespace circuit::gameplay::detail {
namespace {
constexpr std::size_t push_limit = 12;
constexpr std::uint8_t move_ticks = 2;

bool sticky(BlockState s) { return s.is(BlockType::slime_block) || s.is(BlockType::honey_block); }

// A moved block arrives without its water (both official servers; push_classes).
BlockState dried(BlockState s) {
    if (s.is(BlockType::trapdoor)) return s.with_level(static_cast<std::uint8_t>(s.level() & 1));
    if (!s.is(BlockType::other)) return s;
    auto props = registry::properties(s.registry_state());
    bool wet = false;
    for (auto& [key, value] : props)
        if (key == "waterlogged" && value == "true") value = "false", wet = true;
    if (!wet) return s;
    const auto block = registry::block_of(s.registry_state());
    const auto state = block ? registry::state_of(*block, props) : std::nullopt;
    return state ? BlockState::registered(*state) : s;
}
// Sticky blocks hold their neighbors, but slime and honey do not hold each other.
bool stick(BlockState a, BlockState b) {
    if ((a.is(BlockType::slime_block) && b.is(BlockType::honey_block)) || (a.is(BlockType::honey_block) && b.is(BlockType::slime_block)))
        return false;
    return sticky(a) || sticky(b);
}

// Structure discovery of the target version as observed in harness traces:
// a line runs from the sticky chain behind its origin through the origin and
// forward until air or a destroyable block; every sticky block in the list
// then branches into lines from its neighbors off the movement axis, in the
// game's direction order (down, up, north, south, west, east).
class Resolver {
public:
    // A pull leaves blocks that pistons only push.
    Resolver(RuleContext& ctx, BlockPos piston, Direction movement, bool pulling = false)
        : ctx_(ctx), piston_(piston), movement_(movement), pulling_(pulling) {}

    bool resolve(BlockPos start) {
        const auto s = ctx_.block(start);
        if (s.is_air()) return true;
        if (!movable(s) || start == piston_) {
            if (destroyed_by_piston(s)) {
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
    bool movable(BlockState s) const { return is_movable(s) && !(pulling_ && push_only(s)); }

    bool line(BlockPos origin) {
        auto state = ctx_.block(origin);
        if (state.is_air() || !movable(state) || origin == piston_ || contains(origin)) return true;
        std::size_t count = 1;
        if (count + moved.size() > push_limit) return false;
        // Sticky blocks drag the chain behind them.
        while (sticky(state)) {
            const auto behind = origin.relative(contracts::opposite(movement_), static_cast<std::int32_t>(count));
            const auto bs = ctx_.block(behind);
            if (bs.is_air() || !stick(state, bs) || !movable(bs) || behind == piston_) break;
            if (++count + moved.size() > push_limit) return false;
            state = bs;
        }
        for (std::size_t k = count; k-- > 0;)
            moved.push_back(origin.relative(contracts::opposite(movement_), static_cast<std::int32_t>(k)));
        for (std::int32_t j = 1;; ++j) {
            const auto ahead = origin.relative(movement_, j);
            const auto as = ctx_.block(ahead);
            if (as.is_air()) return true;
            if (destroyed_by_piston(as)) {
                destroyed.push_back(ahead);
                return true;
            }
            if (!movable(as) || ahead == piston_) return false;
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
            if (ns.is_air() || !stick(ctx_.block(p), ns) || !movable(ns) || n == piston_) continue;
            if (!line(n)) return false;
        }
        return true;
    }

    RuleContext& ctx_;
    BlockPos piston_;
    Direction movement_;
    bool pulling_;
};

// Writes of one piston action, in the target version's order. Every write
// but a destruction sends shape updates at once (observed in target-version
// traces); neighbor
// updates for the changed positions follow when the action finishes, in
// first-write order.
class Edit {
public:
    explicit Edit(RuleContext& ctx) : ctx_(ctx) {}
    // Every write is recorded as it happens, including intermediate states.
    void write(BlockPos p, BlockState s, bool shapes = true) {
        const auto before = ctx_.block(p);
        if (before == s) return;
        ctx_.write_block(p, s);
        ctx_.record(TraceKind::block_changed, p, before.raw(), s.raw());
        if (touched_.insert(p).second) changed_.push_back(p);
        if (shapes) notify_shapes(ctx_, p);
    }
    // Replaces a block, and its block entity if any, with air.
    void clear(BlockPos p) {
        if (has_block_entity(ctx_.block(p).type())) ctx_.remove_block_entity(p);
        write(p, blocks::air());
    }
    // A block destroyed by the move: its drops spawned, then removed (the
    // order of both official servers' traces). The removal sends no shape
    // updates; the moving block written there next does (a door's upper
    // half breaks only then: door_more).
    void destroy(BlockPos p) {
        const auto s = ctx_.block(p);
        drop_items(ctx_, p, s);
        // A registry block's saved block entity goes with it.
        if (has_block_entity(s.type()) || (s.is(BlockType::other) && ctx_.peek_block_entity(p))) ctx_.remove_block_entity(p);
        write(p, blocks::air(), false);
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
        write(p, dried(carried));
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
        if (!ts.is_air() && is_movable(ts) && !push_only(ts)) {
            Resolver structure(ctx, pos, contracts::opposite(facing), true);
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

// A piston that extends or retracts plays its sound at a pitch drawn from
// the level random: one draw each time, after the move (both official
// servers' traces).
void run_piston_event(RuleContext& ctx, const BlockEvent& event, BlockState s) {
    const bool powered = piston_powered(ctx, event.pos, s.facing());
    if (event.event == piston_event::extend) {
        if (s.flag() || !powered) return;
        // A blocked piston waits for the next neighbor update.
        if (!extend(ctx, event.pos, s)) {
            ctx.record(TraceKind::piston_blocked, event.pos);
            return;
        }
        (void)ctx.random_float();
    } else if (event.event == piston_event::retract) {
        if (!s.flag() || powered) return;
        retract(ctx, event.pos, s);
        (void)ctx.random_float();
    }
}

namespace {
using contracts::Vec3;
constexpr double step = 0.5;           // of a move, each tick
constexpr double push_margin = 0.01;   // beyond the moving shape
constexpr double piston_push_limit = 0.51; // a tick, on each axis

// Where a moving piston's block goes, and how far along it is (0 to 1).
struct Move {
    BlockState carried;
    Direction dir;
    bool moves;
    double progress;
};
std::optional<Move> move_of(RuleContext& ctx, BlockPos pos) {
    const auto* be = ctx.peek_block_entity(pos);
    if (!be) return std::nullopt;
    const auto m = Motion::unpack(be->motion);
    const auto carried = BlockState::decode(be->moving).value_or(blocks::air());
    // A retracting piston's base stays where it is; pulled blocks move back.
    return Move{carried, m.extending ? m.facing : contracts::opposite(m.facing), m.extending || !m.source,
                std::min(1.0, static_cast<double>(be->progress) * step)};
}

std::vector<physics::Aabb> boxes_at(RuleContext& ctx, BlockPos pos, const Move& move) {
    std::vector<physics::Aabb> out;
    const auto o = contracts::offset(move.dir);
    const double back = move.moves ? move.progress - 1.0 : 0.0;
    for (const auto& b : physics::collision_boxes(move.carried, ctx.bundle().schema.content_release))
        out.push_back(physics::Aabb{pos.x + b.min_x, pos.y + b.min_y, pos.z + b.min_z, pos.x + b.max_x, pos.y + b.max_y, pos.z + b.max_z}
                          .move(o.dx * back, o.dy * back, o.dz * back));
    return out;
}

// The space a box sweeps through moving `length` along `dir`.
physics::Aabb sweep(const physics::Aabb& b, Direction dir, double length) {
    switch (dir) {
    case Direction::east: return {b.max_x, b.min_y, b.min_z, b.max_x + length, b.max_y, b.max_z};
    case Direction::west: return {b.min_x - length, b.min_y, b.min_z, b.min_x, b.max_y, b.max_z};
    case Direction::up: return {b.min_x, b.max_y, b.min_z, b.max_x, b.max_y + length, b.max_z};
    case Direction::down: return {b.min_x, b.min_y - length, b.min_z, b.max_x, b.min_y, b.max_z};
    case Direction::south: return {b.min_x, b.min_y, b.max_z, b.max_x, b.max_y, b.max_z + length};
    case Direction::north: return {b.min_x, b.min_y, b.min_z - length, b.max_x, b.max_y, b.max_z};
    }
    return b;
}

// How far an entity's box must move along `dir` to leave the swept space.
double needed(const physics::Aabb& area, Direction dir, const physics::Aabb& e) {
    switch (dir) {
    case Direction::east: return area.max_x - e.min_x;
    case Direction::west: return e.max_x - area.min_x;
    case Direction::up: return area.max_y - e.min_y;
    case Direction::down: return e.max_y - area.min_y;
    case Direction::south: return area.max_z - e.min_z;
    case Direction::north: return e.max_z - area.min_z;
    }
    return 0.0;
}

physics::Aabb bounds(const std::vector<physics::Aabb>& boxes) {
    auto b = boxes.front();
    for (const auto& x : boxes)
        b = {std::min(b.min_x, x.min_x), std::min(b.min_y, x.min_y), std::min(b.min_z, x.min_z), std::max(b.max_x, x.max_x),
             std::max(b.max_y, x.max_y), std::max(b.max_z, x.max_z)};
    return b;
}

// Moves an entity as a piston does: against the blocks, at most 0.51 on
// each axis in one tick from all pistons together.
void piston_move(RuleContext& ctx, EntityId id, Vec3 push) {
    auto* e = ctx.entity(id);
    if (e->piston_tick != ctx.now()) {
        e->piston_tick = ctx.now();
        e->piston_push = {};
    }
    const auto limit = [](double& total, double& v) {
        const double next = std::clamp(total + v, -piston_push_limit, piston_push_limit);
        v = next - total;
        total = next;
    };
    limit(e->piston_push.x, push.x);
    limit(e->piston_push.y, push.y);
    limit(e->piston_push.z, push.z);
    if (push == Vec3{}) return;
    const auto box = entity_box(*e);
    const auto start = e->position;
    auto p = start;
    auto v = e->velocity;
    e->on_ground = move_entity_by(ctx, p, v, push, box.max_x - box.min_x, box.max_y - box.min_y);
    e->velocity = v;
    if (p == start) return;
    const auto from = contracts::chunk_of(contracts::block_of(start));
    const auto to = contracts::chunk_of(contracts::block_of(p));
    if (from != to) ctx.record(TraceKind::entity_changed_chunk, contracts::block_of(p), id.value, contracts::pack_chunk(from), contracts::pack_chunk(to));
    ctx.relocate_entity(id, p);
}

// Entities in the space the moving block sweeps through this step are
// pushed along to 0.01 beyond it (at most the step); a moving slime block
// also sets their speed along its motion to one block a tick (both official
// servers; piston_entities, slime_entities, item_piston_push).
void push_entities(RuleContext& ctx, BlockPos pos) {
    const auto move = move_of(ctx, pos);
    if (!move || !move->moves) return;
    const auto boxes = boxes_at(ctx, pos, *move);
    if (boxes.empty()) return;
    const auto whole = bounds(boxes);
    const auto ahead = sweep(whole, move->dir, step);
    const physics::Aabb area{std::min(whole.min_x, ahead.min_x), std::min(whole.min_y, ahead.min_y), std::min(whole.min_z, ahead.min_z),
                             std::max(whole.max_x, ahead.max_x), std::max(whole.max_y, ahead.max_y), std::max(whole.max_z, ahead.max_z)};
    const bool slime = move->carried.is(BlockType::slime_block);
    const auto o = contracts::offset(move->dir);
    for (const auto id : entities_in_section_order(ctx, entities_intersecting(ctx, area))) {
        auto* e = ctx.entity(id);
        if (!e) continue;
        if (slime) {
            auto& v = e->velocity;
            if (o.dx != 0) v.x = o.dx;
            if (o.dy != 0) v.y = o.dy;
            if (o.dz != 0) v.z = o.dz;
        }
        const auto eb = entity_box(*e);
        double d = 0.0;
        for (const auto& b : boxes) {
            const auto swept = sweep(b, move->dir, step);
            if (!swept.intersects(eb)) continue;
            d = std::max(d, needed(swept, move->dir, eb));
            if (d >= step) break;
        }
        if (d <= 0.0) continue;
        const double amount = std::min(d, step) + push_margin;
        piston_move(ctx, id, {o.dx * amount, o.dy * amount, o.dz * amount});
    }
}

// Honey moving sideways then carries what stands on it: entities on the
// ground whose box meets the space from its top to 1.5 above its cell, with
// their position over the cell, move the step along with it (both official
// servers; honey_more).
void carry_stuck(RuleContext& ctx, BlockPos pos) {
    const auto move = move_of(ctx, pos);
    if (!move || !move->moves || !move->carried.is(BlockType::honey_block) || !contracts::is_horizontal(move->dir)) return;
    const auto boxes = boxes_at(ctx, pos, *move);
    if (boxes.empty()) return;
    const auto o = contracts::offset(move->dir);
    const double back = move->progress - 1.0;
    const physics::Aabb area = physics::Aabb{static_cast<double>(pos.x), bounds(boxes).max_y, static_cast<double>(pos.z),
                                             pos.x + 1.0, pos.y + 1.500001, pos.z + 1.0}
                                   .move(o.dx * back, 0.0, o.dz * back);
    for (const auto id : entities_in_section_order(ctx, entities_intersecting(ctx, area))) {
        const auto* e = ctx.peek_entity(id);
        if (!e || !e->on_ground) continue;
        const auto& p = e->position;
        if (p.x < area.min_x || p.x > area.max_x || p.z < area.min_z || p.z > area.max_z) continue;
        piston_move(ctx, id, {o.dx * step, 0.0, o.dz * step});
    }
}
} // namespace

std::vector<registry::Box> moving_block_boxes(RuleContext& ctx, BlockPos pos) {
    std::vector<registry::Box> out;
    const auto move = move_of(ctx, pos);
    if (!move) return out;
    for (const auto& b : boxes_at(ctx, pos, *move))
        out.push_back({b.min_x - pos.x, b.min_y - pos.y, b.min_z - pos.z, b.max_x - pos.x, b.max_y - pos.y, b.max_z - pos.z});
    return out;
}

void tick_moving_piston(RuleContext& ctx, BlockPos pos) {
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    if (be->progress < move_ticks) {
        // Entities are pushed from where the block stood, then it moves on.
        push_entities(ctx, pos); // `be` must not be used after entities move.
        carry_stuck(ctx, pos);
        ++ctx.block_entity(pos)->progress;
        return;
    }
    const auto carried = BlockState::decode(be->moving).value_or(blocks::air());
    set_block(ctx, pos, dried(carried)); // Removes the moving piston's block entity.
}

} // namespace circuit::gameplay::detail
