// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/level_random.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
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
        // The block the update comes from, as the target versions name it:
        // the block a change replaced, or the block that sends the update.
        BlockState cause{};
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

// One 16x16x16 section of blocks in the paletted layout of the protocol's
// chunk data: each block is an index into `palette`, packed `bits` to a value
// in 64-bit words from the lowest bits up, `per_word` values to a word, none
// spanning two words, in section_local_index order (y, then z, then x). With
// `bits` 0 every block is palette[0] and `words` is unused. The view borrows
// the executor's storage; the executor keeps it valid while a cursor holds it.
struct SectionView {
    const BlockState* palette{};
    const std::uint64_t* words{};
    std::uint8_t bits{};
    std::uint8_t per_word{}; // 64 / bits; set by make()

    // A view of packed values (bits 1 to 32), or of one state for the whole
    // section (bits 0).
    static constexpr SectionView make(const BlockState* palette, const std::uint64_t* words, std::uint8_t bits) noexcept {
        return {palette, words, bits, bits == 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(64 / bits)};
    }
    [[nodiscard]] BlockState at(std::uint32_t index) const noexcept {
        if (bits == 0) return palette[0];
        const auto word = words[index / per_word];
        const auto shift = (index % per_word) * bits;
        const auto mask = (std::uint64_t{1} << bits) - 1;
        return palette[(word >> shift) & mask];
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
    // Unloaded chunks and positions outside the world height read as the
    // sentinel. A read inside the section the cursor holds is served inline
    // from the executor's storage; any other goes to load_block.
    BlockState block(BlockPos pos) {
        if (cursor_holds(pos)) return cursor_.view.at(contracts::section_local_index(pos));
        return load_block(pos);
    }
    // Raw write without reactions. Precondition: block(pos) is not the sentinel.
    virtual void write_block(BlockPos pos, BlockState state) = 0;
    // A game rule's value (gameplay::game_rule_value).
    virtual std::int32_t game_rule(std::string_view name) = 0;
    // The light at a position as the tick sees it: the most of its block
    // light and sky light, from the blocks at the end of the previous tick
    // (gameplay/light.hpp), in every phase.
    virtual std::uint8_t light(BlockPos pos) = 0;
    // The sky light alone, and the block light alone, from the same light.
    virtual std::uint8_t sky_light(BlockPos pos) = 0;
    virtual std::uint8_t block_light(BlockPos pos) = 0;
    // Whether a player of the level is within 128 blocks of the chunk's
    // center (what fire needs on 1.21.5 and later; presences change only in
    // the command phase).
    virtual bool player_near(ChunkPos chunk) = 0;
    // The non-spectator player of the level nearest to a position, within
    // `range` (gameplay::nearest_player; presences change only in the
    // command phase).
    virtual std::optional<Presence> nearest_player(contracts::Vec3 at, double range) = 0;
    // Where a player of the level is (their presence), if anywhere.
    virtual std::optional<Presence> presence_of(contracts::PlayerKey player) = 0;
    // Whether entities spawned now get their final ids (the serial command
    // phase); otherwise the commit gives them.
    [[nodiscard]] virtual bool final_ids() const noexcept = 0;
    // Whether a chunk's section (from the lowest, 0) holds states that make
    // it random-ticked (ticks_section_randomly), as it is now.
    virtual bool section_ticks_randomly(ChunkPos chunk, std::int32_t section) = 0;
    // The world's difficulty (contracts::SetDifficulty).
    virtual std::uint8_t difficulty() = 0;
    // The overworld's day time, and the darkening of its sky (0-11) as its
    // last tick computed it (gameplay/sky.hpp).
    struct Sky {
        std::int64_t day_time{};
        std::uint8_t darken{};
    };
    virtual Sky sky() = 0;
    // Generation of a loaded chunk; nullopt when unloaded. Loading state is
    // fixed for the duration of a phase.
    virtual std::optional<contracts::Generation> chunk_generation(ChunkPos chunk) = 0;

    virtual bool has_tick(TickList list, BlockPos pos, BlockType block) = 0;
    // `seq` is the trace sequence of the corresponding tick_scheduled record.
    // The executor assigns the level-wide sub-order in serial order.
    virtual void add_tick(TickList list, BlockPos pos, BlockType block, Tick due, std::int8_t priority,
                          std::uint32_t seq) = 0;
    virtual bool has_block_event(const BlockEvent& event) = 0;
    // Turn-offs of redstone torches (their burnout), kept with the column of
    // `pos` through block changes and reloads (count_turn_offs): forgets the
    // column's turn-offs before `since`, records one at `pos` now with
    // `add`, and returns how many `pos` has.
    virtual std::size_t torch_turn_offs(BlockPos pos, Tick since, bool add) = 0;
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
    // Ids of entities whose block position lies in the cells from `low` to
    // `high` (inclusive), ascending; nothing from columns not loaded.
    virtual std::vector<EntityId> entities_in_cells(BlockPos low, BlockPos high) = 0;
    virtual const Entity* peek_entity(EntityId id) = 0;
    // Pointer stays valid until the current root finishes or the entity moves
    // between chunks or is removed.
    virtual Entity* entity(EntityId id) = 0;
    // Assigns the entity id. Ids are a level-wide counter: executors that run
    // roots in parallel stage the assignment and give out ids in serial order.
    virtual EntityId spawn_entity(const Entity& prototype) = 0;
    // Precondition: the destination chunk is loaded.
    virtual void relocate_entity(EntityId id, Vec3 position) = 0;
    virtual void remove_entity(EntityId id) = 0;

    // Player inventories. The command phase uses them freely; a level's
    // other phases only for a root whose region holds the level's random
    // pseudo column (as level_random; a dispenser putting armor on a player),
    // so that one region at a time does, in root order. The reference stays
    // valid until the current root finishes.
    virtual const PlayerInventory* peek_player(contracts::PlayerKey player) = 0;
    virtual PlayerInventory& player(contracts::PlayerKey player) = 0; // created empty when absent
    // Maps: a map's data, a map to change (explore_held_maps), every map's
    // id in order, and a new map's id, the world's next, given to `data` as
    // it is added. The command phase uses them all; a level's other phases
    // only peek_map, add_map and last_map_id, for a root whose region holds
    // the level's random (as level_random; a crafter extending a map).
    virtual const MapData* peek_map(std::int32_t id) = 0;
    virtual MapData* map(std::int32_t id) = 0;
    virtual std::vector<std::int32_t> map_ids() = 0;
    virtual std::int32_t add_map(MapData data) = 0;
    virtual std::int32_t last_map_id() = 0; // -1 before the first
    // The players in the current level, by key (contracts::PlayerPresence).
    virtual std::vector<Presence> presences() = 0;
    // The highest block (not air) of a column of the current level, below the
    // level's floor when it has none; nullopt where the column is not loaded.
    virtual std::optional<std::int32_t> column_top(std::int32_t x, std::int32_t z) = 0;

    // Appends a trace record at the next sequence of the current root and
    // returns that sequence.
    virtual std::uint32_t record(TraceKind kind, BlockPos pos, std::uint64_t a = 0, std::uint64_t b = 0,
                                 std::uint64_t c = 0) = 0;

    // The level's random sequence (LevelRandom), where the target versions
    // draw from theirs. Each use records its raw draws (level_random_draw).
    // Uses are serial: the region-parallel runtime runs the roots that use
    // the sequence in one region, in root order.
    double random_double() { return draw([](LevelRandom& r) { return r.next_double(); }); }
    float random_float() { return draw([](LevelRandom& r) { return r.next_float(); }); }
    std::int32_t random_int(std::int32_t bound) { return draw([&](LevelRandom& r) { return r.next_int(bound); }); }
    bool random_boolean() { return draw([](LevelRandom& r) { return r.next_boolean(); }); }
    // Several draws recorded as one use (an explosion's rays, its shuffle).
    template <class F>
    std::invoke_result_t<F, LevelRandom&> random_draws(F&& f) {
        return draw(std::forward<F>(f));
    }
    // The next value of the level's random-tick position generator: v ->
    // 3v + 1013904223 (mod 2^32), from which random ticks take their positions
    // (both official servers, fitted to every pick of farmland layers). Uses
    // are serial as the level random's are; they are not level-random draws.
    std::uint32_t random_tick_pick() {
        auto& v = random_tick_state();
        v = v * 3u + 1013904223u;
        return v;
    }
    // The random sequence of a namespaced id (random_sequence.hpp; loot
    // tables roll from theirs), started from the world seed when first used.
    // Uses are serial as the level random's are.
    virtual RandomSequence& random_sequence(const std::string& id, const SequenceIdHash& hash) = 0;

protected:
    // The block at a position, as block() returns it, for every read the
    // cursor does not serve. An executor that uses the cursor checks access
    // here and may point the cursor at the position's section.
    virtual BlockState load_block(BlockPos pos) = 0;

    // The cursor: one section of the current level that block() reads
    // without a virtual call. An executor points it at a section it may read
    // and that is loaded and inside the world height, and drops it before the
    // section's view stops describing the section: when a write changes the
    // palette, the bit width or the storage the view borrows, when the column
    // unloads, or when the context's access changes. A write that only
    // changes a packed value in place keeps it valid. Changing the level
    // (enter) drops it. Executors that never point it (the oracle) read every
    // block through load_block.
    void point_cursor(BlockPos in_section, SectionView view) noexcept {
        cursor_ = {in_section.x >> 4, in_section.y >> 4, in_section.z >> 4, view, true};
    }
    void drop_cursor() noexcept { cursor_.valid = false; }
    [[nodiscard]] bool cursor_holds(BlockPos pos) const noexcept {
        return cursor_.valid && (pos.x >> 4) == cursor_.x && (pos.y >> 4) == cursor_.y && (pos.z >> 4) == cursor_.z;
    }

    // The current level's random sequence and random-tick position state.
    virtual LevelRandom& level_random() = 0;
    virtual std::uint32_t& random_tick_state() = 0;
    // Executors switch the level between roots, never within one.
    void enter(contracts::DimensionId dimension) noexcept {
        dimension_ = dimension;
        shape_ = &bundle_->shape(dimension);
        drop_cursor();
    }

private:
    template <class F>
    std::invoke_result_t<F, LevelRandom&> draw(F&& f) {
        auto& random = level_random();
        const auto before = random.draws();
        const auto value = f(random);
        record(TraceKind::level_random_draw, {}, random.draws() - before);
        return value;
    }

    struct Cursor {
        std::int32_t x{}, y{}, z{}; // section coordinates (block coordinates >> 4)
        SectionView view;
        bool valid{};
    };

    Cursor cursor_;
    const contracts::VersionBundle* bundle_;
    const GameData* data_;
    const contracts::WorldShape* shape_;
    contracts::DimensionId dimension_{};
    NeighborUpdater updater_;
};

} // namespace circuit::gameplay
