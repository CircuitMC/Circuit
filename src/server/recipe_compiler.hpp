// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "registries.hpp"
#include <circuit/contracts/version.hpp>
#include <circuit/data/packs.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <cstddef>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

namespace circuit::server {

struct RecipeCompilation {
    std::vector<gameplay::recipes::Recipe> crafting; // the crafting recipes the rules match, by id
    std::vector<gameplay::recipes::Cooking> cooking; // the cooking recipes, by id
    // The stonecutting recipes, by id path and then namespace (the official order).
    std::vector<gameplay::recipes::Stonecutting> stonecutting;
    std::vector<gameplay::recipes::Smithing> smithing; // the smithing recipes, by id
    // The items smithing recipes take as templates, bases and additions, ascending
    // (clients receive them as property sets).
    std::vector<std::uint16_t> smithing_template, smithing_base, smithing_addition;
    // Trim materials by their item and, before 1.21.5, patterns by their template item.
    std::unordered_map<std::uint16_t, std::string> trim_materials, trim_patterns;
    std::map<std::string, std::size_t> valid;        // valid recipes, by type
    std::map<std::string, std::size_t> invalid;      // recipes the target version would skip too, by reason
    std::map<std::string, std::size_t> not_crafted;  // valid crafting recipes the rules do not match yet, by reason
    std::vector<std::string> invalid_ids;            // the first few invalid recipes, with their reasons
};

// Reads and checks every recipe of `packs` (data/<ns>/recipe/*.json) against
// the format of `bundle`'s data pack version (minecraft.wiki "Recipe") and
// its item registry, and compiles the shaped, shapeless and transmute
// crafting recipes. A recipe that fails is skipped, as the target version
// skips it with an error; `invalid` counts them.
RecipeCompilation compile_recipes(const data::DataPacks& packs, const contracts::VersionBundle& bundle,
                                  const std::vector<RegistryEntries>& registries);

} // namespace circuit::server
