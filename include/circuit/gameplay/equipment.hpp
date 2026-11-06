// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// What players wear and hold (client/armor): the items worn in the armor
// slots with the armor, toughness and knockback resistance they give and the
// sound they make when put on, and what weapons and tools give in the main
// hand, as both official servers give them by default (minecraft.wiki
// "Armor", "Damage" and each item's page; the attribute modifiers both
// official servers send checked for the recorded items).
namespace circuit::gameplay::equipment {

// The equipment slots in the protocol's order (set_equipment).
enum class Slot : std::uint8_t { mainhand, offhand, feet, legs, chest, head };
inline constexpr std::size_t slot_count = 6;
// The player inventory slot of an equipment slot: the selected hotbar slot
// (0-8), the off hand, or an armor slot (5 the head to 8 the feet).
std::uint8_t inventory_slot(Slot s, std::uint8_t held) noexcept;

enum class EquipSound : std::uint8_t { generic, leather, chain, iron, gold, diamond, netherite, turtle, elytra };
// An item worn in an armor slot: the slot, what it gives there and its
// sound; `swappable` when using it puts it on, swapping it with what is worn.
struct Wearable {
    Slot slot{};
    float armor{};
    float toughness{};
    float knockback_resistance{};
    EquipSound sound{};
    bool swappable{};
};
const Wearable* wearable(std::uint16_t item);
// The attribute modifier id of an armor slot's item ("armor.helmet" to
// "armor.boots").
std::string_view armor_modifier(Slot s) noexcept;
// What a weapon or tool gives in the main hand: the attack damage and speed
// it adds to a hand's 1 and 4 (the base_attack_damage and base_attack_speed
// modifiers).
struct Weapon {
    float damage{};
    float speed{};
};
std::optional<Weapon> weapon(std::uint16_t item);

} // namespace circuit::gameplay::equipment
