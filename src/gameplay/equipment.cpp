// SPDX-License-Identifier: AGPL-3.0-only
// What players wear and hold (client/armor).
#include <circuit/gameplay/equipment.hpp>
#include "internal.hpp"
#include <circuit/contracts/commands.hpp>
#include <circuit/gameplay/items.hpp>
#include <array>
#include <string>
#include <unordered_map>

namespace circuit::gameplay::equipment {
namespace {
using enum Slot;
using enum EquipSound;

struct Worn {
    std::string_view item;
    Wearable wearable;
};
// The armor of each material by slot (minecraft.wiki "Armor": armor points,
// toughness, netherite's knockback resistance, the double 0.1 as
// client/ench_combat's knockback against a netherite chestplate shows;
// client/armor: the modifiers of the recorded pieces), the turtle shell, the
// elytra, and what is worn on the head without armor and not put on by use
// (client/armor: a carved pumpkin over a worn helmet; client/armor2: a carved
// pumpkin and a zombie head with nothing on the head; the other heads by the
// same rule); which
// wear as their wearer is hurt (minecraft.wiki "Durability": armor; the
// elytra's durability goes by gliding).
const std::array<Worn, 34> worn{{
    {"leather_helmet", {head, 1, 0, 0, leather, true, true}},
    {"leather_chestplate", {chest, 3, 0, 0, leather, true, true}},
    {"leather_leggings", {legs, 2, 0, 0, leather, true, true}},
    {"leather_boots", {feet, 1, 0, 0, leather, true, true}},
    {"chainmail_helmet", {head, 2, 0, 0, chain, true, true}},
    {"chainmail_chestplate", {chest, 5, 0, 0, chain, true, true}},
    {"chainmail_leggings", {legs, 4, 0, 0, chain, true, true}},
    {"chainmail_boots", {feet, 1, 0, 0, chain, true, true}},
    {"iron_helmet", {head, 2, 0, 0, iron, true, true}},
    {"iron_chestplate", {chest, 6, 0, 0, iron, true, true}},
    {"iron_leggings", {legs, 5, 0, 0, iron, true, true}},
    {"iron_boots", {feet, 2, 0, 0, iron, true, true}},
    {"golden_helmet", {head, 2, 0, 0, gold, true, true}},
    {"golden_chestplate", {chest, 5, 0, 0, gold, true, true}},
    {"golden_leggings", {legs, 3, 0, 0, gold, true, true}},
    {"golden_boots", {feet, 1, 0, 0, gold, true, true}},
    {"diamond_helmet", {head, 3, 2, 0, diamond, true, true}},
    {"diamond_chestplate", {chest, 8, 2, 0, diamond, true, true}},
    {"diamond_leggings", {legs, 6, 2, 0, diamond, true, true}},
    {"diamond_boots", {feet, 3, 2, 0, diamond, true, true}},
    {"netherite_helmet", {head, 3, 3, 0.1, netherite, true, true}},
    {"netherite_chestplate", {chest, 8, 3, 0.1, netherite, true, true}},
    {"netherite_leggings", {legs, 6, 3, 0.1, netherite, true, true}},
    {"netherite_boots", {feet, 3, 3, 0.1, netherite, true, true}},
    {"turtle_helmet", {head, 2, 0, 0, turtle, true, true}},
    {"elytra", {chest, 0, 0, 0, elytra, true}},
    {"carved_pumpkin", {head, 0, 0, 0, generic, false}},
    {"skeleton_skull", {head, 0, 0, 0, generic, false}},
    {"wither_skeleton_skull", {head, 0, 0, 0, generic, false}},
    {"player_head", {head, 0, 0, 0, generic, false}},
    {"zombie_head", {head, 0, 0, 0, generic, false}},
    {"creeper_head", {head, 0, 0, 0, generic, false}},
    {"dragon_head", {head, 0, 0, 0, generic, false}},
    {"piglin_head", {head, 0, 0, 0, generic, false}},
}};

// The attack damage and speed weapons and tools add when held (minecraft.wiki
// "Damage", "Sword", "Axe", "Pickaxe", "Shovel", "Hoe", "Trident", "Mace":
// their damage less a hand's 1, their attacks a second less a hand's 4), as
// the floats both official servers send (client/armor: an iron sword's
// attack speed -2.4000000953674316, a diamond axe's -3).
struct Held {
    std::string_view item;
    Weapon weapon;
};
constexpr std::array<Held, 32> held{{
    {"wooden_sword", {3, -2.4f}},     {"stone_sword", {4, -2.4f}},    {"iron_sword", {5, -2.4f}},
    {"golden_sword", {3, -2.4f}},     {"diamond_sword", {6, -2.4f}},  {"netherite_sword", {7, -2.4f}},
    {"wooden_axe", {6, -3.2f}},       {"stone_axe", {8, -3.2f}},      {"iron_axe", {8, -3.1f}},
    {"golden_axe", {6, -3.0f}},       {"diamond_axe", {8, -3.0f}},    {"netherite_axe", {9, -3.0f}},
    {"wooden_pickaxe", {1, -2.8f}},   {"stone_pickaxe", {2, -2.8f}},  {"iron_pickaxe", {3, -2.8f}},
    {"golden_pickaxe", {1, -2.8f}},   {"diamond_pickaxe", {4, -2.8f}}, {"netherite_pickaxe", {5, -2.8f}},
    {"wooden_shovel", {1.5f, -3.0f}}, {"stone_shovel", {2.5f, -3.0f}}, {"iron_shovel", {3.5f, -3.0f}},
    {"golden_shovel", {1.5f, -3.0f}}, {"diamond_shovel", {4.5f, -3.0f}}, {"netherite_shovel", {5.5f, -3.0f}},
    {"wooden_hoe", {0, -3.0f}},       {"stone_hoe", {0, -2.0f}},      {"iron_hoe", {0, -1.0f}},
    {"golden_hoe", {0, -3.0f}},       {"diamond_hoe", {0, 0.0f}},     {"netherite_hoe", {0, 0.0f}},
    {"trident", {8, -2.9f}},          {"mace", {5, -3.4f}},
}};

template <typename T, std::size_t N>
std::unordered_map<std::uint16_t, const T*> by_id(const std::array<T, N>& table) {
    std::unordered_map<std::uint16_t, const T*> m;
    for (const auto& e : table)
        if (const auto id = items::by_name("minecraft:" + std::string(e.item))) m.emplace(*id, &e);
    return m;
}
} // namespace

std::uint8_t inventory_slot(Slot s, std::uint8_t held_slot) noexcept {
    switch (s) {
    case mainhand: return static_cast<std::uint8_t>(contracts::hotbar_first + held_slot);
    case offhand: return contracts::offhand_slot;
    case feet: return 8;
    case legs: return 7;
    case chest: return 6;
    case head: return 5;
    }
    return 0;
}

const Wearable* wearable(std::uint16_t item) {
    static const auto map = by_id(worn);
    const auto it = map.find(item);
    return it == map.end() ? nullptr : &it->second->wearable;
}

std::optional<contracts::ItemStack> worn_down(const contracts::ItemStack& piece, std::int32_t amount, bool fire) {
    const auto* worn = wearable(piece.item);
    const auto most = items::max_damage(piece); // the stack's own (its max_damage component)
    if (piece.empty() || !worn || !worn->wears || most <= 0 || amount < 1) return std::nullopt;
    if (fire && detail::fire_resistant(piece)) return std::nullopt;
    const auto damage = static_cast<std::int64_t>(piece.damage) + amount;
    if (damage >= most) return contracts::ItemStack{};
    auto out = piece;
    out.damage = static_cast<std::uint16_t>(damage);
    return out;
}

std::string_view slot_name(Slot s) noexcept {
    switch (s) {
    case mainhand: return "mainhand";
    case offhand: return "offhand";
    case feet: return "feet";
    case legs: return "legs";
    case chest: return "chest";
    case head: return "head";
    }
    return {};
}

std::string_view armor_modifier(Slot s) noexcept {
    switch (s) {
    case head: return "armor.helmet";
    case chest: return "armor.chestplate";
    case legs: return "armor.leggings";
    case feet: return "armor.boots";
    default: return {};
    }
}

std::optional<Weapon> weapon(std::uint16_t item) {
    static const auto map = by_id(held);
    const auto it = map.find(item);
    if (it == map.end()) return std::nullopt;
    return it->second->weapon;
}

std::int32_t attack_wear(std::uint16_t item) {
    static const auto map = by_id(held);
    const auto it = map.find(item);
    if (it == map.end()) return 0;
    const std::string_view name = it->second->item;
    return name.ends_with("_sword") || name == "trident" || name == "mace" ? 1 : 2;
}

bool sweeps(std::uint16_t item) {
    static const auto map = by_id(held);
    const auto it = map.find(item);
    return it != map.end() && it->second->item.ends_with("_sword");
}

} // namespace circuit::gameplay::equipment
