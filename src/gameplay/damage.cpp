// SPDX-License-Identifier: AGPL-3.0-only
// How damage to players is reduced and how players burn (client/fire),
// drown and freeze (client/drown).
#include <circuit/gameplay/damage.hpp>
#include <algorithm>
#include <cmath>

namespace circuit::gameplay::damage {

// minecraft.wiki "Armor": armor - 4 * damage / (toughness + 8), between a
// fifth of the armor and 20, over 25 (client/fire: 13 armor and 5
// toughness leave 0.49230766 of 1; 10 and 2 leave 0.616 of 1; 13 and 5
// leave 2.1169229 of 4, to the float).
float after_armor(float damage, float armor, float toughness) noexcept {
    const float effective = std::clamp(armor - 4.0f * damage / (toughness + 8.0f), armor / 5.0f, 20.0f);
    return damage * (1.0f - effective / 25.0f);
}

// client/fire: resistance II leaves 0.6 of 1.
float after_resistance(float damage, std::int32_t amplifier) noexcept {
    const std::int32_t left = 25 - std::clamp(amplifier + 1, 0, 5) * 5;
    return std::max(damage * static_cast<float>(left) / 25.0f, 0.0f);
}

// client/ench_protect: a fall of 7 with 12 points leaves 3.64 (20 health to
// 16.360000610351562), with 20 points 1.4; magic of 6 with 4 points and
// resistance I 4.0320005 (15.967999458312988: resistance first).
float after_enchantments(float damage, float protection) noexcept {
    if (!(protection > 0.0f)) return damage; // and NaN, from a data pack's value
    const float points = std::min(protection, 20.0f);
    return damage * (1.0f - points / 25.0f);
}

// client/fire: hits of 1 and of 4 each wear a piece by one.
std::int32_t armor_wear(float damage) noexcept {
    const float quarter = damage / 4.0f;
    if (!(quarter >= 1.0f)) return 1; // and NaN
    return static_cast<std::int32_t>(std::min(quarter, 1048576.0f));
}

// client/fire: a player in fire burns from the 20th tick in it (the
// burning's first hurt in the next tick, its flag in the one after), and
// burns 8 seconds and a few ticks once out right after; lava's burning
// hurts 14 times after it (minecraft.wiki "Lava"; client/fire: 12 times
// to death from 12 health). With fire protection (client/ench_fire) lava's
// burning hurts 12 times after it with I (a burning time of 0.85), 7 with
// III (0.55), 3 with IV and I (0.25), and never with IV on four pieces (0).
// Rounding down is Circuit's choice: the recorded counts are the same
// rounded to the nearest (255, 165, 75). Fire's 160 scaled alike was not
// recorded; a burning time of 0 leaves the fire ticks as they are (not
// recorded but for never being shown burning).
namespace {
std::int32_t scaled(std::int32_t ticks, double burning) noexcept {
    const double t = std::floor(static_cast<double>(ticks) * burning);
    return t > 0.0 ? static_cast<std::int32_t>(std::min(t, 1.0e9)) : 0; // and NaN
}
} // namespace
std::int32_t touch_fire(std::int32_t ticks, bool more, double burning) noexcept {
    if (ticks < 0) ++ticks;
    else ticks += more ? 2 : 1;
    return ticks >= 0 ? std::max(ticks, scaled(160, burning)) : ticks;
}
std::int32_t touch_lava(std::int32_t ticks, double burning) noexcept {
    const auto set = scaled(300, burning);
    return set > 0 ? std::max(ticks, set) : ticks;
}
std::int32_t ignite(std::int32_t ticks, float seconds, double burning) noexcept {
    const float base = std::floor(seconds * 20.0f);
    if (!(base > 0.0f)) return ticks; // and NaN
    const auto set = scaled(static_cast<std::int32_t>(std::min(base, 1.0e9f)), burning);
    return set > 0 ? std::max(ticks, set) : ticks;
}
// client/fire: below 0 (a second fire after one taken away took 20 ticks
// again); 0, which only fire at a burning time of 0 leaves, alike by
// Circuit's choice (not recorded).
std::int32_t out_of_fire(std::int32_t ticks) noexcept { return ticks <= 0 ? not_burning : ticks; }

Burn burn(std::int32_t ticks) noexcept {
    if (ticks <= 0) return {ticks, false};
    const bool hurts = ticks % 20 == 0;
    --ticks;
    return {ticks == 0 ? not_burning : ticks, hurts};
}

// client/drown: 300 to -19 a tick at a time under water, the drowning hurt
// as it would reach -20 (then 0, told as 0 at the next tick); 4 a tick
// back out of water and with water breathing under it.
Breath breathe(std::int32_t air, bool losing) noexcept {
    if (!losing) return {std::min(air + 4, max_air), false};
    if (--air <= -20) return {0, true};
    return {air, false};
}

// client/drown: 1 to 140 a tick in powder snow, 138 down to 0 by two out of
// it; leather boots keep it at 0.
std::int32_t frost(std::int32_t ticks, bool freezing) noexcept {
    return freezing ? std::min(ticks + 1, frozen_max) : std::max(ticks - 2, 0);
}

// client/drown: -0.0003571428533177823 at 1 tick, -0.05000000074505806 at
// 140: -0.05 times the share frozen, in floats.
double frost_slowness(std::int32_t ticks) noexcept {
    const float share = static_cast<float>(std::clamp(ticks, 0, frozen_max)) / static_cast<float>(frozen_max);
    return static_cast<double>(-0.05f * share);
}

// combat-timing: an iron sword (speed 4 - 2.4000000953674316) 12 ticks after
// the last attack keeps 5.999999 of 6, the fist 0.208 in the same tick.
float attack_charge(std::int32_t ticks, double attack_speed) noexcept {
    if (attack_speed <= 0.0) return 0.0f; // a full charge never comes (not recorded)
    const auto full = static_cast<float>(1.0 / attack_speed * 20.0);
    return std::clamp((static_cast<float>(ticks) + 0.5f) / full, 0.0f, 1.0f);
}

float charged_damage(float damage, float charge) noexcept { return damage * (0.2f + charge * charge * 0.8f); }

// client/ench_combat: sharpness V (3) at a charge of 0.04 adds 0.12, and a
// critical hit with it at full charge deals 6 * 1.5 + 3.
float melee_damage(float charged, float enchanted, float charge, bool critical) noexcept {
    return (critical ? charged * 1.5f : charged) + enchanted * charge;
}
// client/ench_combat: sweeping edge III (0.75) and sharpness V at a charge
// of 0.96 swept 7.889047622680664 off 20 (this order of the floats).
float sweep_damage(float charged, double ratio, float enchanted, float charge) noexcept {
    return (1.0f + static_cast<float>(ratio) * charged + enchanted) * charge;
}

// client/combat: a standing player (velocity (0, -0.0784, 0)) hit from the
// west moves (0.4, 0.3608, 0); a sprint's 0.5 after it, (0.7, 0.4, 0).
Velocity knock(Velocity before, double strength, double x, double z, bool on_ground) noexcept {
    const double length = std::sqrt(x * x + z * z);
    if (strength <= 0.0 || length < 1.0e-4) return before;
    const double dx = x / length * strength, dz = z / length * strength;
    return {before.x / 2.0 + dx, on_ground ? std::min(0.4, before.y / 2.0 + strength) : before.y, before.z / 2.0 + dz};
}

float hurt_angle(double dx, double dz, float yaw) noexcept {
    return static_cast<float>(std::atan2(dz, dx) * 57.2957763671875 - static_cast<double>(yaw));
}

std::int32_t damage_indicators(float health_lost) noexcept {
    return health_lost <= 0.0f ? 0 : static_cast<std::int32_t>(health_lost * 0.5f);
}

} // namespace circuit::gameplay::damage
