// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/world_types.hpp>
#include <cstdint>

// Player experience (minecraft.wiki "Experience"; the official servers'
// level, progress and total after every orb a player takes, checked with a
// protocol client on both versions).
namespace circuit::gameplay::experience {

// The points a player at `level` needs for the next level.
constexpr std::int32_t needed(std::int32_t level) noexcept {
    return level >= 30 ? 112 + (level - 30) * 9 : level >= 15 ? 37 + (level - 15) * 5 : 7 + level * 2;
}

// Gives a player experience points: the progress grows by the points over
// what the level needs, in single precision; each whole level carries what
// is left over, rescaled to the next level's need. The total grows too, up
// to the largest int.
void give_points(PlayerInventory& player, std::int32_t points) noexcept;

} // namespace circuit::gameplay::experience
