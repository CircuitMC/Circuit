// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
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
std::optional<std::uint16_t> by_name(std::string_view name) noexcept;

} // namespace circuit::gameplay::items
