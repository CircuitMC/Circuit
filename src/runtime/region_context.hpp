// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "world_store.hpp"
#include <circuit/gameplay/context.hpp>
#include <deque>
#include <optional>

namespace circuit::runtime::detail {
using contracts::Phase;
using contracts::TickList;
using contracts::TraceKind;
using contracts::TraceRecord;

// Thrown before an access to a column owned by another region touches it.
struct ForeignAccess {
    ChunkPos chunk;
};

// Executes roots of one region in one phase. Writes go directly to owned
// columns after a copy-on-first-write snapshot, so an aborted attempt can be
// rolled back. Level-wide counters are staged and assigned at commit.
class RegionContext final : public gameplay::RuleContext {
public:
    RegionContext(const contracts::VersionBundle& bundle, WorldStore& world, const RegionMap& regions, RegionId region,
                  Phase phase, bool record_trace, bool unchecked);

    void begin_root(std::uint64_t root, ChunkPos home);
    void rollback();
    [[nodiscard]] Phase phase() const noexcept { return phase_; }

    Tick now() const override { return world_.time; }
    BlockState block(BlockPos pos) override;
    void write_block(BlockPos pos, BlockState state) override;
    std::optional<contracts::Generation> chunk_generation(ChunkPos chunk) override;
    bool has_tick(TickList list, BlockPos pos, BlockType block) override;
    void add_tick(TickList list, BlockPos pos, BlockType block, Tick due, std::int8_t priority, std::uint32_t seq) override;
    bool has_block_event(const BlockEvent& event) override;
    void add_block_event(const BlockEvent& event, std::uint32_t seq) override;
    const BlockEntity* peek_block_entity(BlockPos pos) override;
    BlockEntity* block_entity(BlockPos pos) override;
    void create_block_entity(BlockPos pos, BlockType kind) override;
    void remove_block_entity(BlockPos pos) override;
    std::vector<EntityId> entities_in_cell(BlockPos cell) override;
    const Entity* peek_entity(EntityId id) override;
    Entity* entity(EntityId id) override;
    EntityId spawn_entity(const Entity& prototype) override;
    void relocate_entity(EntityId id, contracts::FixedVec position) override;
    void remove_entity(EntityId id) override;
    std::uint32_t record(TraceKind kind, BlockPos pos, std::uint64_t a, std::uint64_t b, std::uint64_t c) override;

    // Staged results, consumed by the engine's commit.
    std::map<ChunkPos, ChunkData> snapshots;                // Pre-phase copies of written columns.
    std::vector<TraceRecord> trace;
    std::unordered_map<EntityId, std::optional<ChunkPos>> entity_moves; // nullopt: removed
    std::set<std::pair<ChunkPos, ChunkPos>> interactions;  // (root home, other column) accessed together

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

private:
    ChunkData* loaded_in_world(BlockPos pos) const;
    ChunkData& owned(ChunkPos c, ChunkData& data);
    ChunkData& writable(ChunkPos c, ChunkData& data);
    std::optional<ChunkPos> locate(EntityId id) const;
    [[nodiscard]] std::size_t section_of(std::int32_t y) const {
        return static_cast<std::size_t>((y - bundle().world.min_y) >> 4);
    }

    WorldStore& world_;
    const RegionMap& regions_;
    RegionId region_;
    Phase phase_;
    bool record_trace_;
    bool unchecked_;
    std::uint64_t root_{};
    std::uint32_t seq_{};
    std::uint32_t creations_{}; // Block entities created in the current root.
    ChunkPos home_{};
};

} // namespace circuit::runtime::detail
