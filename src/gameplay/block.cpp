// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/block.hpp>

namespace circuit::gameplay {
namespace {
struct Properties {
    bool facing, level, flag, delay;
};
constexpr Properties properties(BlockType t) noexcept {
    switch (t) {
    case BlockType::lever: return {true, true, true, false};
    case BlockType::redstone_lamp: return {false, false, true, false};
    case BlockType::redstone_wire: case BlockType::wheat: return {false, true, false, false};
    case BlockType::repeater: return {true, true, true, true};
    case BlockType::observer: case BlockType::piston: case BlockType::sticky_piston:
    case BlockType::piston_head: case BlockType::hopper: case BlockType::moving_piston:
        return {true, false, true, false};
    case BlockType::water: return {false, true, true, false};
    case BlockType::chest: return {true, true, false, false};
    case BlockType::comparator: return {true, true, true, false};
    default: return {false, false, false, false};
    }
}
} // namespace

contracts::Result<BlockState> BlockState::decode(std::uint32_t raw) noexcept {
    const auto invalid = std::unexpected(contracts::ErrorCode::invalid_block_state);
    if ((raw & 0xFF) >= static_cast<std::uint32_t>(BlockType::count_)) return invalid;
    const BlockState s(raw);
    if (raw >> 26) return invalid;
    if (raw >> 18) {
        // Only dust stores sides, each none, side or up.
        if (!s.is(BlockType::redstone_wire)) return invalid;
        for (auto d : contracts::horizontal_directions)
            if (static_cast<std::uint8_t>(s.wire_side(d)) > 2) return invalid;
    }
    const auto p = properties(s.type());
    const auto facing = (raw >> 8) & 7;
    const auto delay_bits = (raw >> 16) & 3;
    if (facing > 5 || (!p.facing && facing != 0)) return invalid;
    if (!p.level && s.level() != 0) return invalid;
    if (!p.flag && s.flag()) return invalid;
    if (!p.delay && delay_bits != 0) return invalid;
    switch (s.type()) {
    case BlockType::repeater:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 1) return invalid;
        break;
    case BlockType::lever:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 2) return invalid;
        break;
    case BlockType::hopper:
        if (s.facing() == Direction::up) return invalid;
        break;
    case BlockType::chest:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 2) return invalid;
        break;
    case BlockType::comparator:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 1) return invalid;
        break;
    case BlockType::water:
        if (s.level() > 7 || (s.flag() && s.level() != 0)) return invalid;
        break;
    case BlockType::wheat:
        if (s.level() > 7) return invalid;
        break;
    default:
        break;
    }
    return s;
}

} // namespace circuit::gameplay
