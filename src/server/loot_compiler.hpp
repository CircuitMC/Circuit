// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "registries.hpp"
#include <circuit/contracts/version.hpp>
#include <circuit/data/packs.hpp>
#include <circuit/gameplay/loot.hpp>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>

namespace circuit::server {

struct LootCompilation {
    // Per internal registry block without rules of its own: its loot table,
    // when it is in the deterministic subset (gameplay/loot.hpp).
    std::unordered_map<std::uint32_t, gameplay::loot::Table> tables;
    std::size_t blocks{};   // block loot tables of the release's blocks without rules
    std::map<std::string, std::size_t> refused; // why tables did not compile, by reason
};

// Compiles the block loot tables of `packs` (data/<ns>/loot_table/blocks/<block>.json)
// for the blocks without rules of their own in `bundle`'s release. Items must
// be in its item registry. Enchantment ids are those of `registries`; without
// an enchantment registry no tool has any.
LootCompilation compile_block_loot(const data::DataPacks& packs, const contracts::VersionBundle& bundle,
                                   const std::vector<RegistryEntries>& registries);

} // namespace circuit::server
