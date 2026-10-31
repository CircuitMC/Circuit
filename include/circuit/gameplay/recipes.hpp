// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
// their own, are listed by kind (Special below) and match in id order with
// the others (client/rocket_order); the one not made yet (map extending)
// is not compiled.
namespace circuit::gameplay::recipes {

// Whether recipe id `a` comes before `b` in the order both official servers
// keep recipes in: by the path, then the namespace (client/ns_crafting,
// client/ns_book; the stonecutter's list). The compiled lists are in this
// order, and a grid makes the first recipe in it that takes the grid.
[[nodiscard]] bool id_before(std::string_view a, std::string_view b) noexcept;

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

// A smithing recipe: a template, a base and an addition (an absent
// template or addition wants an empty slot). A transform makes its result
// with the base's damage and components; a trim gives the base a trim of
// the addition's material in its pattern (or the template item's, for
// packs before 1.21.5).
struct Smithing {
    enum class Kind : std::uint8_t { transform, trim };
    Kind kind{};
    std::string id;
    std::optional<Ingredient> template_item;
    Ingredient base;
    std::optional<Ingredient> addition;
    contracts::ItemStack result; // transform
    std::string pattern;         // trim; empty: the template item's
    bool operator==(const Smithing&) const = default;
};

// The special crafting recipes the rules make (their data pack files name
// only their type; client/special_a, both official servers):
// - repair_item: two stacks of one damageable item, alone: one of it with
//   both durabilities and 5 % of the most (at most whole), and the curses of
//   both, each at its higher level; nothing else of theirs;
// - armor_dye: one dyeable item (the item tag dyeable) and dyes: the item
//   with its color and theirs mixed;
// - banner_duplicate: a banner with patterns and one without, of one color:
//   a copy, the patterned one left in its slot when it is taken;
// - shield_decoration: a shield without patterns and a banner: the shield
//   with the banner's patterns and color (a base color of its own replaced);
// - firework_star (client/fireworks): one gunpowder, dyes and at most one
//   each of a shape item (a fire charge, a gold nugget, a feather or a mob
//   head), a diamond (a trail) and glowstone dust (a twinkle): a star with
//   the dyes' firework colors in grid order;
// - firework_rocket: one paper, one to three gunpowder and any stars: three
//   rockets that fly as long as the gunpowder and hold the stars' explosions
//   in grid order (none from a star without one);
// - firework_star_fade: one star and dyes: the star with the dyes as its
//   fade colors (earlier ones replaced; a star without an explosion gets a
//   small ball without colors);
// - book_cloning (client/special_c): one written book of generation 0 or 1
//   (an original or a copy) and books and quills, with pages or not: as
//   many copies as books and quills, a generation later, the written book
//   left in its slot when they are taken;
// - map_cloning: one filled map and empty maps: that many and one more of
//   the filled map (its data need not exist);
// - tipped_arrow (client/special_d): in a 3 by 3 grid, a lingering potion in
//   the middle and arrows around it: eight tipped arrows with the potion's
//   contents as they are (none without);
// - decorated_pot: in a 3 by 3 grid, bricks or sherds (the item tag
//   decorated_pot_ingredients) in the middle cells of the top row, the
//   sides and the bottom row, nothing else: a pot decorated with them in
//   that order (none when all are bricks, its default).
// In a crafter (client/crafter_special, client/crafter_special2) a kept
// banner or written book is a remainder: it is thrown out after the
// result. Repairing takes damaged and undamaged items alike, and drops
// their other components (a dyed color, say).
struct Special {
    enum class Kind : std::uint8_t {
        repair_item,
        armor_dye,
        banner_duplicate,
        shield_decoration,
        firework_rocket,
        firework_star,
        firework_star_fade,
        book_cloning,
        map_cloning,
        tipped_arrow,
        decorated_pot
    };
    Kind kind{};
    std::string id;
    bool operator==(const Special&) const = default;
};

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
