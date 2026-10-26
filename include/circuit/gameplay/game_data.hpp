// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/loot.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
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
    // Per internal registry block without rules of its own, and wheat: its
    // loot table, when the rules compile it (loot.hpp).
    std::unordered_map<std::uint32_t, loot::Table> block_loot;
    // The crafting recipes the rules match, in recipe id order.
    std::vector<recipes::Recipe> crafting;
    // The cooking recipes, in recipe id order.
    std::vector<recipes::Cooking> cooking;
    // The stonecutting recipes, ordered by id path and then namespace, as
    // both official servers list them to clients and number their buttons.
    std::vector<recipes::Stonecutting> stonecutting;
    // The level of an enchantment (an id of the synchronized registry) on a
    // stack; 0 when absent. Set by the server, which reads the components.
    std::function<long(const contracts::ItemStack&, std::uint32_t enchantment)> enchantment_level;
    // The components of a stack enchanted with these enchantments (namespaced
    // ids and levels), for the tools fixtures hand to players; nullopt when
    // the registry lacks one. Set by the server.
    std::function<std::optional<contracts::ComponentPatch>(const std::vector<std::pair<std::string, std::int32_t>>& levels)> enchanted;
    // The enchantments that keep ice a player breaks from melting (the
    // enchantment tag prevents_ice_melting), as ids of the synchronized registry.
    std::vector<std::uint32_t> keeps_ice;
};

} // namespace circuit::gameplay
