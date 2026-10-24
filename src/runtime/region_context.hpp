// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "world_store.hpp"
#include <circuit/gameplay/context.hpp>
#include <array>
#include <deque>
#include <optional>

namespace circuit::runtime::detail {
using contracts::Phase;
using contracts::TickList;
using contracts::TraceKind;
using contracts::TraceRecord;

// Thrown before an access to a column owned by another region touches it.
struct ForeignAccess {
    ChunkKey chunk;
};

// Executes roots of one region in one phase. Writes go directly to owned
// columns after a copy-on-first-write snapshot, so an aborted attempt can be
// rolled back. Level-wide counters are staged and assigned at commit. A
// region is of one level; the command phase's context (all regions) enters
// the level of each command.
class RegionContext final : public gameplay::RuleContext {
public:
    RegionContext(const contracts::VersionBundle& bundle, WorldStore& world, const RegionMap& regions, RegionId region,
                  Phase phase, DimensionId level, bool record_trace, bool unchecked, const gameplay::GameData* data = nullptr);

    void begin_root(std::uint64_t root, ChunkPos home);
    // Command phase: the level of the next roots, or of the chunk-level
    // records that follow.
    void use_level(DimensionId level) noexcept { enter(level); }
    void rollback();
    [[nodiscard]] Phase phase() const noexcept { return phase_; }

    // Commands are inputs between ticks: they observe the previous tick's time.
    Tick now() const override { return phase_ == Phase::commands ? world_.time - 1 : world_.time; }
    BlockState block(BlockPos pos) override;
    void write_block(BlockPos pos, BlockState state) override;
    std::uint8_t light(BlockPos pos) override;
    std::uint8_t sky_light(BlockPos pos) override;
    Sky sky() override;
    std::int32_t game_rule(std::string_view name) override;
    std::optional<contracts::Generation> chunk_generation(ChunkPos chunk) override;
    bool has_tick(TickList list, BlockPos pos, BlockType block) override;
    void add_tick(TickList list, BlockPos pos, BlockType block, Tick due, std::int8_t priority, std::uint32_t seq) override;
    bool has_block_event(const BlockEvent& event) override;
    std::size_t torch_turn_offs(BlockPos pos, Tick since, bool add) override;
    void add_block_event(const BlockEvent& event, std::uint32_t seq) override;
    const BlockEntity* peek_block_entity(BlockPos pos) override;
    BlockEntity* block_entity(BlockPos pos) override;
    void create_block_entity(BlockPos pos, BlockType kind) override;
    void remove_block_entity(BlockPos pos) override;
    std::vector<EntityId> entities_in_cell(BlockPos cell) override;
    std::vector<EntityId> entities_in_cells(BlockPos low, BlockPos high) override;
    const Entity* peek_entity(EntityId id) override;
    Entity* entity(EntityId id) override;
    EntityId spawn_entity(const Entity& prototype) override;
    void relocate_entity(EntityId id, contracts::Vec3 position) override;
    void remove_entity(EntityId id) override;
    const gameplay::PlayerInventory* peek_player(contracts::PlayerKey player) override;
    gameplay::PlayerInventory& player(contracts::PlayerKey player) override;
    std::uint32_t record(TraceKind kind, BlockPos pos, std::uint64_t a, std::uint64_t b, std::uint64_t c) override;

    // Staged results, consumed by the engine's commit.
    std::map<ChunkKey, ChunkData> snapshots;               // Pre-phase copies of written columns.
    std::vector<TraceRecord> trace;
    std::unordered_map<EntityId, std::optional<ChunkKey>> entity_moves; // nullopt: removed
    std::set<std::pair<ChunkKey, ChunkKey>> interactions;  // (root home, other column) accessed together

    // Block-event phase: the region's FIFO. Local ids are positions in the
    // FIFO; `children[i]` lists ids appended while event i ran.
    struct LocalEvent {
        BlockEvent event;
        std::uint32_t id{};
    };
    std::deque<LocalEvent> fifo;
    std::set<BlockEvent> fifo_pending;
    std::vector<std::vector<std::uint32_t>> children;
    std::uint32_t next_local{};
    void push_local(const BlockEvent& event);

    // Tick phases: this region's drained ticks that have not run yet. They
    // still count as scheduled until they run.
    std::set<TickKey> pending_ticks;

    // Creation ledger: every level-wide counter value this attempt consumes,
    // per counter, keyed by (root, sequence). It keeps the creations of objects
    // removed again before commit, so commit consumes exactly the values the
    // serial order consumes. Rollback discards it with the attempt.
    struct Creation {
        std::uint64_t root{};
        std::uint32_t seq{};
        EntityId entity{}; // Provisional id of a spawned entity.
        DimensionId level{}; // whose per-level counter it consumes
    };
    std::array<std::vector<Creation>, counter_kinds> ledger;

protected:
    gameplay::LevelRandom& level_random() override;
    gameplay::RandomSequence& random_sequence(const std::string& id, const gameplay::SequenceIdHash& hash) override;

private:
    Provisional stage(Counter counter, std::uint32_t seq, EntityId entity = {});
    ChunkData* loaded_in_world(BlockPos pos) const;
    [[nodiscard]] ChunkKey key(ChunkPos c) const noexcept { return {dimension(), c}; }
    ChunkData& owned(ChunkPos c, ChunkData& data);
    ChunkData& writable(ChunkPos c, ChunkData& data);
    // The column of an entity of the current level.
    std::optional<ChunkPos> locate(EntityId id) const;
    [[nodiscard]] std::size_t section_of(std::int32_t y) const {
        return static_cast<std::size_t>((y - world().min_y) >> 4);
    }

    WorldStore& world_;
    const RegionMap& regions_;
    RegionId region_;
    Phase phase_;
    bool record_trace_;
    bool unchecked_;
    std::uint64_t root_{};
    std::uint32_t seq_{};
    std::uint32_t creations_{}; // Block entities and entities created in the current root.
    std::uint32_t provisional_entities_{};
    ChunkPos home_{};
    // Each level's random sequence, and its loot tables' sequences, before
    // this attempt first drew from them.
    std::array<std::optional<gameplay::LevelRandom>, contracts::dimension_count> random_snapshots_;
    std::array<std::optional<std::map<std::string, gameplay::RandomSequence>>, contracts::dimension_count> sequence_snapshots_;
    // Each level's light as this context first read it outside the phases
    // that publish it (WorldStore::light_now).
    std::array<const gameplay::LightField*, contracts::dimension_count> light_seen_{};
    const gameplay::LightField& light_field();
};

} // namespace circuit::runtime::detail
