// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/item_ids.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Item registry of the reference rules: every item of the supported versions,
// from public item lists (see tools/compile_item_registry.py). The rules give
// behavior to some; the others are inert and can be stored, moved, dropped
// and picked up.
namespace circuit::gameplay::items {

struct ItemInfo {
    std::string_view name;   // e.g. "minecraft:stone"
    std::uint8_t max_stack;  // 1, 16 or 64
    std::uint16_t max_damage; // durability; 0 for items without
};

// Indexed by id - 1.
std::span<const ItemInfo> registry() noexcept;

constexpr bool valid(std::uint16_t id) noexcept { return id >= 1 && id <= max_id; }
inline const ItemInfo* info(std::uint16_t id) noexcept { return valid(id) ? &registry()[id - 1u] : nullptr; }
inline std::uint8_t max_stack(std::uint16_t id) noexcept { return valid(id) ? registry()[id - 1u].max_stack : 0; }
// The stack's own limit: its max_stack_size component, else the item's.
inline std::uint8_t max_stack(const contracts::ItemStack& s) noexcept {
    if (const auto m = s.components.typed().max_stack_size) return *m;
    return max_stack(s.item);
}
// Durability of the stack: its max_damage component, else the item's.
inline std::int32_t max_damage(const contracts::ItemStack& s) noexcept {
    if (const auto m = s.components.typed().max_damage) return *m;
    return valid(s.item) ? registry()[s.item - 1u].max_damage : 0;
}
std::optional<std::uint16_t> by_name(std::string_view name) noexcept;

} // namespace circuit::gameplay::items
