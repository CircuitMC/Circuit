// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <optional>
#include <vector>

namespace circuit::gameplay {
using contracts::TraceKind;

// Per-root state of immediate neighbor-update propagation. Version rule:
// deliveries run depth-first; sources emitted while one delivery runs are
// processed in emission order before the next sibling delivery.
struct NeighborUpdater {
    struct Entry {
        BlockPos source;
        std::uint8_t next{};
    };
    std::vector<Entry> stack;
    std::vector<BlockPos> layer;
    bool running{};
    std::uint64_t delivered{};
    void reset() noexcept {
        stack.clear();
        layer.clear();
        running = false;
        delivered = 0;
    }
};

// Everything a rule may do to the world. An executor implements it over its
// own storage and ordering machinery: the sequential oracle applies effects
// immediately; the parallel runtime restricts access to one region and stages
// level-wide counters until commit. Rules hold no pointers across calls except
// the block-entity and entity pointers documented below.
class RuleContext {
public:
    explicit RuleContext(const contracts::VersionBundle& bundle) noexcept : bundle_(&bundle) {}
    virtual ~RuleContext() = default;
    RuleContext(const RuleContext&) = delete;
    RuleContext& operator=(const RuleContext&) = delete;

    [[nodiscard]] const contracts::VersionBundle& bundle() const noexcept { return *bundle_; }
    NeighborUpdater& updater() noexcept { return updater_; }

    [[nodiscard]] virtual Tick now() const = 0;
    // Unloaded chunks and positions outside the world height read as the sentinel.
    virtual BlockState block(BlockPos pos) = 0;
    // Raw write without reactions. Precondition: block(pos) is not the sentinel.
    virtual void write_block(BlockPos pos, BlockState state) = 0;
    // Generation of a loaded chunk; nullopt when unloaded. Loading state is
    // fixed for the duration of a phase.
    virtual std::optional<contracts::Generation> chunk_generation(ChunkPos chunk) = 0;

    virtual bool has_tick(TickList list, BlockPos pos, BlockType block) = 0;
    // `seq` is the trace sequence of the corresponding tick_scheduled record.
    // The executor assigns the level-wide sub-order in serial order.
    virtual void add_tick(TickList list, BlockPos pos, BlockType block, Tick due, std::int8_t priority,
                          std::uint32_t seq) = 0;
    virtual bool has_block_event(const BlockEvent& event) = 0;
    virtual void add_block_event(const BlockEvent& event, std::uint32_t seq) = 0;

    // Pointers stay valid until the current root finishes or the block entity
    // is removed.
    virtual const BlockEntity* peek_block_entity(BlockPos pos) = 0;
    virtual BlockEntity* block_entity(BlockPos pos) = 0;
    // Block-entity creation order is a level-wide counter, staged like entity ids.
    virtual void create_block_entity(BlockPos pos, BlockType kind) = 0;
    virtual void remove_block_entity(BlockPos pos) = 0;

    // Ids of entities whose block position equals `cell`, ascending.
    virtual std::vector<EntityId> entities_in_cell(BlockPos cell) = 0;
    virtual const Entity* peek_entity(EntityId id) = 0;
    // Pointer stays valid until the current root finishes or the entity moves
    // between chunks or is removed.
    virtual Entity* entity(EntityId id) = 0;
    // Assigns the entity id. Ids are a level-wide counter: executors that run
    // roots in parallel stage the assignment and give out ids in serial order.
    virtual EntityId spawn_entity(const Entity& prototype) = 0;
    // Precondition: the destination chunk is loaded.
    virtual void relocate_entity(EntityId id, FixedVec position) = 0;
    virtual void remove_entity(EntityId id) = 0;

    // Appends a trace record at the next sequence of the current root and
    // returns that sequence.
    virtual std::uint32_t record(TraceKind kind, BlockPos pos, std::uint64_t a = 0, std::uint64_t b = 0,
                                 std::uint64_t c = 0) = 0;

private:
    const contracts::VersionBundle* bundle_;
    NeighborUpdater updater_;
};

} // namespace circuit::gameplay
