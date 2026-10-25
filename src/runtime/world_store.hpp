// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/light.hpp>
#include <functional>
#include <circuit/gameplay/level_random.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Chunk-column storage of the runtime. A ChunkData object is mutated only by
// the region that owns its column during a phase, or by the coordinator
// between phases.
namespace circuit::runtime::detail {
using contracts::BlockPos;
using contracts::ChunkKey;
using contracts::ChunkPos;
using contracts::DimensionId;
using contracts::EntityId;
using contracts::Tick;
using gameplay::BlockEntity;
using gameplay::BlockEvent;
using gameplay::BlockState;
using gameplay::BlockType;
using gameplay::ChunkLevel;
using gameplay::Entity;

struct Section {
    std::array<BlockState, 4096> blocks{};
    // Blocks that react to random ticks; sections without any are skipped in
    // the random-tick phase (their per-section random stream is independent).
    std::uint16_t random_ticking{};
};
// Counts the randomly ticking blocks of a section filled without write_block.
inline void count_random_ticking(Section& s) {
    s.random_ticking = 0;
    for (const auto b : s.blocks) s.random_ticking += gameplay::takes_random_ticks(b) ? 1 : 0;
}

// Level-wide counters whose values are staged while regions run in parallel.
// The first three count per level; entity ids count over all levels.
enum class Counter : std::uint8_t { suborder, event_seq, block_entity_order, entity_id };
inline constexpr std::size_t counter_kinds = 4;

struct LevelCounters {
    std::uint64_t next_suborder{};
    std::uint64_t next_event_seq{};
    std::uint64_t next_block_entity_order{};
    // Players in the level, and the last tick it had players or forced chunks.
    std::map<contracts::PlayerKey, gameplay::Presence> presences;
    Tick last_active{};
    gameplay::LevelRandom random; // see LevelDump::random
    std::uint32_t random_tick{};  // see LevelDump::random_tick
    std::map<std::string, gameplay::RandomSequence> sequences; // see LevelDump::sequences
};

// The pseudo column that stands for a level's random sequence in the region
// map: the region that owns it may draw, so roots that draw run in one
// region, in root order. No real column has its position (outside the border).
inline ChunkKey random_key(DimensionId level) noexcept { return {level, {INT32_MIN, INT32_MIN}}; }

// Staging metadata: an entry created in the current phase. `ledger` indexes the
// creation in its region context's ledger for the entry's counter; commit
// assigns the value.
struct Provisional {
    bool pending{};
    std::uint32_t ledger{};
};

struct TickEntry {
    Tick due{};
    std::int8_t priority{};
    std::uint64_t suborder{};
    Provisional provisional;
};
using TickKey = std::pair<BlockPos, BlockType>;

struct EventEntry {
    BlockEvent event;
    std::uint64_t seq{};
    Provisional provisional;
};

struct ChunkData {
    DimensionId dimension{};
    ChunkPos pos;
    contracts::Generation generation;
    ChunkLevel level{};             // status, from levels.load
    gameplay::ChunkLevels levels;   // load and simulation levels
    Tick saved_at{};                // saved: the tick from which its pending ticks wait (gameplay::reload_shift)
    Tick resumes{};                 // loaded: its scheduled ticks and block entities run from this tick
    [[nodiscard]] bool block_ticking() const noexcept { return gameplay::block_ticking(levels.simulation); }
    [[nodiscard]] bool entity_ticking() const noexcept { return gameplay::entity_ticking(levels.simulation); }
    // Sections are shared copy-on-write between a chunk and its rollback snapshot.
    std::vector<std::shared_ptr<Section>> sections;
    std::map<BlockPos, BlockEntity> block_entities;
    std::map<EntityId, Entity> entities;
    // Entities saved with the column that come back after a reload
    // (Entity::returns), by the ids they had.
    std::map<EntityId, Entity> returning;
    std::map<TickKey, TickEntry> ticks[2];
    std::vector<EventEntry> events; // Pending block events at this column; deferred while not ticking.
    gameplay::TurnOffs turn_offs;   // Redstone torch turn-offs (RuleContext::torch_turn_offs).
    // Block entities created in this phase; their creation order is assigned at commit.
    std::map<BlockPos, Provisional> new_block_entities;
    // Entities spawned in this phase under provisional ids; real ids are assigned at commit.
    std::map<EntityId, Provisional> new_entities;
};
// Provisional entity ids: high bit, region id, and a per-context counter.
inline constexpr std::uint64_t provisional_entity_bit = 1ULL << 63;

struct WorldStore {
    // Columns of every level, ordered by level first. Structure changes only between phases.
    std::map<ChunkKey, std::unique_ptr<ChunkData>> loaded;
    std::map<ChunkKey, std::unique_ptr<ChunkData>> saved; // Unloaded columns with all their state.
    std::unordered_map<EntityId, ChunkKey> entity_index;  // Updated at commit.
    Tick time{};
    Tick started{}; // see WorldDump::started
    std::map<std::string, std::int32_t> game_rules; // see WorldDump::game_rules
    std::int64_t day_time{};                        // see WorldDump::day_time
    std::uint8_t difficulty{};                      // see WorldDump::difficulty
    gameplay::Weather weather;                      // see WorldDump::weather
    gameplay::SequenceSettings sequence_settings;   // see WorldDump::sequence_settings
    std::int64_t sequence_seed{};                   // the world seed new random sequences start from
    // The darkening of the overworld's sky as its last tick computed it
    // (gameplay::sky_darken); derived from the weather and day time on a restore.
    std::uint8_t sky_darken{};
    std::uint64_t next_entity_id{1};
    std::array<LevelCounters, contracts::dimension_count> levels{};
    // Each level's light while the command, random-tick and block-entity
    // phases read it (up to date for the tick); null otherwise, when rules of
    // the other phases get it from `light_now`, which brings it up to date for
    // the tick (once, serially: it is computed from the blocks at the start
    // of the tick) and returns it.
    std::array<const gameplay::LightField*, contracts::dimension_count> light{};
    std::function<const gameplay::LightField*(contracts::DimensionId)> light_now;
    gameplay::TicketSet tickets;
    std::map<contracts::PlayerKey, gameplay::PlayerInventory> players; // changed in the command phase only

    [[nodiscard]] ChunkData* find_loaded(ChunkKey c) const {
        const auto it = loaded.find(c);
        return it == loaded.end() ? nullptr : it->second.get();
    }
    [[nodiscard]] LevelCounters& counters(DimensionId d) { return levels[static_cast<std::size_t>(d)]; }
    [[nodiscard]] const LevelCounters& counters(DimensionId d) const { return levels[static_cast<std::size_t>(d)]; }
    // The loaded columns of one level, in position order.
    template <class F>
    void for_each_loaded(DimensionId d, F&& f) const {
        for (auto it = loaded.lower_bound({d, {INT32_MIN, INT32_MIN}}); it != loaded.end() && it->first.dimension == d; ++it)
            f(it->first.pos, *it->second);
    }
};

using RegionId = std::uint64_t;
inline constexpr RegionId all_regions = 0;

// Regions partition the loaded columns; a region's columns are all of one
// level, since rules never reach into another level.
struct RegionMap {
    std::unordered_map<ChunkKey, RegionId> owner; // Every loaded column has exactly one owner.
    std::map<RegionId, std::set<ChunkKey>> members;
    RegionId next_id{1};

    RegionId add_singleton(ChunkKey c) {
        const auto id = next_id++;
        owner[c] = id;
        members[id].insert(c);
        return id;
    }
    void remove(ChunkKey c) {
        const auto it = owner.find(c);
        if (it == owner.end()) return;
        auto& set = members[it->second];
        set.erase(c);
        if (set.empty()) members.erase(it->second);
        owner.erase(it);
    }
    // Moves every member of `from` into `into`. Precondition: both regions
    // are of the same level.
    void absorb(RegionId into, RegionId from) {
        if (into == from) return;
        auto node = members.extract(from);
        if (node.empty()) return;
        for (const auto c : node.mapped()) owner[c] = into;
        members[into].merge(node.mapped());
    }
};

} // namespace circuit::runtime::detail
