// SPDX-License-Identifier: AGPL-3.0-only
// Rails, powered, activator and detector rails, as both official servers run
// them (checked against their traces; minecraft.wiki "Rail", "Powered Rail",
// "Activator Rail", "Detector Rail").
//
// A rail's shape gives its two ends; a raised end leads one block up. A
// neighbor on a horizontal side counts as a rail when there is one in that
// cell, the cell above or the cell below. A rail that appears (placed, set
// or moved in) takes a shape from the rails beside it that can join it, then
// those rails at its ends join it. A rail can join another when one of its
// ends already leads to it or when fewer than two of its ends lead to rails
// that lead back. Plain rails curve; with rails on three or four sides they
// prefer, last to first, south-east, south-west, north-east and north-west,
// or, while a signal reaches them, the reverse. A straight shape rises
// toward a side where a rail lies a block higher. A plain rail with rails on
// three sides takes its shape again when an update comes from a signal
// source. Powered and activator rails are powered by a signal, or along their
// line through up to eight powered rails of their kind from one that has a
// signal. Rails need a sturdy top below them, an ascending rail also on the
// side it rises toward; without it they break with their drop.
//
// A detector rail is pressed while a minecart's box meets the middle of its
// cell (0.2 in from each side, up to 0.8 above its bottom): an entity inside
// it while it is unpowered, a placement and its check 20 ticks after each
// press look. A change of its power updates the rails at its ends, around
// it and below it, then the comparators that read it, which read 0 (no
// minecart has contents).
#include "internal.hpp"
#include <array>
#include <optional>
#include <vector>

namespace circuit::gameplay::detail {
namespace {
using blocks::RailKind;
using blocks::RailShape;
using enum RailShape;

RailShape shape_of(BlockState s) { return static_cast<RailShape>(s.level()); }
bool is_rail(BlockState s) { return s.is(BlockType::rail); }
bool curves(BlockState s) { return s.variant() == static_cast<std::uint8_t>(RailKind::rail); }
bool ascending(RailShape r) { return r >= ascending_east && r <= ascending_south; }
BlockPos up(BlockPos p) { return p.relative(Direction::up); }

// The cells a shape leads to, in the order the rules visit them.
std::vector<BlockPos> ends_of(BlockPos p, RailShape r) {
    const auto n = p.relative(Direction::north), s = p.relative(Direction::south), w = p.relative(Direction::west),
               e = p.relative(Direction::east);
    switch (r) {
    case north_south: return {n, s};
    case east_west: return {w, e};
    case ascending_east: return {w, up(e)};
    case ascending_west: return {up(w), e};
    case ascending_north: return {up(n), s};
    case ascending_south: return {n, up(s)};
    case south_east: return {e, s};
    case south_west: return {w, s};
    case north_west: return {w, n};
    case north_east: return {e, n};
    }
    return {};
}

class Track {
public:
    Track(RuleContext& ctx, BlockPos pos) : ctx_(ctx), pos_(pos), state_(ctx.block(pos)), ends_(ends_of(pos, shape_of(state_))) {}

    [[nodiscard]] BlockPos pos() const { return pos_; }

    // The rail in a cell, or a block above or below it.
    static std::optional<Track> find(RuleContext& ctx, BlockPos p) {
        for (const auto q : {p, up(p), p.relative(Direction::down)})
            if (is_rail(ctx.block(q))) return Track(ctx, q);
        return std::nullopt;
    }

    // Whether an end leads to the column of `p`.
    [[nodiscard]] bool leads_to(BlockPos p) const {
        for (const auto& e : ends_)
            if (e.x == p.x && e.z == p.z) return true;
        return false;
    }
    // Keeps the ends that lead to rails leading back.
    void prune() {
        for (std::size_t i = 0; i < ends_.size();) {
            const auto r = find(ctx_, ends_[i]);
            if (r && r->leads_to(pos_)) {
                ends_[i] = r->pos_;
                ++i;
            } else {
                ends_.erase(ends_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }
    [[nodiscard]] bool can_join(BlockPos p) const { return leads_to(p) || ends_.size() != 2; }

    // Sides with a rail in the cell, above or below it.
    [[nodiscard]] int potential_sides() const {
        int n = 0;
        for (const auto d : {Direction::north, Direction::south, Direction::west, Direction::east}) {
            const auto p = pos_.relative(d);
            if (is_rail(ctx_.block(p)) || is_rail(ctx_.block(up(p))) || is_rail(ctx_.block(p.relative(Direction::down)))) ++n;
        }
        return n;
    }

    // Takes a shape from the rails beside it, then lets those at its ends join it.
    void place(bool powered, bool always) {
        const auto n = pos_.relative(Direction::north), s = pos_.relative(Direction::south), w = pos_.relative(Direction::west),
                   e = pos_.relative(Direction::east);
        const bool fn = joinable(n), fs = joinable(s), fw = joinable(w), fe = joinable(e);
        const bool ns = fn || fs, ew = fw || fe;
        const bool se = fs && fe, sw = fs && fw, ne = fn && fe, nw = fn && fw;
        std::optional<RailShape> shape;
        if (ns && !ew) shape = north_south;
        if (ew && !ns) shape = east_west;
        if (curves(state_)) {
            if (se && !fn && !fw) shape = south_east;
            if (sw && !fn && !fe) shape = south_west;
            if (nw && !fs && !fe) shape = north_west;
            if (ne && !fs && !fw) shape = north_east;
        }
        if (!shape) {
            if (ns && ew) shape = shape_of(state_);
            else if (ns) shape = north_south;
            else if (ew) shape = east_west;
            if (curves(state_)) {
                const std::array<std::pair<bool, RailShape>, 4> unpowered{{{nw, north_west}, {ne, north_east}, {sw, south_west}, {se, south_east}}};
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto& [has, curve] = unpowered[powered ? 3 - i : i];
                    if (has) shape = curve;
                }
            }
        }
        shape = rise(shape);
        if (!shape) shape = shape_of(state_);
        ends_ = ends_of(pos_, *shape);
        const auto next = state_.with_level(static_cast<std::uint8_t>(*shape));
        if (!always && ctx_.block(pos_) == next) return;
        state_ = next;
        set_block(ctx_, pos_, next);
        for (std::size_t i = 0; i < ends_.size(); ++i)
            if (auto r = find(ctx_, ends_[i])) {
                r->prune();
                if (r->can_join(pos_)) r->join(pos_);
            }
    }

private:
    // Whether the rail on side cell `p` can join this one.
    bool joinable(BlockPos p) {
        auto r = find(ctx_, p);
        if (!r) return false;
        r->prune();
        return r->can_join(pos_);
    }
    // A straight shape rises toward a side with a rail a block higher (south and west last).
    std::optional<RailShape> rise(std::optional<RailShape> shape) const {
        const auto railed = [&](Direction d) { return is_rail(ctx_.block(up(pos_.relative(d)))); };
        if (shape == north_south) {
            if (railed(Direction::north)) shape = ascending_north;
            if (railed(Direction::south)) shape = ascending_south;
        } else if (shape == east_west) {
            if (railed(Direction::east)) shape = ascending_east;
            if (railed(Direction::west)) shape = ascending_west;
        }
        return shape;
    }
    // Adds an end toward `p` and takes the shape the ends give.
    void join(BlockPos p) {
        ends_.push_back(p);
        const bool fn = leads_to(pos_.relative(Direction::north)), fs = leads_to(pos_.relative(Direction::south)),
                   fw = leads_to(pos_.relative(Direction::west)), fe = leads_to(pos_.relative(Direction::east));
        std::optional<RailShape> shape;
        if (fn || fs) shape = north_south;
        if (fw || fe) shape = east_west;
        if (curves(state_)) {
            if (fs && fe && !fn && !fw) shape = south_east;
            if (fs && fw && !fn && !fe) shape = south_west;
            if (fn && fw && !fs && !fe) shape = north_west;
            if (fn && fe && !fs && !fw) shape = north_east;
        }
        shape = rise(shape);
        if (!shape) shape = north_south;
        state_ = state_.with_level(static_cast<std::uint8_t>(*shape));
        set_block(ctx_, pos_, state_);
    }

    RuleContext& ctx_;
    BlockPos pos_;
    BlockState state_;
    std::vector<BlockPos> ends_;
};

// Whether a rail of `kind` at `p` lying along `axis` is powered and has a
// signal, or passes power on from farther along.
bool line_power(RuleContext& ctx, BlockPos pos, BlockState s, bool forward, int distance);
bool powered_along(RuleContext& ctx, BlockPos p, bool forward, int distance, RailShape axis, std::uint8_t kind) {
    const auto s = ctx.block(p);
    if (!is_rail(s) || s.variant() != kind) return false;
    const auto r = shape_of(s);
    if (axis == east_west && (r == north_south || r == ascending_north || r == ascending_south)) return false;
    if (axis == north_south && (r == east_west || r == ascending_east || r == ascending_west)) return false;
    if (!s.flag()) return false;
    return has_neighbor_signal(ctx, p) || line_power(ctx, p, s, forward, distance + 1);
}
// Power reaching the rail along its line in one direction (`forward`: south
// along north-south rails, west along east-west ones), within eight rails.
bool line_power(RuleContext& ctx, BlockPos pos, BlockState s, bool forward, int distance) {
    if (distance >= 8) return false;
    auto next = pos;
    bool below_too = true;
    auto axis = shape_of(s);
    switch (shape_of(s)) {
    case north_south: next.z += forward ? 1 : -1; break;
    case east_west: next.x += forward ? -1 : 1; break;
    case ascending_east:
        if (forward) next.x -= 1;
        else next.x += 1, next.y += 1, below_too = false;
        axis = east_west;
        break;
    case ascending_west:
        if (forward) next.x -= 1, next.y += 1, below_too = false;
        else next.x += 1;
        axis = east_west;
        break;
    case ascending_north:
        if (forward) next.z += 1;
        else next.z -= 1, next.y += 1, below_too = false;
        axis = north_south;
        break;
    case ascending_south:
        if (forward) next.z += 1, next.y += 1, below_too = false;
        else next.z -= 1;
        axis = north_south;
        break;
    default: return false;
    }
    if (powered_along(ctx, next, forward, distance, axis, s.variant())) return true;
    return below_too && powered_along(ctx, next.relative(Direction::down), forward, distance, axis, s.variant());
}

bool rigid_top(RuleContext& ctx, BlockPos p) { return supports_dust(ctx.block(p)); }

bool unsupported(RuleContext& ctx, BlockPos pos, RailShape r) {
    if (!rigid_top(ctx, pos.relative(Direction::down))) return true;
    switch (r) {
    case ascending_east: return !rigid_top(ctx, pos.relative(Direction::east));
    case ascending_west: return !rigid_top(ctx, pos.relative(Direction::west));
    case ascending_north: return !rigid_top(ctx, pos.relative(Direction::north));
    case ascending_south: return !rigid_top(ctx, pos.relative(Direction::south));
    default: return false;
    }
}

// Signal sources, whose updates make a plain rail on three sides take its shape again.
bool signal_source(BlockState s) {
    switch (s.type()) {
    case BlockType::lever: case BlockType::redstone_wire: case BlockType::repeater: case BlockType::comparator:
    case BlockType::observer: case BlockType::redstone_block: case BlockType::redstone_torch: case BlockType::redstone_wall_torch:
    case BlockType::button: case BlockType::pressure_plate: case BlockType::weighted_plate: case BlockType::target:
    case BlockType::daylight_detector:
        return true;
    case BlockType::rail: return s.variant() == static_cast<std::uint8_t>(RailKind::detector);
    default: return false;
    }
}

bool is_detector(BlockState s) { return is_rail(s) && s.variant() == static_cast<std::uint8_t>(RailKind::detector); }

bool cart_on(RuleContext& ctx, BlockPos pos) {
    const physics::Aabb area{pos.x + 0.2, static_cast<double>(pos.y), pos.z + 0.2, pos.x + 0.8, pos.y + 0.8, pos.z + 0.8};
    for (const auto id : entities_intersecting(ctx, area))
        if (const auto* e = ctx.peek_entity(id); e && e->kind == EntityKind::minecart) return true;
    return false;
}

void check_detector(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool pressed = cart_on(ctx, pos);
    if (pressed != s.flag()) {
        const auto next = s.with_flag(pressed);
        set_block(ctx, pos, next);
        // Each block at its ends gets an update as from itself.
        for (const auto& end : ends_of(pos, shape_of(next))) notify_single(ctx, end, pos, ctx.block(end));
        notify_neighbors(ctx, pos, std::nullopt, next);
        notify_neighbors(ctx, pos.relative(Direction::down), std::nullopt, next);
    }
    if (pressed) schedule(ctx, TickList::block, pos, BlockType::rail, 20, 0);
    container_changed(ctx, pos);
}

void check_power(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool on = has_neighbor_signal(ctx, pos) || line_power(ctx, pos, s, true, 0) || line_power(ctx, pos, s, false, 0);
    if (on == s.flag()) return;
    set_block(ctx, pos, s.with_flag(on));
    notify_neighbors(ctx, pos.relative(Direction::down), std::nullopt, s);
    if (ascending(shape_of(s))) notify_neighbors(ctx, up(pos), std::nullopt, s);
}

// A neighbor update with `cause` (null: the rail's own check after it appears).
void update_rail(RuleContext& ctx, BlockPos pos, BlockState s, std::optional<BlockState> cause) {
    switch (static_cast<RailKind>(s.variant())) {
    case RailKind::rail:
        if (cause && signal_source(*cause) && Track(ctx, pos).potential_sides() == 3) Track(ctx, pos).place(has_neighbor_signal(ctx, pos), false);
        break;
    case RailKind::powered: case RailKind::activator: check_power(ctx, pos, s); break;
    case RailKind::detector: break;
    }
}

void rail_neighbor_changed(RuleContext& ctx, BlockPos pos, std::optional<BlockState> cause) {
    const auto s = ctx.block(pos);
    if (!is_rail(s)) return;
    if (unsupported(ctx, pos, shape_of(s))) {
        destroy_block(ctx, pos);
        return;
    }
    update_rail(ctx, pos, s, cause);
}
} // namespace

void rail_placed(RuleContext& ctx, BlockPos pos) {
    Track(ctx, pos).place(has_neighbor_signal(ctx, pos), true);
    // The others check themselves at once, as a neighbor update would.
    const auto s = ctx.block(pos);
    if (is_rail(s) && !curves(s)) rail_neighbor_changed(ctx, pos, std::nullopt);
    if (const auto now = ctx.block(pos); is_detector(now)) check_detector(ctx, pos, now);
}

void detector_entity_inside(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (is_detector(s) && !s.flag()) check_detector(ctx, pos, s);
}

void tick_rail(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (is_detector(s) && s.flag()) check_detector(ctx, pos, s);
}

bool is_detector_rail(BlockState s) noexcept { return is_detector(s); }

void check_rail(RuleContext& ctx, BlockPos pos, BlockState cause) { rail_neighbor_changed(ctx, pos, cause); }

void rail_removed(RuleContext& ctx, BlockPos pos, BlockState previous) {
    if (ascending(shape_of(previous))) notify_neighbors(ctx, up(pos), std::nullopt, previous);
    if (!curves(previous)) {
        notify_neighbors(ctx, pos, std::nullopt, previous);
        notify_neighbors(ctx, pos.relative(Direction::down), std::nullopt, previous);
    }
}

bool rail_supported(RuleContext& ctx, BlockPos pos) { return rigid_top(ctx, pos.relative(Direction::down)); }

std::uint16_t rail_item(BlockState s) {
    switch (static_cast<RailKind>(s.variant())) {
    case RailKind::powered: return items::powered_rail;
    case RailKind::activator: return items::activator_rail;
    case RailKind::detector: return items::detector_rail;
    default: return items::rail;
    }
}

} // namespace circuit::gameplay::detail
