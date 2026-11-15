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
// What enchantments' protection leaves (minecraft.wiki "Armor"): 4% less
// per point of the enchantment protection factor, which counts between 0
// and 20 (NaN as 0), in single precision.
float after_enchantments(float damage, float protection) noexcept;
// The durability each piece of armor loses by damage armor reduces: a
// quarter of it, at least one.
std::int32_t armor_wear(float damage) noexcept;

// Burning. A player's remaining fire ticks are -20 while not on fire. Each
// tick in fire counts them up by one to 0, where the player catches fire
// for 8 seconds (160 ticks); from then on each tick in fire adds one or two
// (`more`; the official servers draw it, and their draws were not recorded
// tick by tick: Circuit draws its own) and keeps at least 160. Lava sets
// at least 300. Out of fire, a count of 0 or below goes back to -20. The 160
// and 300 are the player's burning time attribute (`burning`, 1 but for fire
// protection) times them, rounded down (Circuit's choice: not told apart from
// rounding by the recordings); lava sets nothing at a burning time of 0.
inline constexpr std::int32_t not_burning = -20;
std::int32_t touch_fire(std::int32_t ticks, bool more, double burning = 1.0) noexcept;
std::int32_t touch_lava(std::int32_t ticks, double burning = 1.0) noexcept;
// Set on fire by an attack for `seconds` (fire aspect: client/ench_combat),
// as many ticks rounded down, times the burning time as fire and lava are;
// a longer burning kept (fire aspect I on a player burning from II), nothing
// at 0.
std::int32_t ignite(std::int32_t ticks, float seconds, double burning = 1.0) noexcept;
// A tick out of fire: a count of 0 or below back to -20.
std::int32_t out_of_fire(std::int32_t ticks) noexcept;
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
// Melee (client/combat, combat-timing). An attack's charge: the whole ticks
// since the attacker's last attack or swing, or since the tick their main
// hand's item changed, plus a half, over the ticks a full charge takes (20
// over the attack speed), at most 1 (the target versions' floats; with an
// attack speed of 0 or less none, not recorded). The damage an attack keeps
// of the attack damage by its charge: 0.2 plus 0.8 times its square. A
// charge above 0.9 is a strong attack: sweeps, critical hits and the
// sprint's knockback need one.
float attack_charge(std::int32_t ticks, double attack_speed) noexcept;
float charged_damage(float damage, float charge) noexcept;
inline constexpr float strong_charge = 0.9f;
// A melee attack's damage to its target (client/combat, client/ench_combat):
// the charged attack damage, half as much again when critical, and the
// enchantments' damage against the target (`enchanted`) times the charge.
float melee_damage(float charged, float enchanted, float charge, bool critical) noexcept;
// A sweep's damage to another player (client/ench_combat): 1 and the
// sweeping damage ratio of the charged attack damage, with the
// enchantments' damage against that player, all times the charge.
float sweep_damage(float charged, double ratio, float enchanted, float charge) noexcept;
// A knockback of `strength` along the horizontal direction (x, z) (for a
// hurt, from what hurt to the one hurt): half the velocity before plus
// `strength` along the direction; upwards, on the ground, half the vertical
// velocity plus `strength`, at most 0.4 (in the air it stays: Circuit's
// reading, not recorded).
struct Velocity {
    double x{}, y{}, z{};
    bool operator==(const Velocity&) const = default;
};
Velocity knock(Velocity before, double strength, double x, double z, bool on_ground) noexcept;
// The angle of a hurt's animation for the one hurt: the direction to what
// hurt them (dx, dz) in degrees, less their yaw. The official servers'
// arctangent is not the exact one (client/combat: -243.43496704101562 where
// the exact one gives -243.4349365234375); Circuit takes the exact one.
float hurt_angle(double dx, double dz, float yaw) noexcept;
// The damage indicator particles a hurt shows: half the health it took,
// rounded down.
std::int32_t damage_indicators(float health_lost) noexcept;

} // namespace circuit::gameplay::damage
