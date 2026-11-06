// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// How damage to players is reduced and how players burn, drown and freeze,
// as both official servers do it (client/fire, client/drown;
// minecraft.wiki "Armor", "Durability", "Resistance", "Fire", "Lava",
// "Damage").
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

// Breath. A player's air supply is 300 ticks. A tick it is `losing` (under
// water and unable to breathe there) takes one; at -20 the player drowns
// (a hurt) and it is 0 again. A tick it is not adds four, up to 300.
inline constexpr std::int32_t max_air = 300;
struct Breath {
    std::int32_t air{max_air};
    bool drowns{};
};
Breath breathe(std::int32_t air, bool losing) noexcept;

// Frost. A player's frozen ticks go up by one a tick while `freezing` (in
// powder snow and able to freeze), to 140, where the player is fully frozen;
// down by two a tick otherwise, to 0. The slowness they give: an amount
// added to the movement speed, -0.05 when fully frozen (the target
// versions' float arithmetic, widened).
inline constexpr std::int32_t frozen_max = 140;
std::int32_t frost(std::int32_t ticks, bool freezing) noexcept;
double frost_slowness(std::int32_t ticks) noexcept;

} // namespace circuit::gameplay::damage
