// SPDX-License-Identifier: AGPL-3.0-only
// The status effect registry and how effects combine and count down, as both
// official servers show them (circuit-clienttest/effect-colors.js,
// effects3.js and effects4.js, both versions identical).
#include <circuit/gameplay/effects.hpp>
#include <algorithm>
#include <array>

namespace circuit::gameplay::effects {
namespace {
constexpr std::array<std::string_view, count> effect_names{
    "speed", "slowness", "haste", "mining_fatigue", "strength", "instant_health", "instant_damage", "jump_boost", "nausea",
    "regeneration", "resistance", "fire_resistance", "water_breathing", "invisibility", "blindness", "night_vision", "hunger",
    "weakness", "poison", "wither", "health_boost", "absorption", "saturation", "glowing", "levitation", "luck", "unluck",
    "slow_falling", "conduit_power", "dolphins_grace", "bad_omen", "hero_of_the_village", "darkness", "trial_omen", "raid_omen",
    "wind_charged", "weaving", "oozing", "infested"};

// The entity_effect particle's color of each effect (0: a particle of its
// own, below), as both official servers send it.
constexpr std::array<std::uint32_t, count> colors{
    0xFF33EBFF, 0xFF8BAFE0, 0xFFD9C043, 0xFF4A4217, 0xFFFFC700, 0xFFF82423, 0xFFA9656A, 0xFFFDFF84, 0xFF551D4A, 0xFFCD5CAB,
    0xFF9146F0, 0xFFFF9900, 0xFF98DAC0, 0xFFF6F6F6, 0xFF1F1F23, 0xFFC2FF66, 0xFF587653, 0xFF484D48, 0xFF87A363, 0xFF736156,
    0xFFF87D23, 0xFF2552A5, 0xFFF82423, 0xFF94A061, 0xFFCEFFFF, 0xFF59C106, 0xFFC0A44D, 0xFFF3CFB9, 0xFF1DC2D1, 0xFF88A3BE,
    0xFF0B6138, 0xFF44FF44, 0xFF292721, 0, 0, 0, 0, 0, 0};
constexpr std::array<std::string_view, 6> own_particles{"trial_omen", "raid_omen", "small_gust", "item_cobweb", "item_slime", "infested"};
constexpr std::uint8_t trial_omen = 33;
} // namespace

std::span<const std::string_view> names() noexcept { return effect_names; }

std::optional<std::uint8_t> id(std::string_view name) noexcept {
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    const auto it = std::ranges::find(effect_names, name);
    if (it == effect_names.end()) return std::nullopt;
    return static_cast<std::uint8_t>(it - effect_names.begin());
}

bool instant(std::uint8_t effect) noexcept { return effect == 5 || effect == 6 || effect == 22; }

bool acts(std::uint8_t effect, std::int32_t time, std::int32_t amplifier) noexcept {
    const auto every = [&](std::int32_t base) {
        const auto interval = amplifier >= 0 && amplifier < 31 ? base >> amplifier : 0;
        return interval <= 0 || time % interval == 0;
    };
    switch (effect) {
    case 5:  // instant health
    case 6:  // instant damage
    case 16: // hunger
    case 22: // saturation
        return true;
    case 9: return every(50);  // regeneration
    case 18: return every(25); // poison
    case 19: return every(40); // wither
    default: return false;
    }
}

bool is_beacon_power(std::uint8_t effect) noexcept {
    // speed, haste, strength, jump boost, regeneration, resistance
    return effect == 0 || effect == 2 || effect == 4 || effect == 7 || effect == 9 || effect == 10;
}

Particle particle(std::uint8_t effect) noexcept {
    if (effect >= count) return {};
    if (effect >= trial_omen) return {own_particles[effect - trial_omen], 0};
    return {"entity_effect", colors[effect]};
}

std::uint8_t shared_flag(std::uint8_t effect) noexcept {
    if (effect == 13) return 0x20; // invisibility
    if (effect == 23) return 0x40; // glowing
    return 0;
}

bool shorter(const Level& a, const Level& b) noexcept {
    if (a.duration == infinite) return false;
    return b.duration == infinite || a.duration < b.duration;
}

namespace {
bool update(std::vector<Level>& levels, std::size_t at, const Level& other) {
    bool changed = false;
    if (other.amplifier > levels[at].amplifier) {
        // The shown level stays underneath when it lasts longer.
        if (shorter(other, levels[at])) levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(at) + 1, levels[at]);
        levels[at].amplifier = other.amplifier;
        levels[at].duration = other.duration;
        changed = true;
    } else if (shorter(levels[at], other)) {
        if (other.amplifier == levels[at].amplifier) {
            levels[at].duration = other.duration;
            changed = true;
        } else if (at + 1 == levels.size()) {
            levels.push_back(other);
        } else {
            update(levels, at + 1, other);
        }
    }
    auto& shown = levels[at];
    if ((!other.ambient && shown.ambient) || changed) {
        shown.ambient = other.ambient;
        changed = true;
    }
    if (other.particles != shown.particles) {
        shown.particles = other.particles;
        changed = true;
    }
    if (other.icon != shown.icon) {
        shown.icon = other.icon;
        changed = true;
    }
    return changed;
}
} // namespace

bool give(Effect& effect, const Level& level) {
    if (effect.levels.empty()) {
        effect.levels.push_back(level);
        return true;
    }
    return update(effect.levels, 0, level);
}

Ticked tick(Effect& effect) {
    Ticked out;
    if (effect.levels.empty()) {
        out.ended = true;
        return out;
    }
    for (auto& level : effect.levels)
        if (level.duration > 0) --level.duration;
    if (effect.levels.front().duration == 0) {
        effect.levels.erase(effect.levels.begin());
        out.next = !effect.levels.empty();
        // A level underneath that ran out while it waited is shown, and the
        // effect ends with it (client/effect_edges).
        out.ended = effect.levels.empty() || effect.levels.front().duration == 0;
    }
    if (!out.ended) out.again = effect.levels.front().duration > 0 && effect.levels.front().duration % 600 == 0;
    return out;
}

} // namespace circuit::gameplay::effects
