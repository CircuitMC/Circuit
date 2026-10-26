// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Crafting recipes the rules match, compiled from data packs
// (data/<namespace>/recipe). The format follows minecraft.wiki "Recipe":
// - a shaped recipe matches its pattern anywhere in the grid, also mirrored
//   left to right;
// - a shapeless recipe matches its ingredients in any order;
// - a transmute recipe turns an input and a material into its result, which
//   keeps the input's components; an input that already is the result item
//   does not match.
// Ingredients test the item alone. The special recipes, which need code of
// their own (repairing, map and book cloning, fireworks, dyeing armor), are
// not compiled.
namespace circuit::gameplay::recipes {

// The items an ingredient accepts, ascending; none for an empty pattern cell.
struct Ingredient {
    std::vector<std::uint16_t> items;
    [[nodiscard]] bool empty() const noexcept { return items.empty(); }
    [[nodiscard]] bool test(std::uint16_t item) const noexcept;
    bool operator==(const Ingredient&) const = default;
};

struct Recipe {
    enum class Kind : std::uint8_t { shaped, shapeless, transmute };
    Kind kind{};
    std::string id; // "minecraft:oak_planks"
    // shaped: the pattern without empty outer rows and columns, row by row;
    // shapeless: the ingredients; transmute: the input, then the material.
    std::uint8_t width{}, height{};
    std::vector<Ingredient> ingredients;
    contracts::ItemStack result;
};

// A cooking recipe: smelting (furnaces), blasting (blast furnaces), smoking
// (smokers) or campfire cooking, with its cooking time in ticks (the type's
// default when the recipe gives none: 200 for smelting, 100 otherwise).
struct Cooking {
    enum class Kind : std::uint8_t { smelting, blasting, smoking, campfire };
    Kind kind{};
    std::string id;
    Ingredient ingredient;
    contracts::ItemStack result;
    float experience{};
    std::int32_t time{};
    bool operator==(const Cooking&) const = default;
};

// A stonecutting recipe: one of the ingredient becomes the result.
struct Stonecutting {
    std::string id;
    Ingredient ingredient;
    contracts::ItemStack result;
    bool operator==(const Stonecutting&) const = default;
};

// The stonecutting recipes an item can be cut by, in the order the list is
// given (the stonecutter's buttons).
std::vector<const Stonecutting*> cuttings(std::span<const Stonecutting> recipes, std::uint16_t item);

// A crafting grid's slots, row by row.
struct Grid {
    std::uint8_t width{}, height{};
    std::span<const contracts::ItemStack> slots;
};

struct Match {
    const Recipe* recipe{};
    contracts::ItemStack result;
};
// The first of `recipes` that matches the grid, and the stack it makes.
std::optional<Match> match(std::span<const Recipe> recipes, const Grid& grid);

// What a grid slot keeps when one of `item` is used up: buckets of milk,
// water and lava leave a bucket, honey bottles and dragon's breath a glass
// bottle (minecraft.wiki "Crafting"); other items leave nothing.
contracts::ItemStack remainder(std::uint16_t item);

} // namespace circuit::gameplay::recipes
