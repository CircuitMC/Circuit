// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/attributes.hpp>
#include <circuit/gameplay/equipment.hpp>
#include <array>
#include <cstdint>
#include <functional>
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
// (`attacker` and `direct_attacker`; empty for none); for a post-attack
// effect, the enchantment's level and the draws of its random chances (a
// uniform draw in [0, 1) each; none elsewhere).
struct DamageContext {
    std::string_view type;
    bool direct{};
    std::string_view entity{"minecraft:player"};
    std::string_view attacker{};
    std::string_view direct_attacker{};
    std::int32_t level{};
    const std::function<float()>* draw{};
};
// A requirement on damage, of the forms the vanilla packs give their damage
// and attack effects (minecraft.wiki "Predicate"): the damage type in or out
// of damage type tags and, where Circuit knows it, whether the damage is
// direct (damage_source_properties with tags and is_direct alone); the type
// of the entity it reaches or of those causing or dealing it (entity_properties
// of `this`, `attacker` or `direct_attacker` with a type alone: an entity
// absent fails); for post-attack effects, a random chance (random_chance:
// a draw below the chance at the enchantment's level; without draws it
// fails); all or any of other requirements, or one inverted.
struct DamageRequirement {
    enum class Kind : std::uint8_t { tags, all_of, any_of, inverted, entity, chance };
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
    LevelValue chance;                    // chance: by the enchantment's level
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
// The entity whose enchantment a post-attack effect is or whom it reaches:
// the attacker or the victim (a player's melee attack is dealt by the
// attacker, its damaging entity).
enum class Party : std::uint8_t { attacker, victim };
// One of the entity effects a post-attack effect runs, in the order
// minecraft:all_of lists them: the affected entity set on fire for `a`
// seconds (ignite); hurt for a draw between `a` and `b` of the damage type
// `damage_type`, caused and dealt by the enchanted entity (damage_entity);
// or the enchanted item worn by `a` (change_item_damage).
struct EntityEffect {
    enum class Kind : std::uint8_t { ignite, damage, wear };
    Kind kind{};
    LevelValue a, b;
    std::string damage_type;
};
// A post-attack effect (minecraft:post_attack) of the forms Circuit acts on:
// whose enchantment it is, whom it reaches, what it does, and when the
// attack meets its requirement.
struct PostAttackEffect {
    Party enchanted{}, affected{};
    std::vector<EntityEffect> effects;
    std::optional<DamageRequirement> requirement;
};
// A requirement on the item an effect is for, of the forms the vanilla
// packs give (minecraft.wiki "Predicate"): match_tool on its item alone (one
// item, a list or an item tag), all or any of other requirements, or one
// inverted.
struct ItemRequirement {
    enum class Kind : std::uint8_t { items, all_of, any_of, inverted };
    Kind kind{};
    std::vector<std::string> items;    // items: those it may be, sorted ("minecraft:iron_sword")
    std::vector<ItemRequirement> terms; // all_of, any_of; inverted: the one inverted
    bool matches(std::string_view item) const;
};
// A change of the wear an enchanted item takes (minecraft:item_damage) of
// the form remove_binomial: each point of it removed by a draw below
// `chance` at the enchantment's level (unbreaking), for an item meeting the
// requirement.
struct ItemDamageEffect {
    LevelValue chance;
    std::optional<ItemRequirement> requirement;
};
// A value effect of any of the data's forms (minecraft.wiki "Enchantment
// definition"), worked out in single precision on a value: a level-based
// value added (add), a factor multiplied by (multiply), the value replaced
// (set), each whole unit of it removed by a draw below a chance
// (remove_binomial: its integer part's units, none below 1, at most 2^20 of
// them; Circuit's choices for a value with a fraction and for huge values),
// or the effects in order (all_of).
struct ValueOperation {
    enum class Kind : std::uint8_t { add, multiply, set, remove_binomial, all_of };
    Kind kind{};
    LevelValue value;                  // add, set: the value; multiply: the factor; remove_binomial: the chance
    std::vector<ValueOperation> parts; // all_of
    // `draw` gives uniform draws in [0, 1) for remove_binomial.
    float apply(float input, std::int32_t level, const std::function<float()>& draw) const;
};
// The durability an experience orb's points repair (minecraft:repair_with_xp,
// mending), for an item meeting the requirement.
struct RepairEffect {
    ValueOperation operation;
    std::optional<ItemRequirement> requirement;
};
// What a location-changed effect's requirement is tested against: whether
// the enchanted entity is on the ground (a player's: as its client last
// told) and whether it rides a vehicle (Circuit's players never do).
struct EntityContext {
    bool on_ground{};
    bool riding{};
};
// A requirement on the enchanted entity of a location-changed effect, of the
// forms frost walker's gives (minecraft.wiki "Predicate"): entity_properties
// of `this` with the flag is_on_ground and whether it rides a vehicle (an
// empty vehicle predicate: any), each asked or not; all or any of other
// requirements, or one inverted.
struct EntityRequirement {
    enum class Kind : std::uint8_t { properties, all_of, any_of, inverted };
    Kind kind{};
    std::optional<bool> on_ground;        // properties: the flag, if asked
    bool riding{};                        // properties: whether it must ride a vehicle
    std::vector<EntityRequirement> terms; // all_of, any_of; inverted: the one inverted
    bool matches(const EntityContext& entity) const;
};
// A disk of blocks an entity's move places around it (the effect
// minecraft:replace_disk of minecraft:location_changed), of the form frost
// walker's gives: the block state `state` (simple_state_provider; raw, as
// contracts::SetBlock's) placed in the disk's cells, `offset` from the
// entity's block, of `radius` and `height` by level, that hold still water
// with air above and nothing in the way (the predicate all_of of
// matching_block_tag air above, matching_blocks and matching_fluids water and
// unobstructed, which the rules test), when the entity meets the requirement.
struct DiskEffect {
    std::uint32_t state{};
    LevelValue radius, height;
    std::array<std::int32_t, 3> offset{};
    std::optional<EntityRequirement> requirement;
};
// What an enchantment does that Circuit models: in the slots of its `slots`
// groups (whatever the item; client/ench_attributes: efficiency on a stick
// held counts, swift sneak on a chestplate does not), its attribute
// modifiers, the protection it adds against damage, the damage it keeps
// off (minecraft:damage_immunity), the damage and knockback it adds to an
// attack, what follows an attack that hurt (the fire an attacker's
// enchantment sets, a victim's thorns) and the repair experience gives (an
// enchantment with repair effects makes its item take part, whatever their
// requirements; client/ench_mending_pack: shears meeting none repaired a
// point a point), and the disks of blocks a move places
// (minecraft:location_changed's replace_disk effects: frost walker's ice);
// and, wherever the enchanted item is, the wear it is spared, whether it
// stays on in an armor slot (minecraft:prevent_armor_change, the curse of
// binding) and whether it vanishes at its holder's death
// (minecraft:prevent_equipment_drop, the curse of vanishing).
struct Effects {
    std::uint8_t slots{};
    std::vector<AttributeEffect> attributes;
    std::vector<ValueEffect> protection;
    std::vector<std::optional<DamageRequirement>> immunity;
    std::vector<ValueEffect> damage;
    std::vector<ValueEffect> knockback;
    std::vector<PostAttackEffect> post_attack;
    std::vector<ItemDamageEffect> item_damage;
    std::vector<RepairEffect> repair_with_xp;
    std::vector<DiskEffect> location_changed;
    bool prevent_armor_change{};
    bool prevent_equipment_drop{};
    bool empty() const noexcept {
        return attributes.empty() && protection.empty() && immunity.empty() && damage.empty() && knockback.empty() && post_attack.empty()
               && item_damage.empty() && repair_with_xp.empty() && location_changed.empty() && !prevent_armor_change
               && !prevent_equipment_drop;
    }
};

} // namespace circuit::gameplay::enchantments
