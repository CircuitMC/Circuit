// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/block.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Block loot tables in the deterministic subset the rules evaluate, compiled
// from data packs (data/<namespace>/loot_table/blocks). The format follows
// minecraft.wiki "Loot table": pools roll once; an entry is an item, or a
// composite (alternatives, group, sequence); conditions test survival of an
// explosion, block state properties and the tool; functions set and limit
// counts. Parts that draw random numbers (weighted choices between entries,
// random counts, chances, fortune) wait for the target version's random
// streams: a table that needs one does not compile, and a roll that would
// need one yields nothing.
namespace circuit::gameplay::loot {

struct Condition {
    enum class Kind : std::uint8_t { survives_explosion, block_state, tool, any_of, all_of, inverted };
    Kind kind{};
    // block_state: property -> exact value, or an integer range.
    struct Property {
        std::string name;
        std::optional<std::string> exact{};
        std::optional<long> min{}, max{};
    };
    std::vector<Property> properties{};
    bool other_block{}; // block_state: the condition names another block, so it never holds
    // tool: the items it may be (empty: any), and enchantment levels it must have.
    std::vector<std::uint16_t> items{};
    struct Enchantment {
        std::uint32_t id{};  // in the enchantment registry the server synchronizes
        long min{1}, max{255};
    };
    std::vector<Enchantment> enchantments{};
    std::vector<Condition> children{}; // any_of, all_of; inverted: one
};

struct Function {
    enum class Kind : std::uint8_t { set_count, limit_count, explosion_decay, ore_bonus };
    Kind kind{};
    long count{};    // set_count
    bool add{};      // set_count adds instead of setting
    std::optional<long> min{}, max{}; // limit_count
    std::uint32_t enchantment{};      // ore_bonus: the enchantment whose level multiplies drops (fortune)
    std::vector<Condition> conditions{};
};

struct Entry {
    enum class Kind : std::uint8_t { item, alternatives, group, sequence, empty };
    Kind kind{};
    std::uint16_t item{};
    std::vector<Condition> conditions{};
    std::vector<Function> functions{};
    std::vector<Entry> children{};
};

struct Pool {
    std::vector<Entry> entries{};
    std::vector<Condition> conditions{};
    std::vector<Function> functions{};
};

struct Table {
    std::vector<Pool> pools{};
    std::vector<Function> functions{};
};

// What a player's breaking of a block gives the conditions to test: the
// block's properties and the tool, with its enchantment levels.
struct BreakContext {
    std::vector<std::pair<std::string, std::string>> properties;
    contracts::ItemStack tool;
    // Level of an enchantment on the tool (0 when absent).
    std::function<long(std::uint32_t enchantment)> enchantment_level;
};

// The drops, or nullopt when a roll would need a random number.
std::optional<std::vector<contracts::ItemStack>> drops(const Table& table, const BreakContext& context);

} // namespace circuit::gameplay::loot
