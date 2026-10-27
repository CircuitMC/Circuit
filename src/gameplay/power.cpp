// SPDX-License-Identifier: AGPL-3.0-only
// Redstone power of circuit-reference-rules-v6, as documented and observed in
// target-version traces (strong_power, weak_power, dust_shape):
// - Sources give power to their neighbors. Some also power one block strongly:
//   a lever or button the block it hangs on, a repeater or comparator the
//   block in front, an observer the block at its back, a redstone torch the
//   block above it.
// - A redstone torch powers every neighbor but the block it stands or hangs
//   on, and goes out while that block is powered (redstone_torch).
// - Conductors (full opaque blocks) give nothing themselves. A conductor that a
//   source powers strongly powers dust and components next to it. Dust powers
//   the block below it and the blocks it points into; such a weakly powered
//   conductor powers components next to it, but not dust.
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay::detail {

// Redstone conductors as both official servers report them for every state:
// blocks without rules of their own by the registry's rule (full-cube
// collision, with named exceptions); slime blocks conduct, glass, honey, the
// redstone block, observers and pistons do not. Faces of blocks without rules
// are sturdy as their support shapes are (registry::sturdy_face, checked
// against both official servers for every state).
bool is_conductor(BlockState s) noexcept {
    switch (s.type()) {
    case BlockType::stone: case BlockType::obsidian: case BlockType::redstone_lamp: case BlockType::target: case BlockType::note_block:
    case BlockType::dispenser: case BlockType::furnace: case BlockType::crafter: case BlockType::slime_block:
        return true;
    case BlockType::other: return registry::conducts(s.registry_state());
    default: return false;
    }
}

bool face_sturdy(BlockState s, Direction face) noexcept {
    switch (s.type()) {
    case BlockType::other: return registry::sturdy_face(s.registry_state(), face);
    // Doors, trapdoors, fence gates and lecterns by the shape of the state they stand for.
    case BlockType::door: case BlockType::trapdoor: case BlockType::fence_gate: case BlockType::lectern: {
        const auto state = physics::registry_state_of(s);
        return state && registry::face_covered(*state, face);
    }
    case BlockType::stone: case BlockType::glass: case BlockType::obsidian: case BlockType::redstone_block:
    case BlockType::slime_block: case BlockType::redstone_lamp: case BlockType::observer: case BlockType::target:
    case BlockType::honey_block: case BlockType::note_block: case BlockType::dispenser: case BlockType::furnace: case BlockType::crafter:
        return true;
    case BlockType::piston: case BlockType::sticky_piston:
        // An extended base keeps only its back face whole.
        return !s.flag() || face == contracts::opposite(s.facing());
    case BlockType::piston_head: return face == s.facing(); // the plate
    default: return false;
    }
}

bool supports_dust(BlockState below) noexcept { return face_sturdy(below, Direction::up) || below.is(BlockType::hopper); }

// A floor torch stands on a block whose top's middle bears it (as torches and
// lanterns do; registry::supports_middle, checked against both official
// servers for every state).
bool supports_torch(RuleContext& ctx, BlockState below) {
    const auto state = physics::registry_state_of(below);
    return state && registry::supports_middle(*state, Direction::up, ctx.bundle().schema.content_release);
}
bool supports_diode(BlockState below) noexcept { return face_sturdy(below, Direction::up); }

Direction lever_attachment(BlockState lever) noexcept {
    switch (static_cast<blocks::LeverFace>(lever.level())) {
    case blocks::LeverFace::floor: return Direction::down;
    case blocks::LeverFace::ceiling: return Direction::up;
    default: return contracts::opposite(lever.facing());
    }
}

Direction attachment(BlockState s) noexcept {
    switch (s.type()) {
    case BlockType::redstone_torch: return Direction::down;
    case BlockType::redstone_wall_torch: return contracts::opposite(s.facing());
    default: return lever_attachment(s);
    }
}

bool can_survive(RuleContext& ctx, BlockPos pos, BlockState s) {
    switch (s.type()) {
    case BlockType::redstone_wire: return supports_dust(ctx.block(pos.relative(Direction::down)));
    case BlockType::repeater: case BlockType::comparator: return supports_diode(ctx.block(pos.relative(Direction::down)));
    case BlockType::lever: case BlockType::button: case BlockType::redstone_wall_torch: {
        const auto d = attachment(s);
        return face_sturdy(ctx.block(pos.relative(d)), contracts::opposite(d));
    }
    // Plates stand where floor torches do (an approximation of rigid and
    // center support).
    case BlockType::redstone_torch: case BlockType::pressure_plate: case BlockType::weighted_plate:
        return supports_torch(ctx, ctx.block(pos.relative(Direction::down)));
    // A door's lower half needs a sturdy top below it; the upper one its lower half.
    case BlockType::door: return upper_half(s) || face_sturdy(ctx.block(pos.relative(Direction::down)), Direction::up);
    case BlockType::rail: return rail_supported(ctx, pos);
    case BlockType::coral_fan: case BlockType::coral_wall_fan: return coral_supported(ctx, pos, s);
    // Wheat is planted on farmland (both official servers; tests/fixtures/placement).
    case BlockType::wheat: {
        const auto below = ctx.block(pos.relative(Direction::down));
        const auto block = below.is(BlockType::other) ? registry::block_of(below.registry_state()) : std::nullopt;
        return block && registry::block(*block).name == "farmland";
    }
    default: return true;
    }
}

std::uint8_t emitted(RuleContext& ctx, BlockPos pos, BlockState s, Direction toward, bool wires) {
    switch (s.type()) {
    case BlockType::lever: case BlockType::button: return s.flag() ? 15 : 0;
    case BlockType::redstone_torch: case BlockType::redstone_wall_torch:
        return s.flag() && toward != attachment(s) ? 15 : 0;
    case BlockType::redstone_block: return 15;
    case BlockType::pressure_plate: case BlockType::weighted_plate: return plate_output(s);
    case BlockType::target: case BlockType::daylight_detector: return s.level();
    case BlockType::rail: return s.variant() == static_cast<std::uint8_t>(blocks::RailKind::detector) && s.flag() ? 15 : 0;
    case BlockType::repeater: return s.flag() && toward == s.facing() ? 15 : 0;
    case BlockType::comparator: {
        if (!s.flag() || toward != s.facing()) return 0;
        const auto* be = ctx.peek_block_entity(pos);
        return be ? be->signal : 0;
    }
    case BlockType::observer: return s.flag() && toward == contracts::opposite(s.facing()) ? 15 : 0;
    // A powered lectern (a page just turned) powers every neighbor.
    case BlockType::lectern: return s.flag() ? 15 : 0;
    // A trapped chest by its viewers (client/openers).
    case BlockType::chest: return trapped_power(ctx, pos, s);
    case BlockType::redstone_wire: {
        if (!wires || s.level() == 0 || toward == Direction::up) return 0;
        if (toward == Direction::down) return s.level();
        // The side must be connected in the shape the dust has now.
        return fresh_wire_shape(ctx, pos, s, is_dot(s)).wire_connected(toward) ? s.level() : 0;
    }
    default: return 0;
    }
}

std::uint8_t direct(RuleContext& ctx, BlockPos pos, BlockState s, Direction toward, bool wires) {
    switch (s.type()) {
    case BlockType::lever: case BlockType::button: return s.flag() && toward == attachment(s) ? 15 : 0;
    case BlockType::redstone_torch: case BlockType::redstone_wall_torch: return s.flag() && toward == Direction::up ? 15 : 0;
    case BlockType::pressure_plate: case BlockType::weighted_plate: return toward == Direction::down ? plate_output(s) : 0;
    case BlockType::rail: return toward == Direction::down ? emitted(ctx, pos, s, toward, wires) : 0;
    // ... and the block below it strongly (lectern_pages).
    case BlockType::lectern: return s.flag() && toward == Direction::down ? 15 : 0;
    case BlockType::chest: return toward == Direction::down ? trapped_power(ctx, pos, s) : 0;
    case BlockType::repeater: case BlockType::comparator: case BlockType::observer: case BlockType::redstone_wire:
        return emitted(ctx, pos, s, toward, wires);
    default: return 0;
    }
}

std::uint8_t strong_into(RuleContext& ctx, BlockPos pos, bool wires) {
    std::uint8_t best = 0;
    for (const auto d : {Direction::down, Direction::up, Direction::north, Direction::south, Direction::west, Direction::east}) {
        const auto n = pos.relative(d);
        best = std::max(best, direct(ctx, n, ctx.block(n), contracts::opposite(d), wires));
        if (best >= 15) break;
    }
    return best;
}

std::uint8_t signal_from(RuleContext& ctx, BlockPos from, Direction toward, bool wires) {
    const auto s = ctx.block(from);
    auto power = emitted(ctx, from, s, toward, wires);
    if (power < 15 && is_conductor(s)) power = std::max(power, strong_into(ctx, from, wires));
    return power;
}

std::uint8_t best_neighbor_signal(RuleContext& ctx, BlockPos pos, bool wires) {
    std::uint8_t best = 0;
    for (const auto d : {Direction::down, Direction::up, Direction::north, Direction::south, Direction::west, Direction::east}) {
        best = std::max(best, signal_from(ctx, pos.relative(d), contracts::opposite(d), wires));
        if (best >= 15) break;
    }
    return best;
}

bool has_neighbor_signal(RuleContext& ctx, BlockPos pos) { return best_neighbor_signal(ctx, pos, true) > 0; }

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
bool redstone_conductor(BlockState s) noexcept { return detail::is_conductor(s); }
} // namespace circuit::gameplay
