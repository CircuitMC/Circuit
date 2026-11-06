// SPDX-License-Identifier: AGPL-3.0-only
// How damage to players is reduced and how players burn (client/fire).
#include <circuit/gameplay/damage.hpp>
#include <algorithm>

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

} // namespace circuit::gameplay::damage
