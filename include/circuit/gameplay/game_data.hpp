// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/loot.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Game data that data packs define, in the form the rules use. An engine
// without it uses the public registry data (block_registry.hpp). It is fixed
// for the life of an engine, so a log replays under the data it was written
// with (the fingerprint names it).
namespace circuit::gameplay {

struct BlockTool {
    registry::Tool tool{};
    std::uint8_t tier{}; // lowest tool tier that harvests the block; 0: any
};

struct GameData {
    std::string fingerprint{"none"};
    // Per internal registry block: the tool that mines it faster and the
    // tier its drops need, from the mineable and needs-tool block tags.
    std::unordered_map<std::uint32_t, BlockTool> block_tools;
    // Per internal registry block without rules of its own: its loot table,
    // when the table is in the deterministic subset (loot.hpp).
    std::unordered_map<std::uint32_t, loot::Table> block_loot;
    // The crafting recipes the rules match, in recipe id order.
    std::vector<recipes::Recipe> crafting;
    // The level of an enchantment (an id of the synchronized registry) on a
    // stack; 0 when absent. Set by the server, which reads the components.
    std::function<long(const contracts::ItemStack&, std::uint32_t enchantment)> enchantment_level;
};

} // namespace circuit::gameplay
