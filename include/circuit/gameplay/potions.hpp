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

// The effects a drunk potion gives, by its id (minecraft.wiki "Potion":
// each potion's effect, level and duration in ticks; 1 for the instant
// ones, which act at once; client/eat: regeneration's 900 ticks, and
// healing healing at once without an effect). None for an unknown id.
struct Effect {
    std::string_view effect; // without "minecraft:"
    std::int32_t amplifier{};
    std::int32_t duration{};
};
std::span<const Effect> effects(std::uint32_t potion) noexcept;

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
