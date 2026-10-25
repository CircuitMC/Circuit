// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/level_random.hpp>
#include <circuit/gameplay/light.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <array>
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
    // `level_random_seed`: as EngineOptions::level_random_seed.
    Oracle(const contracts::VersionBundle& bundle, std::uint64_t seed, std::shared_ptr<const gameplay::GameData> data = {},
           std::optional<std::int64_t> level_random_seed = {});
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
        gameplay::ChunkLevels levels;
        contracts::Tick saved_at{}; // unloaded: the tick from which its pending ticks wait
        contracts::Tick resumes{};  // loaded: scheduled ticks and block entities run from this tick
    };
    using TickKey = std::pair<contracts::BlockPos, gameplay::BlockType>;
    // One level's state: plain global containers, as the serial order sees them.
    struct Level {
        std::map<contracts::ChunkPos, ChunkInfo> chunks;
        std::map<contracts::BlockPos, gameplay::BlockState> blocks;
        std::map<contracts::BlockPos, gameplay::BlockEntity> block_entities;
        std::map<contracts::EntityId, gameplay::Entity> entities;
        std::vector<gameplay::ScheduledTick> ticks[2];
        std::set<TickKey> tick_index[2];
        std::deque<gameplay::BlockEvent> events;
        std::set<gameplay::BlockEvent> pending_events;
        std::map<contracts::ChunkPos, gameplay::TurnOffs> turn_offs; // by column, as the runtime keeps them
        std::uint64_t next_suborder{};
        std::uint64_t next_block_entity_order{};
        std::map<contracts::PlayerKey, gameplay::Presence> presences;
        contracts::Tick last_active{};
        gameplay::LevelRandom random;
        std::uint32_t random_tick{};
        std::map<std::string, gameplay::RandomSequence> sequences;
        std::uint64_t light_version{}; // changes whenever something light depends on changes
        [[nodiscard]] gameplay::ChunkLevel level(contracts::ChunkPos chunk) const;
        // The chunk's simulation level; none when unloaded.
        [[nodiscard]] std::uint8_t simulation(contracts::ChunkPos chunk) const;
        // Whether a reloaded chunk runs its scheduled ticks and block entities again.
        [[nodiscard]] bool resumed(contracts::ChunkPos chunk, contracts::Tick now) const;
    };

    void apply_levels(contracts::DimensionId level);
    // Light as a tick sees it (gameplay::RuleContext::light), computed from
    // scratch from the blocks at the start of the tick.
    // The most of the block and sky light, or `only` one of them.
    std::uint8_t light(contracts::DimensionId level, contracts::BlockPos pos, std::optional<gameplay::LightKind> only = std::nullopt);
    struct LightSnapshot;
    void run_tick_list(contracts::DimensionId level, contracts::TickList list);
    void run_random_ticks(contracts::DimensionId level);
    void run_block_events(contracts::DimensionId level);
    void run_entities(contracts::DimensionId level);
    void run_block_entities(contracts::DimensionId level);
    Level& at(contracts::DimensionId level) { return levels_[static_cast<std::size_t>(level)]; }

    const contracts::VersionBundle& bundle_;
    std::uint64_t seed_;
    std::shared_ptr<const gameplay::GameData> data_;
    contracts::Tick time_{};
    contracts::Tick started_{};
    std::map<std::string, std::int32_t> game_rules_;
    std::int64_t day_time_{};
    std::uint8_t difficulty_{};
    gameplay::Weather weather_;
    std::uint8_t sky_darken_{}; // as the overworld's last tick computed it
    bool failed_{};
    std::array<Level, contracts::dimension_count> levels_;
    gameplay::TicketSet tickets_;
    std::map<contracts::PlayerKey, gameplay::PlayerInventory> players_;
    std::uint64_t next_entity_id_{1};
    std::vector<contracts::TraceRecord> trace_;
    std::array<std::unique_ptr<LightSnapshot>, contracts::dimension_count> light_;
    std::unique_ptr<Context> ctx_;
};

} // namespace circuit::oracle
