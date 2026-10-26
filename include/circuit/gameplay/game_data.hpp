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
#include <string_view>
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
    // The smithing recipes, in recipe id order, and the items they take as
    // templates, bases and additions (ascending; what the smithing table's
    // slots accept, and the property sets clients receive).
    std::vector<recipes::Smithing> smithing;
    std::vector<std::uint16_t> smithing_template, smithing_base, smithing_addition;
    // Trim materials by the item that gives them, and (for packs before
    // 1.21.5) trim patterns by their template item: ids of the trim
    // registries ("minecraft:iron").
    std::unordered_map<std::uint16_t, std::string> trim_materials, trim_patterns;
    // A stack's components with a trim of this material and pattern (ids of
    // the trim registries); nullopt when the registries lack them. Set by
    // the server, which knows the components' encoding.
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::string_view material, std::string_view pattern)> trimmed;
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
    // A loom's patterns, in the order its buttons number them: those of the
    // banner pattern tag no_item_required, or a pattern item's (by the
    // item, from the tag pattern_item/<its name less _banner_pattern>).
    std::vector<std::string> loom_patterns;
    std::unordered_map<std::uint16_t, std::vector<std::string>> pattern_item_patterns;
    // A banner's components with a layer of a pattern (an id of the banner
    // pattern registry) in a dye color added on top; set by the server.
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::string_view pattern, std::uint32_t color)>
        with_banner_layer;
    // The curses (the enchantment tag curse), ascending ids of the synchronized registry.
    std::vector<std::uint32_t> curses;
    // 1.21.4: a book a grindstone makes of an enchanted book keeps an empty
    // stored_enchantments component (1.21.8 leaves it out).
    bool keeps_empty_book_enchantments{};
    // A stack's enchantments, or stored enchantments (`stored`), as (id,
    // level) pairs; and its components with them replaced (left out when
    // none, unless `keep_empty`) and its repair cost set. Set by the server,
    // which knows the components' encoding.
    std::function<std::vector<std::pair<std::uint32_t, std::int32_t>>(const contracts::ComponentPatch&, bool stored)> enchantments_of;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, bool stored,
                                                            const std::vector<std::pair<std::uint32_t, std::int32_t>>& levels, bool keep_empty,
                                                            std::int32_t repair_cost)>
        with_enchantments;
    // A stack's potion (an id of the built-in potion registry, potions.hpp)
    // and its components with the potion set (their other potion contents
    // kept); set by the server, which knows the components' encoding.
    std::function<std::optional<std::string_view>(const contracts::ComponentPatch&)> potion_of;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::string_view potion)> with_potion;
    // What fuels brewing stands: the items of the item tag brewing_fuel, ascending.
    std::vector<std::uint16_t> brewing_fuel;
};

} // namespace circuit::gameplay
