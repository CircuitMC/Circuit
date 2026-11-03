// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Eating and drinking: the items players consume by holding use, as both
// official servers give them by default (client/eat). Nutrition and
// saturation from PrismarineJS minecraft-data foods.json: its foodPoints,
// and half its saturationRatio in pc/1.21.4 and pc/1.21.8 (the same for
// every food; pc/1.21.1 gives that as its saturation, and lists the ominous
// bottle as food, which both official servers do not treat as food); use
// times, the foods eaten with full hunger, what each leaves and its effects
// from minecraft.wiki ("Food", each item's page). Both official servers
// gave the same for the 18 kinds client/eat eats, in the ways it eats them.
namespace circuit::gameplay::food {

// The sound a use makes while it lasts, and a food's when it is eaten.
enum class Sound : std::uint8_t { eat, drink, honey_drink };
// What consuming does besides food and the item's effects.
enum class Special : std::uint8_t {
    none,
    clear_effects, // milk: every effect ends
    remove_poison, // honey
    teleport,      // chorus fruit: to a random place within 8 blocks
    stew,          // suspicious stew: the effects its stack names
    potion,        // a potion's effects, the instant ones at once
    ominous,       // an ominous bottle: bad omen at its stack's amplifier
};
// An effect consuming gives, in the item's order, with its chance.
struct Chance {
    std::string_view effect;
    std::int32_t amplifier{};
    std::int32_t duration{}; // ticks
    float probability{1.0f};
};
struct Consumable {
    std::string_view item;            // without "minecraft:"
    std::int32_t nutrition{};         // 0: no food (no hunger to wait for, no sounds at the end)
    float saturation_modifier{};      // saturation given: nutrition x modifier x 2
    bool always{};                    // eaten with full hunger
    std::int32_t ticks{32};           // the use's length
    Sound sound{Sound::eat};
    std::string_view remainder;       // the item left, "" for none
    std::span<const Chance> effects;
    Special special{};

    [[nodiscard]] bool food() const noexcept { return nutrition > 0; }
    [[nodiscard]] bool drink() const noexcept { return sound != Sound::eat; }
    [[nodiscard]] float saturation() const noexcept { return static_cast<float>(nutrition) * saturation_modifier * 2.0f; }
};

// The consumable an item is, if any.
const Consumable* of(std::uint16_t item);
// What an item leaves when consumed, if anything.
std::optional<std::uint16_t> remainder(std::uint16_t item);
// Whether a use of `ticks` makes its sound in a tick that leaves it `left`
// ticks (0: the tick it ends): in a tick that starts with a multiple of 4
// left, once more than a fifth of the use has passed, and in its last
// (client/eat: 7 sounds in 32 ticks, 4 in 16, 8 in 40, one in a use let go
// after 10 ticks and two in one switched away from after 16).
bool use_sound(std::int32_t ticks, std::int32_t left) noexcept;

} // namespace circuit::gameplay::food
