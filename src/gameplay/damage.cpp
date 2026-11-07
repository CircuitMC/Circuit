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
// to death from 12 health).
std::int32_t touch_fire(std::int32_t ticks, bool more) noexcept {
    if (ticks < 0) ++ticks;
    else ticks += more ? 2 : 1;
    return ticks >= 0 ? std::max(ticks, 160) : ticks;
}
std::int32_t touch_lava(std::int32_t ticks) noexcept { return std::max(ticks, 300); }

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
