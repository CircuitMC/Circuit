// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/experience.hpp>
#include <algorithm>
#include <climits>

namespace circuit::gameplay::experience {

void give_points(PlayerInventory& player, std::int32_t points) noexcept {
    player.xp_progress += static_cast<float>(points) / static_cast<float>(needed(player.xp_level));
    player.xp_total = static_cast<std::int32_t>(std::clamp<std::int64_t>(std::int64_t{player.xp_total} + points, 0, INT_MAX));
    while (player.xp_progress >= 1.0f) {
        player.xp_progress = (player.xp_progress - 1.0f) * static_cast<float>(needed(player.xp_level));
        if (player.xp_level < INT_MAX) ++player.xp_level;
        player.xp_progress /= static_cast<float>(needed(player.xp_level));
    }
}

} // namespace circuit::gameplay::experience
