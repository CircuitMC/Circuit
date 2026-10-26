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
// is left over, rescaled to the next level's need. The total grows too, as
// a 32-bit sum that wraps, and is kept from going below 0. Negative points
// take progress away, and levels while the progress is below 0 (at level 0
// all experience is gone).
void give_points(PlayerInventory& player, std::int32_t points) noexcept;
// Gives (or takes) levels; the total stays. Below level 0 the player has no
// level, progress or total.
void give_levels(PlayerInventory& player, std::int32_t levels) noexcept;
// Sets the progress to `points` of what the level needs (at most one less
// than all of it); the level and total stay.
void set_points(PlayerInventory& player, std::int32_t points) noexcept;

// The player's enchantment seed, which an enchanting table's offers draw
// from: its own once enchanting changed it (`xp_seed`), else one Circuit
// derives from the player (the official servers draw a new player's at
// random).
std::int32_t enchantment_seed(const PlayerInventory& player) noexcept;

} // namespace circuit::gameplay::experience
