// SPDX-License-Identifier: AGPL-3.0-only
// Attributes of players and the modifiers of status effects, as both
// official servers send them (circuit-clienttest/attributes.js, both
// versions identical).
#include <circuit/gameplay/attributes.hpp>
#include <circuit/gameplay/effects.hpp>
#include <algorithm>
#include <array>

namespace circuit::gameplay::attributes {
namespace {
struct Entry {
    std::string_view name;
    double base, min, max;
    bool synced;
};
constexpr std::array<Entry, count> entries{{
    {"armor", 0.0, 0.0, 30.0, true},
    {"armor_toughness", 0.0, 0.0, 20.0, true},
    {"attack_damage", 1.0, 0.0, 2048.0, false},
    {"attack_speed", 4.0, 0.0, 1024.0, true},
    {"block_interaction_range", 4.5, 0.0, 64.0, true},
    {"entity_interaction_range", 3.0, 0.0, 64.0, true},
    {"knockback_resistance", 0.0, 0.0, 1.0, false},
    {"luck", 0.0, -1024.0, 1024.0, true},
    {"max_absorption", 0.0, 0.0, 2048.0, true},
    {"max_health", 20.0, 1.0, 1024.0, true},
    {"movement_speed", static_cast<double>(0.1f), 0.0, 1024.0, true},
    {"safe_fall_distance", 3.0, -1024.0, 1024.0, true},
}};

// Each effect's modifier: its attribute, the amount per level and the operation.
struct ByEffect {
    std::string_view effect;
    std::string_view id;
    Attribute attribute;
    double per_level;
    Operation operation;
};
constexpr std::array<ByEffect, 11> by_effect{{
    {"speed", "effect.speed", Attribute::movement_speed, static_cast<double>(0.2f), Operation::add_multiplied_total},
    {"slowness", "effect.slowness", Attribute::movement_speed, static_cast<double>(-0.15f), Operation::add_multiplied_total},
    {"haste", "effect.haste", Attribute::attack_speed, static_cast<double>(0.1f), Operation::add_multiplied_total},
    {"mining_fatigue", "effect.mining_fatigue", Attribute::attack_speed, static_cast<double>(-0.1f), Operation::add_multiplied_total},
    {"strength", "effect.strength", Attribute::attack_damage, 3.0, Operation::add_value},
    {"jump_boost", "effect.jump_boost", Attribute::safe_fall_distance, 1.0, Operation::add_value},
    {"weakness", "effect.weakness", Attribute::attack_damage, -4.0, Operation::add_value},
    {"health_boost", "effect.health_boost", Attribute::max_health, 4.0, Operation::add_value},
    {"absorption", "effect.absorption", Attribute::max_absorption, 4.0, Operation::add_value},
    {"luck", "effect.luck", Attribute::luck, 1.0, Operation::add_value},
    {"unluck", "effect.unluck", Attribute::luck, -1.0, Operation::add_value},
}};
} // namespace

std::string_view name(Attribute a) noexcept { return entries[static_cast<std::size_t>(a)].name; }

std::optional<Attribute> by_name(std::string_view name) noexcept {
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    const auto it = std::ranges::find(entries, name, &Entry::name);
    if (it == entries.end()) return std::nullopt;
    return static_cast<Attribute>(it - entries.begin());
}

double base(Attribute a) noexcept { return entries[static_cast<std::size_t>(a)].base; }
bool synced(Attribute a) noexcept { return entries[static_cast<std::size_t>(a)].synced; }

double value(const Instance& instance) noexcept {
    const auto& e = entries[static_cast<std::size_t>(instance.attribute)];
    double v = e.base;
    for (const auto& m : instance.modifiers)
        if (m.operation == Operation::add_value) v += m.amount;
    double times = 1.0;
    for (const auto& m : instance.modifiers)
        if (m.operation == Operation::add_multiplied_base) times += m.amount;
    v *= times;
    for (const auto& m : instance.modifiers)
        if (m.operation == Operation::add_multiplied_total) v *= 1.0 + m.amount;
    return std::clamp(v, e.min, e.max);
}

std::optional<EffectModifier> effect_modifier(std::uint8_t effect, std::int32_t amplifier) noexcept {
    const auto names = effects::names();
    if (effect >= names.size()) return std::nullopt;
    const auto it = std::ranges::find(by_effect, names[effect], &ByEffect::effect);
    if (it == by_effect.end()) return std::nullopt;
    return EffectModifier{it->attribute, {it->id, it->per_level * static_cast<double>(amplifier + 1), it->operation}};
}

} // namespace circuit::gameplay::attributes
