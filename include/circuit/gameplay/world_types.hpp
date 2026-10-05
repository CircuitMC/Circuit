// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
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
    std::uint8_t signal{}; // Comparator output, 0-15.
    bool operator==(const BlockEntity&) const = default;
};

struct Entity {
    EntityId id;
    FixedVec position;
    FixedVec velocity;
    ItemStack stack;
    std::uint32_t age{};
    Tick last_tick{}; // Logical tick of the last basic advancement; prevents double ticking.
    bool on_ground{};
    bool operator==(const Entity&) const = default;
};

enum class RemovalReason : std::uint8_t { despawned = 1, fell_out_of_world = 2, picked_up = 3, command = 4 };

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
    std::uint8_t radius{};
    bool operator==(const Ticket&) const = default;
};

// Ticket bookkeeping and the load-level rule. Owned by one executor; changes
// are applied to the active chunk set only at the command-phase boundary.
class TicketSet {
public:
    contracts::Result<void> add(const contracts::AddTicket& command, const contracts::RuleLimits& limits);
    contracts::Result<void> remove(std::uint32_t id);
    [[nodiscard]] std::map<ChunkPos, ChunkLevel> levels() const;
    [[nodiscard]] std::vector<Ticket> list() const;

private:
    std::map<std::uint32_t, Ticket> tickets_;
};

// Canonical, storage-independent view of all simulation state, used to compare
// executors. Unloaded chunk contents are included: unloading must not lose them.
struct WorldDump {
    Tick time{};
    std::uint64_t next_suborder{};
    std::uint64_t next_entity_id{};
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
    std::vector<Ticket> tickets;                                // ascending id
    bool operator==(const WorldDump&) const = default;
};
// Sorts the order-insensitive sections into canonical order.
void canonicalize(WorldDump& dump);
// Human-readable description of the first difference; empty when equal.
std::string first_difference(const WorldDump& a, const WorldDump& b);
// Order-sensitive 64-bit digest of a canonical dump, for compact reporting.
// Equal digests are not proof of equal states; compare dumps for evidence.
std::uint64_t digest(const WorldDump& dump);

} // namespace circuit::gameplay
