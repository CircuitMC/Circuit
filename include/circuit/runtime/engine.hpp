// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/errors.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// The engine facade: what everything above the simulation sees of it. The
// strict single-threaded runtime implements it, and so does an adapter over
// the oracle (tests/oracle/oracle_engine.hpp), which is its specification: in
// strict mode an implementation produces the oracle's traces, dumps and change
// feeds, tick for tick.
namespace circuit::runtime {

struct EngineOptions {
    // The data packs' data the rules run with; none by default.
    std::shared_ptr<const gameplay::GameData> data;
    // The seed of every level's random and of its random-tick position state;
    // empty: both derived from the world seed. The reference harness seeds the
    // overworld's with 0.
    std::optional<std::int64_t> level_random_seed;
    // The world seed random sequences start from; empty: the world seed. The
    // reference harness's worlds have the seed 0.
    std::optional<std::int64_t> sequence_seed;
};

// What the last tick changed, for the player view and for storage. Every list
// is in ascending (level, position or id) order and names each thing once.
struct ChangeFeed {
    struct Block {
        contracts::DimensionId level{};
        contracts::BlockPos pos;
        gameplay::BlockState state; // the state at the end of the tick
        bool operator==(const Block&) const = default;
    };
    std::vector<Block> blocks; // positions whose state at the end of the tick differs from the start
    std::vector<std::pair<contracts::DimensionId, contracts::BlockPos>> block_entities; // created, removed or changed
    std::vector<std::pair<contracts::DimensionId, contracts::EntityId>> entities;       // added, removed or changed
    std::vector<std::pair<contracts::DimensionId, contracts::ChunkPos>> chunks;         // whose chunk record changed
    // The world's own state changed: day time, weather, difficulty, game
    // rules, tickets, player inventories or maps.
    bool world{};
    bool operator==(const ChangeFeed&) const = default;
};

class Engine {
public:
    virtual ~Engine() = default;
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Executes one logical tick with the commands admitted for it. A failure
    // (an exceeded capacity) throws and leaves the engine unusable; logical
    // ticks are never skipped or repeated.
    virtual void advance(const std::vector<contracts::Command>& commands) = 0;
    // The last tick run (0 before the first).
    [[nodiscard]] virtual contracts::Tick time() const = 0;
    // The trace of the last tick.
    [[nodiscard]] virtual const std::vector<contracts::TraceRecord>& trace() const = 0;
    // The canonical state at the end of the last tick; an engine restored
    // from it continues exactly as this one would.
    [[nodiscard]] virtual gameplay::WorldDump dump() const = 0;
    // What the last tick changed.
    [[nodiscard]] virtual const ChangeFeed& changes() const = 0;

    // Reads of the state at the end of the last tick, in a level.
    // out_of_bounds outside the world height, unloaded_chunk where no column
    // is loaded.
    [[nodiscard]] virtual contracts::Result<gameplay::BlockState> block(contracts::DimensionId level, contracts::BlockPos pos) const = 0;
    // A block entity, or nullopt where the position has none.
    [[nodiscard]] virtual contracts::Result<std::optional<gameplay::BlockEntity>> block_entity(contracts::DimensionId level,
                                                                                               contracts::BlockPos pos) const = 0;
    // An entity of the level; nullopt when it is in no level or another one.
    [[nodiscard]] virtual std::optional<gameplay::Entity> entity(contracts::DimensionId level, contracts::EntityId id) const = 0;
    // Block light and sky light at a position as the next tick sees them:
    // computed from the blocks at the end of the tick before the last one,
    // as the rules read light during a tick. 0 and 0 where no column is loaded.
    [[nodiscard]] virtual std::pair<std::uint8_t, std::uint8_t> light(contracts::DimensionId level, contracts::BlockPos pos) const = 0;
};

// Makes an engine for a bundle and world seed: a new world, or one that
// continues from `state` when it is given. A bundle without executable rules
// and a state the rules never produce throw std::invalid_argument.
using EngineFactory = std::function<std::unique_ptr<Engine>(const contracts::VersionBundle& bundle, std::uint64_t seed,
                                                            const EngineOptions& options, const gameplay::WorldDump* state)>;

// The change feed between two canonical dumps of consecutive ticks: what an
// implementation's changes() must equal. Implementations keep their own
// bookkeeping; this is the definition they are tested against.
ChangeFeed changes_between(const gameplay::WorldDump& before, const gameplay::WorldDump& after);

} // namespace circuit::runtime
