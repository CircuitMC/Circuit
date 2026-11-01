// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Attributes of players. The attributes the server keeps for players, with
// a player's base and each attribute's range (minecraft.wiki "Attribute";
// PrismarineJS minecraft-data attributes.json), the modifiers of creative
// mode, sprinting and status effects, and an attribute's value from its base
// and modifiers, as both official servers send them
// (circuit-clienttest/attributes.js, attributes-join.js and
// attributes-order.js, both versions identical).
namespace circuit::gameplay::attributes {

// In the registry's order, the same on both versions (their ids differ: the
// wire adapter maps them).
enum class Attribute : std::uint8_t {
    attack_damage,
    attack_speed,
    block_interaction_range,
    entity_interaction_range,
    luck,
    max_absorption,
    max_health,
    movement_speed,
    safe_fall_distance,
};
inline constexpr std::size_t count = 9;
// The name without its namespace, and an attribute by name (with or without "minecraft:").
std::string_view name(Attribute a) noexcept;
std::optional<Attribute> by_name(std::string_view name) noexcept;
// A player's base value, as the official servers send it (movement speed
// 0.1 as a float, widened).
double base(Attribute a) noexcept;
// Whether the official servers send the attribute to clients. Attack damage
// they keep to themselves: strength and weakness send no attributes.
bool synced(Attribute a) noexcept;

enum class Operation : std::uint8_t { add_value, add_multiplied_base, add_multiplied_total };
struct Modifier {
    std::string_view id; // without "minecraft:"
    double amount{};
    Operation operation{};
    bool operator==(const Modifier&) const = default;
};
// An attribute a player has, with its modifiers in the order they were added.
struct Instance {
    Attribute attribute{};
    std::vector<Modifier> modifiers;
    bool operator==(const Instance&) const = default;
};
// The base plus the added values, times one plus the multiples of the base,
// times one plus each multiple of the total, within the attribute's range.
double value(const Instance& instance) noexcept;

// Creative mode's longer reach of blocks and entities, and sprinting's speed.
inline constexpr Modifier creative_block_range{"creative_mode_block_range", 0.5, Operation::add_value};
inline constexpr Modifier creative_entity_range{"creative_mode_entity_range", 2.0, Operation::add_value};
inline constexpr Modifier sprinting{"sprinting", static_cast<double>(0.3f), Operation::add_multiplied_total};

// The modifier a status effect adds at an amplifier, if any: speed and
// slowness the movement speed, haste and mining fatigue the attack speed (as
// multiples of the total), jump boost the safe fall distance, health boost
// the maximum health, absorption the maximum absorption, luck and bad luck
// the luck (added values), each by the amplifier plus one; strength and
// weakness the attack damage (by minecraft.wiki: the official servers do
// not send it).
struct EffectModifier {
    Attribute attribute{};
    Modifier modifier;
};
std::optional<EffectModifier> effect_modifier(std::uint8_t effect, std::int32_t amplifier) noexcept;

} // namespace circuit::gameplay::attributes
