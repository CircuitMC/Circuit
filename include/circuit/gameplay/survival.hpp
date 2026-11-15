// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <cstdint>

// Survival rules of the reference rules: how fast blocks break and what they
// drop, from the target version's documented hardness, tool tiers and
// harvest requirements (PrismarineJS minecraft-data at the pinned commit).
namespace circuit::gameplay {

// Destroy time of a block; negative for blocks players cannot break.
float hardness(BlockState s) noexcept;
// Whether the tool harvests the block (gets its drops). With `data`, blocks
// without rules of their own take their tool and tier from it.
bool harvests(BlockState s, const contracts::ItemStack& tool, const GameData* data = nullptr) noexcept;
// The effects on a player's digging speed, by amplifier (-1: none): haste
// or conduit power, the stronger of them, and mining fatigue.
struct DigEffects {
    std::int32_t haste{-1};
    std::int32_t fatigue{-1};
    float efficiency{};       // the mining efficiency attribute (efficiency)
    float submerged{0.2f};    // the submerged mining speed attribute (aqua affinity)
};
// Fraction of the block a player breaks per tick: the tool's speed when it
// is effective on the block (1 otherwise), 20% faster per level of haste
// and times 0.3, 0.09, 0.0027 or 0.00081 by the level of mining fatigue
// (minecraft.wiki "Haste", "Mining Fatigue"; client/dig_effects: haste,
// conduit power and fatigue as both official servers count them), the
// mining efficiency added to a speed above 1 first (efficiency;
// client/ench_dig), times the submerged mining speed with the eyes in water
// (0.2, aqua affinity's 1) and divided by five off the ground, over the
// hardness and over 30 when the tool harvests the block, else 100. 1 or
// more breaks the block at once.
float destroy_progress(BlockState s, const contracts::ItemStack& tool, bool on_ground, bool eyes_in_water = false,
                       const GameData* data = nullptr, DigEffects effects = {}) noexcept;
// Height of the fluid in a cell, in blocks: 1 under the same fluid, else its
// amount (8 for sources and falling water, 8 - distance when flowing) / 9.
float fluid_height(BlockState s, BlockState above) noexcept;

} // namespace circuit::gameplay
