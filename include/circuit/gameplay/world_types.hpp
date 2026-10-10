// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/ids.hpp>
#include <circuit/contracts/time.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/block.hpp>
#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

// Value types for world state shared by the reference oracle and the runtime.
// Storage layout and ordering machinery are executor-specific; these are not.
namespace circuit::gameplay {
using contracts::BlockPos;
using contracts::ChunkPos;
using contracts::EntityId;
using contracts::FixedVec;
using contracts::ItemStack;
using contracts::Tick;
using contracts::TickList;

struct BlockEntity {
    BlockType kind{};
    std::uint64_t order{}; // Creation order; also the block-entity tick order.
    std::vector<ItemStack> slots;
    std::uint8_t cooldown{};
    Tick ticked{};          // Hopper: the last tick it ticked (whether it already ticked this tick).
    std::uint8_t signal{}; // Comparator output, 0-15.
    // Moving piston: the block being moved, ticks of movement completed (0-2),
    // and motion bits (see `Motion`).
    std::uint32_t moving{};
    std::uint8_t progress{};
    std::uint8_t motion{};
    // Data components of the block entity (a container's custom name), in
    // the item component form of the world's protocol.
    contracts::ComponentPatch components{};
    // Kind `other`: the saved data of a block entity of a block without rules
    // of its own, kept as the target version saves it (network NBT, without
    // the position) and never interpreted.
    contracts::Blob saved{};
    bool operator==(const BlockEntity&) const = default;
};

// Motion of a moving piston: the facing of the piston that moves it, whether
// the piston is extending, and whether this is the piston's own head or base.
struct Motion {
    contracts::Direction facing{};
    bool extending{};
    bool source{};
    [[nodiscard]] constexpr std::uint8_t pack() const noexcept {
        return static_cast<std::uint8_t>(static_cast<unsigned>(facing) | (extending ? 8u : 0u) | (source ? 16u : 0u));
    }
    static constexpr Motion unpack(std::uint8_t bits) noexcept {
        return {static_cast<contracts::Direction>(bits & 7), (bits & 8) != 0, (bits & 16) != 0};
    }
};

// Item entities move less than one block per tick on each axis, so they never
// skip a cell; restored states are checked against this bound.
inline constexpr std::int64_t max_entity_speed = contracts::fixed_one - 1;

struct Entity {
    EntityId id;
    FixedVec position;
    FixedVec velocity;
    ItemStack stack;
    std::uint32_t age{};
    Tick last_tick{}; // Logical tick of the last basic advancement; prevents double ticking.
    std::uint16_t pickup_delay{}; // ticks until players may pick it up
    bool on_ground{};
    bool operator==(const Entity&) const = default;
};

enum class RemovalReason : std::uint8_t { despawned = 1, fell_out_of_world = 2, picked_up = 3, command = 4 };

// A player's inventory in the simulation: the slots of the player's own window
// (see contracts::player_slots) and the stack carried by the cursor. Only
// players with an item appear in dumps.
struct PlayerInventory {
    contracts::PlayerKey player;
    std::array<ItemStack, contracts::player_slots> slots{};
    ItemStack cursor;
    // A drag across slots in progress (several clicks): its stage (0 none,
    // 1 collecting slots), its type (0 split evenly, 1 one each, 2 full
    // stacks) and the slots collected so far, in window numbering.
    std::uint8_t drag_stage{};
    std::uint8_t drag_type{};
    std::vector<std::int16_t> drag_slots;
    [[nodiscard]] bool empty() const noexcept {
        return cursor.empty() && drag_stage == 0 && std::ranges::all_of(slots, [](const ItemStack& s) { return s.empty(); });
    }
    bool operator==(const PlayerInventory&) const = default;
};

namespace piston_event {
inline constexpr std::uint8_t extend = 0;
inline constexpr std::uint8_t retract = 1;
} // namespace piston_event

struct BlockEvent {
    BlockPos pos;
    BlockType block{};
    std::uint8_t event{};
    std::uint8_t param{};
    auto operator<=>(const BlockEvent&) const = default;
};

struct ScheduledTick {
    BlockPos pos;
    BlockType block{};
    Tick due{};
    std::int8_t priority{};
    std::uint64_t suborder{}; // Level-wide counter value assigned in serial order.
    bool operator==(const ScheduledTick&) const = default;
};
// Drain order of one tick list.
constexpr bool tick_before(const ScheduledTick& a, const ScheduledTick& b) noexcept {
    if (a.due != b.due) return a.due < b.due;
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.suborder < b.suborder;
}

enum class ChunkLevel : std::uint8_t { unloaded = 0, border = 1, ticking = 2 };

struct Ticket {
    std::uint32_t id{};
    ChunkPos center;
    std::uint8_t radius{};      // ticking
    std::uint8_t load_radius{}; // loaded without ticking: max(radius + 1, load_radius)
    contracts::DimensionId dimension{}; // the level whose chunks it loads
    [[nodiscard]] constexpr std::int32_t reach() const noexcept {
        return std::max<std::int32_t>(std::int32_t{radius} + 1, load_radius);
    }
    bool operator==(const Ticket&) const = default;
};

// Ticket bookkeeping and the load-level rule. Owned by one executor; changes
// are applied to the active chunk set only at the command-phase boundary.
// Ticket ids are unique over all levels; a ticket loads chunks of its level.
class TicketSet {
public:
    contracts::Result<void> add(const contracts::AddTicket& command, contracts::DimensionId dimension,
                                const contracts::RuleLimits& limits);
    contracts::Result<void> remove(std::uint32_t id);
    [[nodiscard]] std::map<contracts::ChunkKey, ChunkLevel> levels() const;
    [[nodiscard]] std::vector<Ticket> list() const;

private:
    std::map<std::uint32_t, Ticket> tickets_;
};

// One level's state in a WorldDump. Positions are the level's. The scheduled
// tick sub-order and the block-entity order are per-level counters, as in the
// target versions; entity ids are counted over all levels.
struct LevelDump {
    contracts::DimensionId dimension{};
    std::uint64_t next_suborder{};
    std::uint64_t next_block_entity_order{};
    struct Chunk {
        ChunkPos pos;
        std::uint32_t generation{};
        ChunkLevel level{};
        bool operator==(const Chunk&) const = default;
    };
    struct PendingTick {
        TickList list{};
        ScheduledTick tick;
        bool operator==(const PendingTick&) const = default;
    };
    std::vector<Chunk> chunks;                                  // ascending position
    std::vector<std::pair<BlockPos, std::uint32_t>> blocks;     // non-air, ascending position
    std::vector<std::pair<BlockPos, BlockEntity>> block_entities; // ascending position
    std::vector<Entity> entities;                               // ascending id
    std::vector<PendingTick> ticks;                             // by list, then drain order
    std::vector<BlockEvent> block_events;                       // queue order
    bool operator==(const LevelDump&) const = default;
};

// Canonical, storage-independent view of all simulation state, used to compare
// executors. Unloaded chunk contents are included: unloading must not lose them.
struct WorldDump {
    using Chunk = LevelDump::Chunk;
    using PendingTick = LevelDump::PendingTick;
    Tick time{};
    std::uint64_t next_entity_id{};
    std::vector<LevelDump> levels;        // ascending dimension; a level is listed once it has a column
    std::vector<Ticket> tickets;          // ascending id
    std::vector<PlayerInventory> players; // ascending player, non-empty only
    bool operator==(const WorldDump&) const = default;
    // A level's state; an empty one when the level has no column.
    [[nodiscard]] const LevelDump& level(contracts::DimensionId dimension) const;
    // The level's entry, added in order when absent.
    LevelDump& level(contracts::DimensionId dimension);
    [[nodiscard]] const LevelDump& overworld() const { return level(contracts::DimensionId::overworld); }
};
// Sorts the order-insensitive sections into canonical order.
void canonicalize(WorldDump& dump);
// Human-readable description of the first difference; empty when equal.
std::string first_difference(const WorldDump& a, const WorldDump& b);
// Order-sensitive 64-bit digest of a canonical dump, for compact reporting.
// Equal digests are not proof of equal states; compare dumps for evidence.
std::uint64_t digest(const WorldDump& dump);

} // namespace circuit::gameplay
