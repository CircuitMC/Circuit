// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <optional>
#include <vector>

namespace circuit::gameplay {
using contracts::TraceKind;

// Per-root state of immediate update propagation (observed in target-version
// traces). Neighbor and shape updates are queued entries. An entry emitted
// while another runs joins the current layer; after each delivery the layer is
// pushed so that its entries run in emission order before the next sibling
// delivery (depth-first). An entry emitted outside a chain starts one, which
// lasts until the queue empties. A chain counts the entries added to it; once
// the count reaches the version limit, new entries are dropped while queued
// entries still run. Shape-update nesting is limited separately.
struct NeighborUpdater {
    enum class Kind : std::uint8_t {
        neighbors, // six neighbors of `source`, in update order, except `skip`
        single,    // one neighbor update to `target`
        shape,     // one shape update to `target`
    };
    struct Entry {
        Kind kind{};
        BlockPos source;
        BlockPos target;
        std::uint8_t next{};   // Next direction index of a `neighbors` entry.
        std::int8_t skip{-1};  // Direction not notified, or -1.
        std::uint16_t depth{}; // Nesting of a `shape` entry.
    };
    std::vector<Entry> stack;
    std::vector<Entry> layer;
    bool running{};
    bool truncated{};
    std::uint64_t count{};       // Entries added to the current chain.
    std::uint16_t shape_depth{}; // Nesting of the shape entry being delivered; 0 otherwise.
    void reset() noexcept {
        stack.clear();
        layer.clear();
        running = false;
        truncated = false;
        count = 0;
        shape_depth = 0;
    }
};

// Everything a rule may do to the world. An executor implements it over its
// own storage and ordering machinery: the sequential oracle applies effects
// immediately; the parallel runtime restricts access to one region and stages
// level-wide counters until commit. Rules hold no pointers across calls except
// the block-entity and entity pointers documented below.
//
// A context acts in one level at a time (dimension()): every position, chunk,
// entity and scheduled tick it names is that level's. Rules never reach into
// another level.
class RuleContext {
public:
    explicit RuleContext(const contracts::VersionBundle& bundle, const GameData* data = nullptr) noexcept
        : bundle_(&bundle), data_(data), shape_(&bundle.world) {}
    virtual ~RuleContext() = default;
    RuleContext(const RuleContext&) = delete;
    RuleContext& operator=(const RuleContext&) = delete;

    [[nodiscard]] const contracts::VersionBundle& bundle() const noexcept { return *bundle_; }
    [[nodiscard]] contracts::DimensionId dimension() const noexcept { return dimension_; }
    // The build height of the current level.
    [[nodiscard]] const contracts::WorldShape& world() const noexcept { return *shape_; }
    // Data-pack data, or nullptr: the rules then use the public registry data.
    [[nodiscard]] const GameData* data() const noexcept { return data_; }
    NeighborUpdater& updater() noexcept { return updater_; }

    [[nodiscard]] virtual Tick now() const = 0;
    // Unloaded chunks and positions outside the world height read as the sentinel.
    virtual BlockState block(BlockPos pos) = 0;
    // Raw write without reactions. Precondition: block(pos) is not the sentinel.
    virtual void write_block(BlockPos pos, BlockState state) = 0;
    // A game rule's value (gameplay::game_rule_value).
    virtual std::int32_t game_rule(std::string_view name) = 0;
    // The light at a position as the tick sees it: the most of its block
    // light and sky light, from the blocks at the end of the previous tick
    // (gameplay/light.hpp). Available in the random-tick phase.
    virtual std::uint8_t light(BlockPos pos) = 0;
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

    // Player inventories, in the command phase only (other phases throw). The
    // reference stays valid until the current root finishes.
    virtual const PlayerInventory* peek_player(contracts::PlayerKey player) = 0;
    virtual PlayerInventory& player(contracts::PlayerKey player) = 0; // created empty when absent

    // Appends a trace record at the next sequence of the current root and
    // returns that sequence.
    virtual std::uint32_t record(TraceKind kind, BlockPos pos, std::uint64_t a = 0, std::uint64_t b = 0,
                                 std::uint64_t c = 0) = 0;

protected:
    // Executors switch the level between roots, never within one.
    void enter(contracts::DimensionId dimension) noexcept {
        dimension_ = dimension;
        shape_ = &bundle_->shape(dimension);
    }

private:
    const contracts::VersionBundle* bundle_;
    const GameData* data_;
    const contracts::WorldShape* shape_;
    contracts::DimensionId dimension_{};
    NeighborUpdater updater_;
};

} // namespace circuit::gameplay
