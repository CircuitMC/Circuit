// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "registries.hpp"
#include <circuit/contracts/ids.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/data/packs.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The recipe book as both official servers keep it (client/recipes): the
// recipes of the data packs as clients show them, the advancements that
// unlock them, and each player's book.
namespace circuit::server {

// How a client shows a slot of a recipe (SlotDisplay of both protocols).
struct SlotDisplay {
    enum class Kind : std::uint8_t { empty = 0, any_fuel = 1, item = 2, item_stack = 3, tag = 4, smithing_trim = 5, with_remainder = 6, composite = 7 };
    Kind kind{};
    std::uint16_t item{};             // item
    contracts::ItemStack stack{};     // item_stack
    std::string tag{};                // tag ("minecraft:planks")
    std::int32_t pattern{-1};         // smithing_trim: the trim pattern's registry index; -1: its template's display
    // composite; smithing_trim: base, material (and the template's display);
    // with_remainder: the item and what it leaves in the grid.
    std::vector<SlotDisplay> parts{};
};
// Items as requirements name them: a tag, or items.
struct ItemSet {
    std::string tag;
    std::vector<std::uint16_t> items;
};
// How a client shows a recipe (RecipeDisplay).
struct RecipeDisplay {
    enum class Kind : std::uint8_t { shapeless = 0, shaped = 1, furnace = 2, stonecutter = 3, smithing = 4 };
    Kind kind{};
    std::uint8_t width{}, height{};
    // shapeless and shaped: the ingredients; furnace and stonecutter: the
    // input; smithing: the template, base and addition.
    std::vector<SlotDisplay> slots;
    SlotDisplay result, station;
    std::int32_t duration{}; // furnace: cooking ticks
    float experience{};
};

// A recipe with a display. Its display id is its place in `RecipeBook::recipes`.
struct BookRecipe {
    std::string id;
    bool in_book{true};      // false: shown nowhere, but numbered (map extending)
    bool notification{true}; // show_notification
    std::int32_t group{};    // as sent: 0 none, else 1 + the group's number
    std::int32_t category{}; // recipe_book_category
    RecipeDisplay display;
    std::vector<ItemSet> requirements;
};

// A criterion of an advancement that rewards recipes, as the rules trigger it.
struct BookCriterion {
    enum class Kind : std::uint8_t { inventory_changed, slots_occupied, recipe_unlocked, tick, in_water, never };
    std::string name;
    Kind kind{Kind::never};
    std::vector<std::uint16_t> items{}; // inventory_changed: an item that changed is one of these
    std::int32_t occupied{};          // slots_occupied: at least this many of the inventory's slots
    std::size_t recipe{};             // recipe_unlocked
};
struct BookAdvancement {
    std::string id;
    std::vector<BookCriterion> criteria{};
    std::vector<std::vector<std::size_t>> requirements{}; // each needs one of its criteria
    std::vector<std::size_t> rewards{};                   // recipes
};
using CriterionRef = std::pair<std::size_t, std::size_t>; // (advancement, criterion)

struct RecipeBook {
    std::vector<BookRecipe> recipes;                         // by display id
    std::map<std::string, std::size_t, std::less<>> by_id;   // recipes by id
    std::vector<BookAdvancement> advancements;               // by id
    std::map<std::string, std::size_t, std::less<>> advancement_ids;
    std::map<std::uint16_t, std::vector<CriterionRef>> by_item;
    std::vector<std::vector<CriterionRef>> by_recipe;        // recipe_unlocked criteria per recipe
    std::vector<CriterionRef> on_tick, in_water, occupied;
    std::size_t unsupported{}; // criteria the rules never trigger (other triggers and conditions)
    std::size_t all{};         // every recipe of the packs, special ones too (/recipe ... * names them)
};

// Reads the recipes and the advancements that reward recipes of `packs`.
// Recipe ids sorted give the display ids; special recipes have none but
// map extending, which no book shows. Groups count by their first recipe.
RecipeBook compile_recipe_book(const data::DataPacks& packs, const contracts::VersionBundle& bundle,
                               const std::vector<RegistryEntries>& registries);

// A player's recipe book: the recipes they know, those still highlighted,
// the four books' settings (open, filtering: crafting, furnace, blast
// furnace, smoker), and the advancements done or under way.
struct PlayerBook {
    std::set<std::size_t> known, highlighted;
    std::array<bool, 8> settings{};
    std::set<std::size_t> done;
    std::map<std::size_t, std::set<std::size_t>> progress;
    bool operator==(const PlayerBook&) const = default;
};

// What an event unlocks: for each advancement it completes (in id order) or
// each award, the recipes new to the player, each list one packet. New
// recipes trigger recipe_unlocked in turn.
using Unlocks = std::vector<std::vector<std::size_t>>;
Unlocks award(const RecipeBook& book, PlayerBook& player, std::span<const std::size_t> recipes);
Unlocks trigger(const RecipeBook& book, PlayerBook& player, std::vector<CriterionRef> criteria);
// The criteria items changed in the inventory meet, and those of how many slots it fills.
std::vector<CriterionRef> inventory_criteria(const RecipeBook& book, const PlayerBook& player, std::span<const std::uint16_t> changed,
                                             std::int32_t occupied);
// Forgets recipes; returns those the player knew, ascending.
std::vector<std::size_t> forget(PlayerBook& player, std::span<const std::size_t> recipes);

} // namespace circuit::server
