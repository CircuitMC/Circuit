// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Potions and brewing. The potion registry is built into the game (not a
// data pack's): its entries in id order, as both official servers number
// them (their brewing report, tests/fixtures/collision/brewing-*.txt;
// minecraft.wiki "Potion#Data values").
namespace circuit::gameplay::potions {

std::span<const std::string_view> names() noexcept;
std::optional<std::uint32_t> id(std::string_view name) noexcept;

// The ingredients a brewing stand takes (minecraft.wiki "Brewing").
bool ingredient(std::uint16_t item) noexcept;

// What brewing makes of a bottle (a potion, splash potion or lingering
// potion item with its potion) with an ingredient: the item and potion it
// becomes, or nullopt without a mix. Gunpowder makes potions splash
// potions and dragon's breath splash potions lingering ones, keeping the
// potion; the other ingredients change the potion and keep the item.
struct Bottle {
    std::uint16_t item{};
    std::string_view potion;
    bool operator==(const Bottle&) const = default;
};
std::optional<Bottle> mix(const Bottle& bottle, std::uint16_t ingredient) noexcept;

} // namespace circuit::gameplay::potions
