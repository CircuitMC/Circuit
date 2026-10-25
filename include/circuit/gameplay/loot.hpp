// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/random_sequence.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Block loot tables the rules evaluate, compiled from data packs
// (data/<namespace>/loot_table/blocks). The format follows minecraft.wiki
// "Loot table": pools roll a constant number of times; an entry is an item,
// or a composite (alternatives, group, sequence); conditions test survival of
// an explosion, block state properties, the tool, a random chance, a chance
// by enchantment level (table bonus) and the block at an offset; functions
// set counts (constant, uniform or binomial), limit them and give fortune's
// bonuses. Random numbers come from the table's random sequence
// (random_sequence.hpp), as both official servers draw them (fitted to their
// drops and saved sequences): a chance is a float below it, a uniform count
// an int bounded by its range, a binomial count n floats each below p;
// fortune's uniform bonus draws an int even at level 0, its ore bonus only
// above it. Weighted choices between entries are not fitted (no block table
// has them): a roll that needs one yields nothing.
namespace circuit::gameplay::loot {

// A count: constant, uniform between two bounds (as integers), or binomial.
struct Number {
    enum class Kind : std::uint8_t { constant, uniform, binomial };
    Kind kind{};
    double value{};       // constant
    double min{}, max{};  // uniform
    long n{};             // binomial
    double p{};
};

struct Condition {
    // breaker: an entity broke the block (entity_properties of `this` with
    // no predicate).
    enum class Kind : std::uint8_t { survives_explosion, block_state, tool, any_of, all_of, inverted, random_chance, location, table_bonus, breaker };
    Kind kind{};
    double chance{};                       // random_chance
    std::uint32_t bonus_enchantment{};     // table_bonus: the enchantment whose level picks the chance
    std::vector<double> chances{};         // table_bonus: by level, the last for higher levels
    std::int32_t dx{}, dy{}, dz{};         // location: the offset of the block tested
    std::uint32_t block{UINT32_MAX};       // location: the internal registry block it must be (UINT32_MAX: any)
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
    enum class Kind : std::uint8_t { set_count, limit_count, explosion_decay, ore_bonus, uniform_bonus, binomial_bonus };
    Kind kind{};
    Number count{};  // set_count
    bool add{};      // set_count adds instead of setting
    long multiplier{};   // uniform_bonus
    long extra{};        // binomial_bonus
    double probability{}; // binomial_bonus
    std::optional<long> min{}, max{}; // limit_count
    std::uint32_t enchantment{};      // the bonuses: the enchantment whose level gives them (fortune)
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
    long rolls{1};
};

struct Table {
    std::vector<Pool> pools{};
    std::vector<Function> functions{};
    // The random sequence its rolls draw from and the MD5 of its id (block
    // tables name one; without one, rolls that draw yield nothing).
    std::string sequence;
    SequenceIdHash sequence_hash{};
};

// What a player's breaking of a block gives the conditions to test: the
// block's properties and the tool, with its enchantment levels.
struct BreakContext {
    std::vector<std::pair<std::string, std::string>> properties;
    contracts::ItemStack tool;
    // Level of an enchantment on the tool (0 when absent).
    std::function<long(std::uint32_t enchantment)> enchantment_level;
    // Where the rolls draw from; without one, a roll that draws yields nothing.
    RandomSequence* random{};
    // Whether the block at an offset is the block a location condition names
    // with its properties; without it, location conditions fail.
    std::function<bool(const Condition&)> location;
    // Whether an entity broke it (a player, an explosion's primed TNT; not an update).
    bool by_entity{};
};

// The drops, or nullopt when a roll would need a random number it has no
// source for, or a weighted choice. A stack may be empty (a count of 0): it still drops, as the
// official servers drop it, and makes no item.
std::optional<std::vector<contracts::ItemStack>> drops(const Table& table, const BreakContext& context);

} // namespace circuit::gameplay::loot
