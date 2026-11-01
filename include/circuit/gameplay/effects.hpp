// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Status effects. The effect registry is built into the game: its entries in
// id order, the same on both versions (minecraft.wiki "Effect", data values;
// PrismarineJS minecraft-data effects.json), with what both official servers
// show of each effect (circuit-clienttest/effect-colors.js: its particle and
// the shared flag it sets), how an effect given again combines with the one
// a player has (effects3.js) and how it counts down (effects4.js).
namespace circuit::gameplay::effects {

inline constexpr std::size_t count = 39;
// The names without their namespace, in id order.
std::span<const std::string_view> names() noexcept;
// An effect by name, with or without "minecraft:".
std::optional<std::uint8_t> id(std::string_view name) noexcept;

// Instant health, instant damage and saturation: /effect counts their
// duration in ticks where others count seconds.
bool instant(std::uint8_t effect) noexcept;
// The powers a beacon gives: speed, haste, resistance, jump boost, strength
// and regeneration (minecraft.wiki "Beacon").
bool is_beacon_power(std::uint8_t effect) noexcept;

// The particle an effect shows: entity_effect in an ARGB color, or a
// particle without data (trial and raid omens, wind charged's small gust,
// weaving's cobweb, oozing's slime, infested).
struct Particle {
    std::string_view type;
    std::uint32_t color{}; // entity_effect only
    bool operator==(const Particle&) const = default;
};
Particle particle(std::uint8_t effect) noexcept;

// The bit an effect sets in its entity's shared flags: invisibility 0x20,
// glowing 0x40; 0 for the others.
std::uint8_t shared_flag(std::uint8_t effect) noexcept;

inline constexpr std::int32_t infinite = -1;

// One level of an effect.
struct Level {
    std::int32_t amplifier{};
    std::int32_t duration{}; // ticks, or infinite
    bool ambient{};
    bool particles{true};
    bool icon{true};
    bool operator==(const Level&) const = default;
};
// An effect a player has: the level shown, then the weaker, longer levels
// kept underneath it, each coming back when the one above it ends.
struct Effect {
    std::uint8_t id{};
    std::vector<Level> levels; // levels[0] is the one shown
    bool operator==(const Effect&) const = default;
};

// Whether a level ends before another (an infinite one never does).
bool shorter(const Level& a, const Level& b) noexcept;

// Gives a level of an effect the player has, as both official servers do:
// a stronger level replaces the shown one, which stays underneath when it
// lasts longer; the same amplifier for longer lengthens it; a weaker, longer
// level is given to the level underneath by the same rules (or becomes it)
// without changing what is shown; a change of the particles, the icon or
// the ambience changes the shown level too. Returns whether it changed.
bool give(Effect& effect, const Level& level);

// One tick: every level with time left counts down; when the shown one
// ends, the next one underneath is shown or the effect ends (a level
// underneath that ran out while it waited is shown, and the effect ends in
// the same tick with any level still under it: effects5.js, with two and
// three levels). Both official servers send an effect again
// whenever its time left is a multiple of 600 ticks (effects4.js and
// effects5.js: given for 620, 1220 and 1820 ticks, sent again 20 ticks
// later; not for 600 or an infinite time).
struct Ticked {
    bool ended{};
    bool next{};  // the level underneath is shown now (also when it ends at once)
    bool again{}; // the time left is a multiple of 600 ticks
};
Ticked tick(Effect& effect);

} // namespace circuit::gameplay::effects
