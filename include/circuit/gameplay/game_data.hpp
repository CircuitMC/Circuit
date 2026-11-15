// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/enchantments.hpp>
#include <circuit/gameplay/loot.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
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
    // The special crafting recipes of the data packs the rules make, in recipe
    // id order, the items armor dye takes (the item tag dyeable, ascending)
    // and those a decorated pot takes (decorated_pot_ingredients, ascending).
    std::vector<recipes::Special> special;
    std::vector<std::uint16_t> dyeable;
    std::vector<std::uint16_t> pot_ingredients;
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
    // Enchantments that set the experience a broken block gives (their
    // minecraft:block_experience effect of type minecraft:set: Silk Touch's
    // 0), as ids of the synchronized registry and the value.
    std::vector<std::pair<std::uint32_t, float>> block_experience_set;
    // Each enchantment's minimum cost (its min_cost: base and per level
    // above the first), by id of the synchronized registry: what a
    // grindstone's experience counts.
    std::unordered_map<std::uint32_t, std::pair<std::int32_t, std::int32_t>> enchantment_min_cost;
    // What an anvil asks of an enchantment (by id): its anvil cost, its
    // highest level, the items it can go on (ascending) and the enchantments
    // it excludes (its exclusive_set, ascending).
    struct EnchantmentRules {
        std::int32_t anvil_cost{1};
        std::int32_t max_level{1};
        std::vector<std::uint16_t> supported;
        std::vector<std::uint32_t> exclusive;
    };
    std::unordered_map<std::uint32_t, EnchantmentRules> enchantment_rules;
    // What each enchantment does to players (by id): the slots it works in,
    // the attribute modifiers it gives there, the protection it adds and the
    // damage it keeps off.
    std::unordered_map<std::uint32_t, enchantments::Effects> enchantment_effects;
    // The damage types enchantments do not protect against (the damage type
    // tags bypasses_effects and bypasses_enchantments), sorted.
    std::vector<std::string> damage_bypassing_enchantments;
    // The items that repair an item at an anvil (minecraft.wiki "Anvil"), by
    // the item: the members of its material's item tag.
    std::unordered_map<std::uint16_t, std::vector<std::uint16_t>> repair_materials;
    // A stack's repair cost (0 when none) and its custom name's plain text
    // (nullopt without one), and its components with the custom name set to
    // plain text or removed; set by the server.
    std::function<std::int32_t(const contracts::ComponentPatch&)> repair_cost_of;
    std::function<std::optional<std::string>(const contracts::ComponentPatch&)> custom_name_of;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, const std::optional<std::string>&)> with_custom_name;
    // Enchanting tables (minecraft.wiki "Enchanting mechanics"): the
    // enchantments of the enchantment tag in_enchanting_table in its order,
    // each with its id, weight, highest level, cost range (base and per
    // level above the first) and primary items (its supported items without
    // any; ascending); and the blocks that power a table and those that let
    // the power through (the block tags enchantment_power_provider and
    // enchantment_power_transmitter; internal registry blocks, ascending).
    struct TableEnchantment {
        std::uint32_t id{};
        std::int32_t weight{};
        std::int32_t max_level{1};
        std::pair<std::int32_t, std::int32_t> min_cost{};
        std::pair<std::int32_t, std::int32_t> max_cost{};
        std::vector<std::uint16_t> primary{};
    };
    std::vector<TableEnchantment> table_enchantments;
    std::vector<std::uint32_t> power_providers;
    std::vector<std::uint32_t> power_transmitters;
    // The books a lectern takes (the item tag lectern_books, ascending), and
    // the pages of a book (its writable or written book content); set by the
    // server, which knows the components' encoding.
    std::vector<std::uint16_t> lectern_books;
    // The books a chiseled bookshelf takes (the item tag bookshelf_books,
    // ascending).
    std::vector<std::uint16_t> bookshelf_books;
    // A beacon's pyramid blocks (the block tag beacon_base_blocks: internal
    // registry blocks, ascending) and payments (the item tag
    // beacon_payment_items, ascending).
    std::vector<std::uint32_t> beacon_base_blocks;
    std::vector<std::uint16_t> beacon_payment_items;
    std::function<std::int32_t(const contracts::ItemStack&)> book_pages;
    // A writable book's components with pages "1" to "n" (fixtures' books).
    std::function<std::optional<contracts::ComponentPatch>(std::int32_t pages)> writable_book;
    // A book and quill's components with its pages set, and those of the
    // written book it becomes when signed (`title` by `author`); set by the
    // server. Without them books are not edited.
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::span<const std::string> pages)> with_book_pages;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::span<const std::string> pages,
                                                           std::string_view title, std::string_view author)>
        signed_book;
    // A shulker box item's contents (its container component, slot by slot
    // up to its last stack), and components with the container set to
    // `stacks` (none: without it); set by the server. Without them a broken
    // shulker box drops its contents beside it.
    std::function<std::vector<contracts::ItemStack>(const contracts::ComponentPatch&)> container_of;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::span<const contracts::ItemStack>)> with_container;
    // A component holding one integer (map_id, map_post_processing): its
    // value, and components with it set, or removed (nullopt); set by the
    // server. Without them empty maps and cartography tables do nothing.
    std::function<std::optional<std::int32_t>(const contracts::ComponentPatch&, std::string_view)> int_component;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::string_view, std::optional<std::int32_t>)>
        with_int_component;
    // For special recipes: a banner's pattern layers (0 without), a stack's
    // dyed color (nullopt without), components with the dyed color set, and
    // components with one named component as another stack's (removed when
    // that has none); set by the server.
    std::function<std::int32_t(const contracts::ComponentPatch&)> banner_layers;
    std::function<std::optional<std::int32_t>(const contracts::ComponentPatch&)> dyed_color_of;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::int32_t)> with_dyed_color;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, const contracts::ComponentPatch& from, std::string_view)>
        with_component_from;
    // For firework recipes: a star's components (its explosion: shape 0
    // small ball, 1 large ball, 2 star, 3 creeper, 4 burst; colors as RGB), a
    // star's components with its fade colors set, and a rocket's flying
    // `flight` with the explosions of `stars`; set by the server.
    std::function<std::optional<contracts::ComponentPatch>(std::int32_t shape, std::span<const std::int32_t> colors, bool trail, bool twinkle)>
        firework_star;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::span<const std::int32_t> fades)> with_fade_colors;
    std::function<std::optional<contracts::ComponentPatch>(std::int32_t flight, std::span<const contracts::ComponentPatch> stars)> firework_rocket;
    // For book cloning: a written book's generation (nullopt without its
    // content) and components with it set; for decorated pots: a pot's
    // components with its four decorations, the back, left, right and front
    // ones, bricks included (the rules leave them out for four bricks, the
    // default); set by the server.
    std::function<std::optional<std::int32_t>(const contracts::ComponentPatch&)> book_generation;
    std::function<std::optional<contracts::ComponentPatch>(const contracts::ComponentPatch&, std::int32_t)> with_book_generation;
    std::function<std::optional<contracts::ComponentPatch>(std::span<const std::uint16_t, 4>)> pot_decorations;
    // A stack's name as a player sees it (in English): its custom name, else
    // its potion's or its item's name; set by the server from the language
    // file of the official jar. Without it the rules name items by their id.
    std::function<std::string(const contracts::ItemStack&)> display_name;
};

// A map's data by its id (null for an id no map has), for map extending:
// without a lookup a map amid paper makes nothing.
struct MapData;
using MapLookup = std::function<const MapData*(std::int32_t)>;

// What a crafting grid makes with the data packs: the result of the first
// matching recipe in id order (recipes::id_before), a special one
// (recipes::Special) or another; empty when nothing matches. A crafter's
// window shows it.
contracts::ItemStack crafted(const GameData& data, const recipes::Grid& grid, const MapLookup& maps = {});

} // namespace circuit::gameplay
