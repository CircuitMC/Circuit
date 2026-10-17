// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
constexpr std::array<Direction, 4> plane{Direction::north, Direction::east, Direction::south, Direction::west};
constexpr std::array<Direction, 6> all_in_order{Direction::down, Direction::up, Direction::north,
                                                Direction::south, Direction::west, Direction::east};

std::uint8_t wire_power(BlockState s) { return s.is(BlockType::redstone_wire) ? s.level() : 0; }

// Blocks dust connects to on side `d` (from the dust): other dust always;
// repeaters along their axis, an observer whose output faces the dust, and the
// other sources on every side. Without a side (a step), only dust.
bool connects_to(BlockState n, std::optional<Direction> d) {
    if (n.is(BlockType::redstone_wire)) return true;
    if (!d) return false;
    switch (n.type()) {
    case BlockType::repeater: return n.facing() == *d || n.facing() == contracts::opposite(*d);
    case BlockType::observer: return n.facing() == *d;
    case BlockType::lever: case BlockType::redstone_block: case BlockType::comparator: case BlockType::button:
    case BlockType::redstone_torch: case BlockType::redstone_wall_torch: case BlockType::pressure_plate:
    case BlockType::weighted_plate: case BlockType::target: case BlockType::daylight_detector:
        return true;
    default: return false;
    }
}

// Power dust takes from its surroundings: the strongest power its neighbors
// give it (dust giving nothing), and one less than the strongest dust it
// reaches on its level or one step up or down.
std::uint8_t wire_target(RuleContext& ctx, BlockPos pos) {
    const int best = best_neighbor_signal(ctx, pos, false);
    int wires = 0;
    if (best < 15) {
        const bool open_above = !is_conductor(ctx.block(pos.relative(Direction::up)));
        for (const auto d : plane) {
            const auto n = pos.relative(d);
            const auto ns = ctx.block(n);
            wires = std::max<int>(wires, wire_power(ns));
            if (is_conductor(ns)) {
                if (open_above) wires = std::max<int>(wires, wire_power(ctx.block(n.relative(Direction::up))));
            } else {
                wires = std::max<int>(wires, wire_power(ctx.block(n.relative(Direction::down))));
            }
        }
    }
    return static_cast<std::uint8_t>(std::max(best, wires - 1));
}

// A power change updates the neighbors of the dust and of each of its six
// neighbors, in hash-set order (observed in target-version traces).
void power_set_updates(RuleContext& ctx, BlockPos pos) {
    std::vector<BlockPos> around{pos};
    for (auto d : contracts::all_directions) around.push_back(pos.relative(d));
    for (const auto p : hash_order(around, around.size())) notify_neighbors(ctx, p);
}

void check_corner(RuleContext& ctx, BlockPos pos) {
    if (!ctx.block(pos).is(BlockType::redstone_wire)) return;
    notify_neighbors(ctx, pos);
    for (const auto d : all_in_order) notify_neighbors(ctx, pos.relative(d));
}

// Dust placed or removed updates the dust around it, also one step up or down.
void neighboring_wire_corners(RuleContext& ctx, BlockPos pos) {
    for (const auto d : plane) check_corner(ctx, pos.relative(d));
    for (const auto d : plane) {
        const auto n = pos.relative(d);
        check_corner(ctx, n.relative(is_conductor(ctx.block(n)) ? Direction::up : Direction::down));
    }
}

std::uint32_t repeater_delay(BlockState s) { return static_cast<std::uint32_t>(s.delay()) * 2; }

// Documented, and observed in target-version traces: a repeater feeding another
// diode schedules at -3, one turning off at -2, otherwise -1.
std::int8_t repeater_priority(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (feeds_diode(ctx, pos, s)) return -3;
    return s.flag() ? -2 : -1;
}
// The turn-off scheduled by pulse extension is not raised for a diode in front.
constexpr std::int8_t pulse_end_priority = -2;
} // namespace

std::uint8_t diode_input(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto behind = pos.relative(contracts::opposite(s.facing()));
    const auto power = signal_from(ctx, behind, s.facing());
    if (power >= 15) return power;
    // Diodes read dust behind them whatever its shape.
    return std::max(power, wire_power(ctx.block(behind)));
}

bool is_dot(BlockState s) {
    return s.is(BlockType::redstone_wire) && std::ranges::none_of(plane, [&](Direction d) { return s.wire_connected(d); });
}
bool is_cross(BlockState s) {
    return s.is(BlockType::redstone_wire) && std::ranges::all_of(plane, [&](Direction d) { return s.wire_connected(d); });
}

WireSide connecting_side(RuleContext& ctx, BlockPos pos, Direction d, bool open_above) {
    const auto side = pos.relative(d);
    const auto s = ctx.block(side);
    // Climbing: dust on top of the block at the side.
    if (open_above && supports_dust(s) && connects_to(ctx.block(side.relative(Direction::up)), std::nullopt))
        return face_sturdy(s, contracts::opposite(d)) ? WireSide::up : WireSide::side;
    if (connects_to(s, d)) return WireSide::side;
    // Descending: dust below the side, unless the side block is a conductor.
    if (!is_conductor(s) && connects_to(ctx.block(side.relative(Direction::down)), std::nullopt)) return WireSide::side;
    return WireSide::none;
}

BlockState fresh_wire_shape(RuleContext& ctx, BlockPos pos, BlockState s, bool keep_dot) {
    const bool open_above = !is_conductor(ctx.block(pos.relative(Direction::up)));
    auto out = s.with_wire_shape(0);
    for (const auto d : plane) out = out.with_wire_side(d, connecting_side(ctx, pos, d, open_above));
    // A dot stays a dot while nothing connects; otherwise a lone connection
    // extends to the opposite side and no connection makes a cross.
    if (keep_dot && is_dot(out)) return out;
    const bool n = out.wire_connected(Direction::north), so = out.wire_connected(Direction::south);
    const bool e = out.wire_connected(Direction::east), w = out.wire_connected(Direction::west);
    const bool no_ns = !n && !so, no_ew = !e && !w;
    if (!w && no_ns) out = out.with_wire_side(Direction::west, WireSide::side);
    if (!e && no_ns) out = out.with_wire_side(Direction::east, WireSide::side);
    if (!n && no_ew) out = out.with_wire_side(Direction::north, WireSide::side);
    if (!so && no_ew) out = out.with_wire_side(Direction::south, WireSide::side);
    return out;
}

void wire_shape_update(RuleContext& ctx, BlockPos pos, Direction d) {
    const auto s = ctx.block(pos);
    if (d == Direction::down) {
        if (!supports_dust(ctx.block(pos.relative(Direction::down)))) destroy_block(ctx, pos);
        return;
    }
    BlockState next;
    if (d == Direction::up) {
        next = fresh_wire_shape(ctx, pos, s, is_dot(s));
    } else {
        const auto side = connecting_side(ctx, pos, d, !is_conductor(ctx.block(pos.relative(Direction::up))));
        if ((side != WireSide::none) == s.wire_connected(d) && !is_cross(s)) next = s.with_wire_side(d, side);
        else next = fresh_wire_shape(ctx, pos, s, false);
    }
    if (next != s) set_block_quiet(ctx, pos, next);
}

void indirect_shapes(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!s.is(BlockType::redstone_wire)) return;
    for (const auto d : plane) {
        if (!s.wire_connected(d) || ctx.block(pos.relative(d)).is(BlockType::redstone_wire)) continue;
        for (const auto v : {Direction::down, Direction::up}) {
            const auto diagonal = pos.relative(d).relative(v);
            if (ctx.block(diagonal).is(BlockType::redstone_wire))
                notify_shape(ctx, diagonal, diagonal.relative(contracts::opposite(d)));
        }
    }
}

void update_wire(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::redstone_wire)) return;
    const auto power = wire_target(ctx, pos);
    if (power == s.level()) return;
    set_block_quiet(ctx, pos, s.with_level(power));
    power_set_updates(ctx, pos);
}

void wire_placed(RuleContext& ctx, BlockPos pos) {
    update_wire(ctx, pos);
    notify_neighbors(ctx, pos.relative(Direction::up));
    notify_neighbors(ctx, pos.relative(Direction::down));
    neighboring_wire_corners(ctx, pos);
}

void wire_removed(RuleContext& ctx, BlockPos pos, BlockState previous) {
    for (const auto d : all_in_order) notify_neighbors(ctx, pos.relative(d));
    // The power the dust would have now, compared with what it had.
    if (wire_target(ctx, pos) != previous.level()) power_set_updates(ctx, pos);
    neighboring_wire_corners(ctx, pos);
}

void toggle_wire(RuleContext& ctx, BlockPos pos, BlockState s) {
    // A cross becomes a dot and a dot a cross, as far as its neighbors allow.
    const auto base = is_cross(s) ? s.with_wire_shape(0)
                                  : s.with_wire_shape(0)
                                        .with_wire_side(Direction::north, WireSide::side)
                                        .with_wire_side(Direction::south, WireSide::side)
                                        .with_wire_side(Direction::west, WireSide::side)
                                        .with_wire_side(Direction::east, WireSide::side);
    const auto next = fresh_wire_shape(ctx, pos, base, is_dot(base));
    if (next == s) return;
    set_block(ctx, pos, next);
    // Conductors that the dust now points into, or no longer does, update around.
    for (const auto d : plane)
        if (s.wire_connected(d) != next.wire_connected(d) && is_conductor(ctx.block(pos.relative(d))))
            notify_neighbors(ctx, pos.relative(d), contracts::opposite(d));
}

bool feeds_diode(RuleContext& ctx, BlockPos pos, BlockState s) {
    // Diodes face their output side.
    const auto front = ctx.block(pos.relative(s.facing()));
    return (front.is(BlockType::repeater) || front.is(BlockType::comparator))
        && front.facing() != contracts::opposite(s.facing());
}

bool repeater_input(RuleContext& ctx, BlockPos pos, BlockState s) { return diode_input(ctx, pos, s) > 0; }

bool repeater_locked_now(RuleContext& ctx, BlockPos pos, BlockState s) {
    for (const auto side : {contracts::clockwise(s.facing()), contracts::counter_clockwise(s.facing())}) {
        const auto n = pos.relative(side);
        const auto ns = ctx.block(n);
        if ((ns.is(BlockType::repeater) || ns.is(BlockType::comparator)) && direct(ctx, n, ns, contracts::opposite(side), true) > 0)
            return true;
    }
    return false;
}

void check_repeater(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::repeater) || repeater_locked_now(ctx, pos, s)) return;
    if (repeater_input(ctx, pos, s) != s.flag())
        schedule(ctx, TickList::block, pos, BlockType::repeater, repeater_delay(s), repeater_priority(ctx, pos, s));
}

void tick_repeater(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (repeater_locked_now(ctx, pos, s)) return;
    const bool input = repeater_input(ctx, pos, s);
    // The state change updates the block in front (see on_placed).
    if (s.flag() && !input) {
        set_block_quiet(ctx, pos, s.with_flag(false));
    } else if (!s.flag()) {
        set_block_quiet(ctx, pos, s.with_flag(true));
        // A pulse shorter than the delay is extended to the delay.
        if (!input) schedule(ctx, TickList::block, pos, BlockType::repeater, repeater_delay(s), pulse_end_priority);
    }
}

void repeater_shape_update(RuleContext& ctx, BlockPos pos, Direction d) {
    const auto s = ctx.block(pos);
    if (d == Direction::down) {
        if (!supports_diode(ctx.block(pos.relative(Direction::down)))) destroy_block(ctx, pos);
        return;
    }
    // A diode at either side locks the repeater (repeater_lock).
    if (!contracts::is_horizontal(d) || d == s.facing() || d == contracts::opposite(s.facing())) return;
    const auto locked = repeater_locked_now(ctx, pos, s);
    if (locked == repeater_locked(s)) return;
    set_block_quiet(ctx, pos, s.with_level(locked ? 1 : 0)); // updates the front too
}

void check_lamp(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::redstone_lamp)) return;
    const bool powered = has_neighbor_signal(ctx, pos);
    if (powered && !s.flag()) set_block_quiet(ctx, pos, s.with_flag(true));
    else if (!powered && s.flag()) schedule(ctx, TickList::block, pos, BlockType::redstone_lamp, 4, 0);
}

void tick_lamp(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.flag() && !has_neighbor_signal(ctx, pos)) set_block_quiet(ctx, pos, s.with_flag(false));
}

void tick_observer(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!s.flag()) {
        set_block_quiet(ctx, pos, s.with_flag(true));
        update_front(ctx, pos, contracts::opposite(s.facing()));
        schedule(ctx, TickList::block, pos, BlockType::observer, 2, 0);
    } else {
        set_block_quiet(ctx, pos, s.with_flag(false));
        update_front(ctx, pos, contracts::opposite(s.facing()));
    }
}

} // namespace circuit::gameplay::detail
