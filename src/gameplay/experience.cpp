// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/experience.hpp>
#include <circuit/gameplay/rng.hpp>
#include <algorithm>
#include <climits>

namespace circuit::gameplay::experience {

void give_points(PlayerInventory& player, std::int32_t points) noexcept {
    player.xp_progress += static_cast<float>(points) / static_cast<float>(needed(player.xp_level));
    // The total wraps as a 32-bit sum before it is kept from going below 0
    // (both official servers: a total of 30 given the largest int has 0).
    player.xp_total = std::max(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(player.xp_total) + static_cast<std::uint32_t>(points)));
    while (player.xp_progress < 0.0f) {
        const float points_short = player.xp_progress * static_cast<float>(needed(player.xp_level));
        if (player.xp_level > 0) {
            give_levels(player, -1);
            player.xp_progress = 1.0f + points_short / static_cast<float>(needed(player.xp_level));
        } else {
            give_levels(player, -1);
            player.xp_progress = 0.0f;
        }
    }
    while (player.xp_progress >= 1.0f) {
        player.xp_progress = (player.xp_progress - 1.0f) * static_cast<float>(needed(player.xp_level));
        if (player.xp_level < INT_MAX) ++player.xp_level;
        player.xp_progress /= static_cast<float>(needed(player.xp_level));
    }
}

void give_levels(PlayerInventory& player, std::int32_t levels) noexcept {
    player.xp_level = static_cast<std::int32_t>(std::clamp<std::int64_t>(std::int64_t{player.xp_level} + levels, INT_MIN, INT_MAX));
    if (player.xp_level < 0) {
        player.xp_level = 0;
        player.xp_progress = 0.0f;
        player.xp_total = 0;
    }
}

void set_points(PlayerInventory& player, std::int32_t points) noexcept {
    const auto need = static_cast<float>(needed(player.xp_level));
    player.xp_progress = std::clamp(static_cast<float>(points) / need, 0.0f, (need - 1.0f) / need);
}

std::int32_t enchantment_seed(const PlayerInventory& player) noexcept {
    if (player.xp_seed != 0) return player.xp_seed;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(Rng::derive({player.player.hi, player.player.lo, 0x5850534545440000ULL}).next()));
}

} // namespace circuit::gameplay::experience
