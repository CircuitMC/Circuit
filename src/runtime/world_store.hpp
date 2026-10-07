// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

// Chunk-column storage of the runtime. A ChunkData object is mutated only by
// the region that owns its column during a phase, or by the coordinator
// between phases.
namespace circuit::runtime::detail {
using contracts::BlockPos;
using contracts::ChunkPos;
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
    for (const auto b : s.blocks) s.random_ticking += gameplay::reacts_to_random_ticks(b.type()) ? 1 : 0;
}

// Level-wide counters whose values are staged while regions run in parallel.
enum class Counter : std::uint8_t { suborder, event_seq, block_entity_order, entity_id };
inline constexpr std::size_t counter_kinds = 4;

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
    ChunkPos pos;
    contracts::Generation generation;
    ChunkLevel level{};
    // Sections are shared copy-on-write between a chunk and its rollback snapshot.
    std::vector<std::shared_ptr<Section>> sections;
    std::map<BlockPos, BlockEntity> block_entities;
    std::map<EntityId, Entity> entities;
    std::map<TickKey, TickEntry> ticks[2];
    std::vector<EventEntry> events; // Pending block events at this column; deferred while not ticking.
    // Block entities created in this phase; their creation order is assigned at commit.
    std::map<BlockPos, Provisional> new_block_entities;
    // Entities spawned in this phase under provisional ids; real ids are assigned at commit.
    std::map<EntityId, Provisional> new_entities;
};
// Provisional entity ids: high bit, region id, and a per-context counter.
inline constexpr std::uint64_t provisional_entity_bit = 1ULL << 63;

struct WorldStore {
    std::map<ChunkPos, std::unique_ptr<ChunkData>> loaded; // Structure changes only between phases.
    std::map<ChunkPos, std::unique_ptr<ChunkData>> saved;  // Unloaded columns with all their state.
    std::unordered_map<EntityId, ChunkPos> entity_index;   // Updated at commit.
    Tick time{};
    std::uint64_t next_suborder{};
    std::uint64_t next_event_seq{};
    std::uint64_t next_entity_id{1};
    std::uint64_t next_block_entity_order{};
    gameplay::TicketSet tickets;
    std::map<contracts::PlayerKey, gameplay::PlayerInventory> players; // changed in the command phase only

    [[nodiscard]] ChunkData* find_loaded(ChunkPos c) const {
        const auto it = loaded.find(c);
        return it == loaded.end() ? nullptr : it->second.get();
    }
};

using RegionId = std::uint64_t;
inline constexpr RegionId all_regions = 0;

struct RegionMap {
    std::unordered_map<ChunkPos, RegionId> owner; // Every loaded column has exactly one owner.
    std::map<RegionId, std::set<ChunkPos>> members;
    RegionId next_id{1};

    RegionId add_singleton(ChunkPos c) {
        const auto id = next_id++;
        owner[c] = id;
        members[id].insert(c);
        return id;
    }
    void remove(ChunkPos c) {
        const auto it = owner.find(c);
        if (it == owner.end()) return;
        auto& set = members[it->second];
        set.erase(c);
        if (set.empty()) members.erase(it->second);
        owner.erase(it);
    }
    // Moves every member of `from` into `into`.
    void absorb(RegionId into, RegionId from) {
        if (into == from) return;
        auto node = members.extract(from);
        if (node.empty()) return;
        for (const auto c : node.mapped()) owner[c] = into;
        members[into].merge(node.mapped());
    }
};

} // namespace circuit::runtime::detail
