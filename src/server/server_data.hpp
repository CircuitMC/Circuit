// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "loot_compiler.hpp"
#include "recipe_compiler.hpp"
#include "registries.hpp"
#include <circuit/contracts/version.hpp>
#include <circuit/data/packs.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/protocol/components.hpp>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// What a server takes from the data packs its operator configures: the
// synchronized registries and tags clients receive, the data the rules use,
// and what item components may refer to. Without data packs it is the
// bundle's public tables alone.
namespace circuit::server {

struct ServerData {
    std::optional<data::DataPacks> packs;
    std::vector<RegistryEntries> registries;          // sent by name to clients with the vanilla pack
    std::vector<NumberedTags> tags;                   // sent with them
    std::shared_ptr<const gameplay::GameData> game;   // for the rules; null without data packs
    protocol::ComponentContext components;            // what item components may refer to
    // What the rules took from the loot tables and recipes (the tables and
    // recipes themselves are in `game`), and why they left the rest.
    LootCompilation loot;
    RecipeCompilation recipes;
    [[nodiscard]] const std::string& fingerprint() const;
};

// Loads the data packs `vanilla_jar` and `packs` name (none: the public
// tables only) for `bundle`.
std::expected<ServerData, std::string> load_server_data(const contracts::VersionBundle& bundle, const data::PackOptions& options);

// One line per pack, the fingerprint, tag counts per registry, resource
// counts and notes: what `circuit-server --data-report` prints.
std::string describe(const ServerData& data);

} // namespace circuit::server
