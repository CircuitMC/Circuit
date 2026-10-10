// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <vector>

// Independent sequential reference executor. It applies every effect
// immediately in the serial phase order, keeps level-wide structures as plain
// global containers, and assigns counters at the moment of use. It shares the
// rule functions with the runtime but none of its storage, ownership, staging
// or commit machinery, and it never invokes a scheduler.
namespace circuit::oracle {

class Oracle {
public:
    // `data`: game data from data packs, as an engine's options give it.
    Oracle(const contracts::VersionBundle& bundle, std::uint64_t seed, std::shared_ptr<const gameplay::GameData> data = {});
    ~Oracle();
    Oracle(const Oracle&) = delete;
    Oracle& operator=(const Oracle&) = delete;

    // Executes one tick. A capacity failure throws and leaves the oracle unusable.
    void advance(const std::vector<contracts::Command>& commands);
    [[nodiscard]] const std::vector<contracts::TraceRecord>& trace() const noexcept { return trace_; }
    [[nodiscard]] gameplay::WorldDump dump() const;
    [[nodiscard]] contracts::Tick time() const noexcept { return time_; }

private:
    class Context;
    friend class Context;
    struct ChunkInfo {
        contracts::Generation generation;
        gameplay::ChunkLevel level{};
    };
    using TickKey = std::pair<contracts::BlockPos, gameplay::BlockType>;

    [[nodiscard]] gameplay::ChunkLevel level(contracts::ChunkPos chunk) const;
    void apply_levels();
    void run_tick_list(contracts::TickList list);
    void run_random_ticks();
    void run_block_events();
    void run_entities();
    void run_block_entities();

    const contracts::VersionBundle& bundle_;
    std::uint64_t seed_;
    std::shared_ptr<const gameplay::GameData> data_;
    contracts::Tick time_{};
    bool failed_{};
    std::map<contracts::ChunkPos, ChunkInfo> chunks_;
    std::map<contracts::BlockPos, gameplay::BlockState> blocks_;
    std::map<contracts::BlockPos, gameplay::BlockEntity> block_entities_;
    std::map<contracts::EntityId, gameplay::Entity> entities_;
    std::vector<gameplay::ScheduledTick> ticks_[2];
    std::set<TickKey> tick_index_[2];
    std::deque<gameplay::BlockEvent> events_;
    std::set<gameplay::BlockEvent> pending_events_;
    gameplay::TicketSet tickets_;
    std::map<contracts::PlayerKey, gameplay::PlayerInventory> players_;
    std::uint64_t next_suborder_{};
    std::uint64_t next_entity_id_{1};
    std::uint64_t next_block_entity_order_{};
    std::vector<contracts::TraceRecord> trace_;
    std::unique_ptr<Context> ctx_;
};

} // namespace circuit::oracle
