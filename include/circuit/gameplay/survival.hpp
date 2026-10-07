// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/block.hpp>

// Survival rules of the reference rules: how fast blocks break and what they
// drop, from the target version's documented hardness, tool tiers and
// harvest requirements (PrismarineJS minecraft-data at the pinned commit).
namespace circuit::gameplay {

// Destroy time of a block; negative for blocks players cannot break.
float hardness(BlockType t) noexcept;
// Whether the tool harvests the block (gets its drops).
bool harvests(BlockState s, contracts::ItemStack tool) noexcept;
// Fraction of the block a player breaks per tick: the tool's speed when it
// is effective on the block (1 otherwise), divided by five with the eyes in
// water and again off the ground, over the hardness and over 30 when the
// tool harvests the block, else 100. 1 or more breaks the block at once.
float destroy_progress(BlockState s, contracts::ItemStack tool, bool on_ground, bool eyes_in_water = false) noexcept;
// Height of the fluid in a cell, in blocks: 1 under the same fluid, else its
// amount (8 for sources and falling water, 8 - distance when flowing) / 9.
float fluid_height(BlockState s, BlockState above) noexcept;

} // namespace circuit::gameplay
