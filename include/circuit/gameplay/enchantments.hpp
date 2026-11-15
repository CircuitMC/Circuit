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
// The damage a requirement is tested against: its type, whether it is
// direct (a melee attack is: its direct source is the entity causing it),
// the type of the entity it reaches (the requirement's `this`; Circuit's
// damage reaches players alone) and of the entities causing and dealing it
// (`attacker` and `direct_attacker`; empty for none).
struct DamageContext {
    std::string_view type;
    bool direct{};
    std::string_view entity{"minecraft:player"};
    std::string_view attacker{};
    std::string_view direct_attacker{};
};
// A requirement on damage, of the forms the vanilla packs give their damage
// and attack effects (minecraft.wiki "Predicate"): the damage type in or out
// of damage type tags and, where Circuit knows it, whether the damage is
// direct (damage_source_properties with tags and is_direct alone); the type
// of the entity it reaches or of those causing or dealing it (entity_properties
// of `this`, `attacker` or `direct_attacker` with a type alone: an entity
// absent fails); all or any of other requirements, or one inverted.
struct DamageRequirement {
    enum class Kind : std::uint8_t { tags, all_of, any_of, inverted, entity };
    enum class Subject : std::uint8_t { self, attacker, direct_attacker };
    struct Tag {
        std::vector<std::string> types; // the tag's damage types, sorted ("minecraft:fall")
        bool expected{};
    };
    Kind kind{};
    std::vector<Tag> tags;                // tags: each must hold as expected
    std::vector<DamageRequirement> terms; // all_of, any_of; inverted: the one inverted
    std::optional<bool> direct;           // tags: whether the damage must be direct, if asked
    Subject subject{};                    // entity: the entity tested
    std::vector<std::string> entities;    // entity: the types it may be, sorted ("minecraft:zombie")
    bool matches(const DamageContext& damage) const;
    // Damage of a type a player takes (no requirement on directness read).
    bool matches(std::string_view type) const { return matches(DamageContext{type}); }
};
// A value an enchantment adds (the effects minecraft:damage_protection,
// minecraft:damage and minecraft:knockback with the value effect add) and
// the damage it adds to.
struct ValueEffect {
    LevelValue value;
    std::optional<DamageRequirement> requirement;
};
// A post-attack effect setting the victim on fire (minecraft:post_attack
// ignite, the attacker's enchantment, the victim affected), for its duration
// in seconds, when the attack meets its requirement.
struct IgniteEffect {
    LevelValue seconds;
    std::optional<DamageRequirement> requirement;
};
// What an enchantment does that Circuit models: in the slots of its `slots`
// groups (whatever the item; client/ench_attributes: efficiency on a stick
// held counts, swift sneak on a chestplate does not), its attribute
// modifiers, the protection it adds against damage, the damage it keeps
// off (minecraft:damage_immunity), the damage and knockback it adds to an
// attack, and the fire its attacks set.
struct Effects {
    std::uint8_t slots{};
    std::vector<AttributeEffect> attributes;
    std::vector<ValueEffect> protection;
    std::vector<std::optional<DamageRequirement>> immunity;
    std::vector<ValueEffect> damage;
    std::vector<ValueEffect> knockback;
    std::vector<IgniteEffect> ignite;
    bool empty() const noexcept {
        return attributes.empty() && protection.empty() && immunity.empty() && damage.empty() && knockback.empty() && ignite.empty();
    }
};

} // namespace circuit::gameplay::enchantments
