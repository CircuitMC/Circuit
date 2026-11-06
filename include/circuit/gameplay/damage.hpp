// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// How damage to players is reduced and how players burn, as both official
// servers do it (client/fire; minecraft.wiki "Armor", "Durability",
// "Resistance", "Fire", "Lava").
namespace circuit::gameplay::damage {

// What armor leaves of damage: its points take 4% each, less 2% per point
// of damage over 1 + toughness / 8 (the target versions' float arithmetic),
// but never less than a fifth of the points' 4% nor more than 80%.
float after_armor(float damage, float armor, float toughness) noexcept;
// What the resistance effect leaves: 20% less per level (amplifier + 1),
// nothing from level 5.
float after_resistance(float damage, std::int32_t amplifier) noexcept;
// The durability each piece of armor loses by damage armor reduces: a
// quarter of it, at least one.
std::int32_t armor_wear(float damage) noexcept;

// Burning. A player's remaining fire ticks are -20 while not on fire. Each
// tick in fire counts them up by one to 0, where the player catches fire
// for 8 seconds (160 ticks); from then on each tick in fire adds one or two
// (`more`; the official servers draw it, and their draws were not recorded
// tick by tick: Circuit draws its own) and keeps at least 160. Lava sets
// at least 300. Out of fire, a count below 0 goes back to -20.
inline constexpr std::int32_t not_burning = -20;
std::int32_t touch_fire(std::int32_t ticks, bool more) noexcept;
std::int32_t touch_lava(std::int32_t ticks) noexcept;
// A burning tick: with ticks above 0, a hurt when they are a multiple of 20,
// then one less; at 0 the fire is out (-20).
struct Burn {
    std::int32_t ticks{not_burning};
    bool hurts{};
};
Burn burn(std::int32_t ticks) noexcept;

} // namespace circuit::gameplay::damage
