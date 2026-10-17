// SPDX-License-Identifier: AGPL-3.0-only
// Doors, trapdoors and fence gates, as both official servers run them
// (checked against their traces; minecraft.wiki "Door", "Trapdoor", "Fence
// Gate"). An update that finds the power around one (for a door, around
// either half) different from its powered state opens or closes it at once
// to match; using one by hand toggles it (iron doors and trapdoors only
// open by power). Every opening and closing draws a float from the level
// random for the sound's pitch. The changes are quiet: a door's other half
// follows through its shape update.
//
// A door is two blocks: the lower half needs a sturdy top below it, and a
// half whose other half is gone breaks, the lower one with the door's drop.
// A fence gate opened by hand faces away from the player unless it already
// does; commands use blocks as a player facing south would (the reference
// harness's player). A fence gate between walls on its sides is in a wall.
#include "internal.hpp"
#include <array>
#include <string>

namespace circuit::gameplay::detail {
namespace {
bool powered(BlockState s) { return s.extra() != 0; }
BlockState with_powered(BlockState s, bool on) { return s.with_extra(on ? 1 : 0); }

BlockPos other_half(BlockPos pos, BlockState door) { return pos.relative(upper_half(door) ? Direction::down : Direction::up); }

void sound(RuleContext& ctx) { ctx.random_float(); }

bool is_wall(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto block = registry::block_of(s.registry_state());
    return block && registry::block(*block).name.ends_with("_wall");
}

bool gate_in_wall(RuleContext& ctx, BlockPos pos, BlockState gate) {
    const auto side = contracts::clockwise(gate.facing());
    return is_wall(ctx.block(pos.relative(side))) || is_wall(ctx.block(pos.relative(contracts::opposite(side))));
}
} // namespace

void check_opening(RuleContext& ctx, BlockPos pos, BlockPos from) {
    const auto s = ctx.block(pos);
    switch (s.type()) {
    case BlockType::door: {
        // Updates from the door's own blocks change nothing.
        const auto source = ctx.block(from);
        if (source.is(BlockType::door) && source.variant() == s.variant()) return;
        const bool on = has_neighbor_signal(ctx, pos) || has_neighbor_signal(ctx, other_half(pos, s));
        if (on == powered(s)) return;
        if (on != s.flag()) sound(ctx);
        set_block_quiet(ctx, pos, with_powered(s, on).with_flag(on));
        break;
    }
    case BlockType::trapdoor: {
        const bool on = has_neighbor_signal(ctx, pos);
        if (on == powered(s)) return;
        auto next = with_powered(s, on);
        if (s.flag() != on) {
            next = next.with_flag(on);
            sound(ctx);
        }
        set_block_quiet(ctx, pos, next);
        break;
    }
    case BlockType::fence_gate: {
        const bool on = has_neighbor_signal(ctx, pos);
        if (on == powered(s)) return;
        set_block_quiet(ctx, pos, with_powered(s, on).with_flag(on));
        if (s.flag() != on) sound(ctx);
        break;
    }
    default: break;
    }
}

void use_opening(RuleContext& ctx, BlockPos pos, BlockState s, Direction player) {
    if (s.is(BlockType::fence_gate)) {
        auto next = s.with_flag(!s.flag());
        // Opened from the side it faces, it turns to face away from the player.
        if (!s.flag() && s.facing() == contracts::opposite(player)) next = next.with_facing(player);
        set_block_quiet(ctx, pos, next);
        sound(ctx);
        return;
    }
    set_block_quiet(ctx, pos, s.with_flag(!s.flag()));
    sound(ctx);
}

void opening_shape_update(RuleContext& ctx, BlockPos pos, Direction d) {
    const auto s = ctx.block(pos);
    if (s.is(BlockType::door)) {
        const auto toward_other = upper_half(s) ? Direction::down : Direction::up;
        if (d == toward_other) {
            const auto other = ctx.block(pos.relative(d));
            if (!other.is(BlockType::door) || other.variant() != s.variant() || upper_half(other) == upper_half(s)) {
                destroy_block(ctx, pos);
                return;
            }
            // The half takes the other half's facing, hinge, opening and power.
            const auto next = s.with_facing(other.facing()).with_level(static_cast<std::uint8_t>((s.level() & 1) | (other.level() & 2)))
                                  .with_flag(other.flag()).with_extra(other.extra());
            if (next != s) set_block_quiet(ctx, pos, next);
            return;
        }
        if (d == Direction::down && !upper_half(s) && !can_survive(ctx, pos, s)) destroy_block(ctx, pos);
        return;
    }
    const auto side = contracts::clockwise(s.facing());
    if (s.is(BlockType::fence_gate) && (d == side || d == contracts::opposite(side))) {
        const bool wall = gate_in_wall(ctx, pos, s);
        if (wall != (s.level() != 0)) set_block_quiet(ctx, pos, s.with_level(wall ? 1 : 0));
    }
}

std::uint16_t opening_item(BlockState s) {
    static const auto table = [] {
        std::array<std::array<std::uint16_t, blocks::door_kinds>, 3> out{};
        constexpr std::array<std::string_view, 3> suffix{"_door", "_trapdoor", "_fence_gate"};
        for (std::size_t t = 0; t < 3; ++t)
            for (std::uint8_t k = 0; k < (t == 2 ? blocks::fence_gate_kinds : blocks::door_kinds); ++k)
                out[t][k] = items::by_name(std::string("minecraft:") + std::string(door_kind_name(k)) + std::string(suffix[t])).value_or(0);
        return out;
    }();
    const std::size_t t = s.is(BlockType::door) ? 0 : s.is(BlockType::trapdoor) ? 1 : 2;
    return table[t][s.variant() % blocks::door_kinds];
}

std::optional<BlockState> placed_opening_by(std::uint16_t item, Direction face, Direction horizontal) {
    for (std::uint8_t k = 0; k < blocks::door_kinds; ++k) {
        const auto kind = static_cast<blocks::DoorKind>(k);
        if (item == opening_item(blocks::door(kind, Direction::north, false))) return blocks::door(kind, horizontal, false);
        // On a side, the trapdoor hangs from the clicked block (bottom half:
        // the official choice by the hit height is not modeled); on a top or
        // bottom it faces the player and lies on that side.
        if (item == opening_item(blocks::trapdoor(kind, Direction::north))) {
            if (contracts::is_horizontal(face)) return blocks::trapdoor(kind, face);
            return blocks::trapdoor(kind, contracts::opposite(horizontal), face == Direction::down);
        }
        if (k < blocks::fence_gate_kinds && item == opening_item(blocks::fence_gate(kind, Direction::north)))
            return blocks::fence_gate(kind, horizontal);
    }
    return std::nullopt;
}

std::optional<registry::BlockInfo> modeled_info(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block ? std::optional(registry::block(*block)) : std::nullopt;
}

BlockState placed_opening(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool on = s.is(BlockType::door) ? has_neighbor_signal(ctx, pos) || has_neighbor_signal(ctx, pos.relative(Direction::up))
                                          : has_neighbor_signal(ctx, pos);
    auto out = with_powered(s, on).with_flag(on);
    if (s.is(BlockType::fence_gate)) out = out.with_level(gate_in_wall(ctx, pos, s) ? 1 : 0);
    return out;
}

} // namespace circuit::gameplay::detail
