// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/attributes.hpp>
#include <circuit/gameplay/equipment.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The effects of enchantments, as the official data packs define them
// (data/minecraft/enchantment/*.json; their forms by minecraft.wiki
// "Enchantment definition"), and what of them reaches players.
namespace circuit::gameplay::enchantments {

// A value by enchantment level (the data's level-based values), worked out
// in single precision as the data's floats are: a constant; linear (base
// plus per level above the first); level squared plus an added value; a
// fraction of two values; a value clamped; one looked up by level with a
// fallback; one value raised to another.
struct LevelValue {
    enum class Kind : std::uint8_t { constant, linear, levels_squared, fraction, clamped, lookup, exponent };
    Kind kind{};
    float a{}, b{};                 // constant: a; linear: base a, per level b; levels_squared: added a; clamped: min a, max b
    std::vector<LevelValue> parts;  // fraction: numerator, denominator; clamped: the value; lookup: the fallback; exponent: base, power
    std::vector<float> values;      // lookup: by level from I
    float at(std::int32_t level) const noexcept;
};

// The equipment slots of a slot group of the data's `slots` (any, hand,
// mainhand, offhand, armor, feet, legs, chest, head; body and saddle are no
// player's), a bit per equipment::Slot; nullopt for a name it does not know.
std::optional<std::uint8_t> slot_group(std::string_view name) noexcept;
constexpr std::uint8_t slot_bit(equipment::Slot s) noexcept { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(s)); }

// An attribute modifier an enchanted item gives in one of its slots (the
// effect minecraft:attributes): the modifier's id is the effect's with the
// slot appended ("enchantment.respiration/head"; client/ench_attributes).
struct AttributeEffect {
    attributes::Attribute attribute{};
    std::array<std::string, equipment::slot_count> ids; // by equipment::Slot, without "minecraft:"
    LevelValue amount;
    attributes::Operation operation{};
};
// A requirement on the damage a player takes, of the forms the vanilla
// packs give their damage effects (minecraft.wiki "Predicate"): the damage
// type in or out of damage type tags (damage_source_properties with tags
// alone), all or any of other requirements, or one inverted.
struct DamageRequirement {
    enum class Kind : std::uint8_t { tags, all_of, any_of, inverted };
    struct Tag {
        std::vector<std::string> types; // the tag's damage types, sorted ("minecraft:fall")
        bool expected{};
    };
    Kind kind{};
    std::vector<Tag> tags;                // tags: each must hold as expected
    std::vector<DamageRequirement> terms; // all_of, any_of; inverted: the one inverted
    bool matches(std::string_view type) const;
};
// A damage protection value (the effect minecraft:damage_protection, its
// value added) and the damage it protects against.
struct ProtectionEffect {
    LevelValue value;
    std::optional<DamageRequirement> requirement;
};
// What an enchantment does that Circuit models: in the slots of its `slots`
// groups (whatever the item; client/ench_attributes: efficiency on a stick
// held counts, swift sneak on a chestplate does not), its attribute
// modifiers, the protection it adds against damage and the damage it keeps
// off (minecraft:damage_immunity).
struct Effects {
    std::uint8_t slots{};
    std::vector<AttributeEffect> attributes;
    std::vector<ProtectionEffect> protection;
    std::vector<std::optional<DamageRequirement>> immunity;
    bool empty() const noexcept { return attributes.empty() && protection.empty() && immunity.empty(); }
};

} // namespace circuit::gameplay::enchantments
