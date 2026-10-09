// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <array>
#include <string_view>
#include <vector>

namespace circuit::gameplay {
namespace {
// Registry blocks with a BlockType of their own (see evidence::ContentMapping).
constexpr std::array<std::string_view, 22> modeled_names{
    "air", "cave_air", "void_air", "stone", "glass", "obsidian", "redstone_block", "slime_block", "lever", "redstone_wire",
    "repeater", "redstone_lamp", "observer", "piston", "sticky_piston", "piston_head", "moving_piston", "hopper", "chest",
    "comparator", "water", "wheat"};

struct Properties {
    bool facing, level, flag, delay;
};
constexpr Properties properties(BlockType t) noexcept {
    switch (t) {
    case BlockType::air: return {false, true, false, false};
    case BlockType::lever: return {true, true, true, false};
    case BlockType::redstone_lamp: return {false, false, true, false};
    case BlockType::redstone_wire: case BlockType::wheat: return {false, true, false, false};
    case BlockType::repeater: return {true, true, true, true};
    case BlockType::observer: case BlockType::piston: case BlockType::sticky_piston:
    case BlockType::hopper: case BlockType::moving_piston:
        return {true, false, true, false};
    case BlockType::piston_head: return {true, true, true, false};
    case BlockType::water: return {false, true, true, false};
    case BlockType::chest: return {true, true, true, false};
    case BlockType::comparator: return {true, true, true, false};
    default: return {false, false, false, false};
    }
}
const std::vector<bool>& modeled_blocks() {
    static const auto modeled = [] {
        std::vector<bool> out(registry::block_count());
        for (const auto name : modeled_names)
            for (const auto release : registry::releases())
                if (const auto b = registry::find_block(release, name)) out[*b] = true;
        return out;
    }();
    return modeled;
}
} // namespace

bool modeled_block(std::uint32_t internal_block) noexcept {
    const auto& modeled = modeled_blocks();
    return internal_block < modeled.size() && modeled[internal_block];
}

std::uint32_t registry_state_count() noexcept { return registry::state_count(); }

bool other_has_collision(BlockState s) noexcept { return registry::collision(s.registry_state()) != registry::Collision::empty; }

bool other_opaque(BlockState s) noexcept {
    const auto b = registry::block_of(s.registry_state());
    return b && registry::block(*b).opaque;
}

bool other_face_covered(BlockState s, Direction face) noexcept { return registry::face_covered(s.registry_state(), face); }

contracts::Result<BlockState> BlockState::decode(std::uint32_t raw) noexcept {
    const auto invalid = std::unexpected(contracts::ErrorCode::invalid_block_state);
    if ((raw & 0xFF) >= static_cast<std::uint32_t>(BlockType::count_)) return invalid;
    const BlockState s(raw);
    if (s.is(BlockType::other)) {
        // Blocks with rules of their own never use the registry encoding.
        const auto block = registry::block_of(s.registry_state());
        if (!block || modeled_block(*block)) return invalid;
        return s;
    }
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
        if (s.level() > 7) return invalid;
        break;
    case BlockType::air:
        if (s.level() > 2) return invalid;
        break;
    case BlockType::piston_head:
        if (s.level() > 1) return invalid;
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
