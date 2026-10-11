// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/ids.hpp>
#include <circuit/contracts/time.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/weather.hpp>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
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

// A loaded chunk's status, from its load level (contracts::AddTicket).
enum class ChunkLevel : std::uint8_t { unloaded = 0, border = 1, block_ticking = 2, entity_ticking = 3 };
constexpr ChunkLevel status_of(std::uint8_t load_level) noexcept {
    return load_level <= 31 ? ChunkLevel::entity_ticking
         : load_level == 32 ? ChunkLevel::block_ticking
         : load_level == 33 ? ChunkLevel::border
                            : ChunkLevel::unloaded;
}
// What a chunk's simulation level runs.
constexpr bool block_ticking(std::uint8_t simulation_level) noexcept { return simulation_level <= 32; }
constexpr bool entity_ticking(std::uint8_t simulation_level) noexcept { return simulation_level <= 31; }

struct Ticket {
    std::uint32_t id{};
    ChunkPos center;
    std::uint8_t radius{};
    contracts::TicketType type{};
    std::uint8_t level{31};
    Tick expires{};                     // removed at this tick's command phase; 0: never
    contracts::DimensionId dimension{}; // the level whose chunks it loads
    // A player's loading ticket loads without simulating; their simulation
    // ticket simulates without loading.
    [[nodiscard]] constexpr bool loads() const noexcept { return type != contracts::TicketType::player_simulation; }
    [[nodiscard]] constexpr bool simulates() const noexcept { return type != contracts::TicketType::player_loading; }
    // Chunks within this distance of the center take a level of 33 or lower.
    [[nodiscard]] constexpr std::int32_t reach() const noexcept {
        return std::int32_t{radius} + std::max(0, 33 - std::int32_t{level});
    }
    bool operator==(const Ticket&) const = default;
};

// A loaded chunk's levels: its load level and its simulation level (34 and
// higher: none). The simulation level is never below the load level: a chunk
// runs only what both allow.
struct ChunkLevels {
    std::uint8_t load{contracts::unloaded_level};
    std::uint8_t simulation{contracts::unloaded_level};
    bool operator==(const ChunkLevels&) const = default;
};

// Ticket bookkeeping and the level rule. Owned by one executor; changes are
// applied to the active chunk set only at the command-phase boundary.
// Ticket ids are unique over all levels; a ticket loads chunks of its level.
class TicketSet {
public:
    // `now`: the tick whose command phase adds it (for its lifetime).
    contracts::Result<void> add(const contracts::AddTicket& command, contracts::DimensionId dimension, Tick now,
                                const contracts::RuleLimits& limits);
    contracts::Result<void> remove(std::uint32_t id);
    // Removes the level's tickets whose lifetime is over by `now`; returns their ids.
    std::vector<std::uint32_t> expire(Tick now, contracts::DimensionId dimension);
    // Restores a ticket as listed.
    contracts::Result<void> restore(const Ticket& ticket, const contracts::RuleLimits& limits);
    // The levels of every chunk some ticket loads (load level 33 or lower),
    // in every level or in one.
    [[nodiscard]] std::map<contracts::ChunkKey, ChunkLevels> levels(std::optional<contracts::DimensionId> only = std::nullopt) const;
    // Whether a forced ticket keeps the level from idling.
    [[nodiscard]] bool keeps_active(contracts::DimensionId dimension) const;
    [[nodiscard]] std::vector<Ticket> list() const;

private:
    std::map<std::uint32_t, Ticket> tickets_;
};

// A player in a level (contracts::PlayerPresence).
struct Presence {
    contracts::PlayerKey player;
    FixedVec position;
    bool spectator{};
    bool operator==(const Presence&) const = default;
};

// Whether a level ticks its entities and block entities at tick `now`: it has
// players or forced chunks, or had them (or started) at most
// contracts::idle_level_ticks ticks ago.
constexpr bool level_awake(bool active, Tick now, Tick last_active, Tick started) noexcept {
    return active || now - std::max(last_active, started) <= contracts::idle_level_ticks;
}

// Whether a non-spectator player is less than 128 blocks horizontally from
// the chunk's center (x * 16 + 8, z * 16 + 8).
template <class Presences>
bool player_near(const Presences& presences, ChunkPos chunk) {
    constexpr std::int64_t range = 128 * contracts::fixed_one;
    const std::int64_t cx = (std::int64_t{chunk.x} * 16 + 8) * contracts::fixed_one;
    const std::int64_t cz = (std::int64_t{chunk.z} * 16 + 8) * contracts::fixed_one;
    for (const auto& entry : presences) {
        const Presence& p = [&]() -> const Presence& {
            if constexpr (requires { entry.second; }) return entry.second;
            else return entry;
        }();
        if (p.spectator) continue;
        const auto dx = p.position.x - cx, dz = p.position.z - cz;
        if (dx <= -range || dx >= range || dz <= -range || dz >= range) continue;
        if (dx * dx + dz * dz < range * range) return true;
    }
    return false;
}

// Game rules (contracts::SetGameRule). A value is valid for a rule the
// version has outside its experiments: a boolean is 0 or 1, and
// spawnChunkRadius lies in 0-32 (both official servers refuse other values;
// the other integer rules take any value).
contracts::Result<void> check_game_rule(const contracts::VersionBundle& bundle, const contracts::SetGameRule& rule);
// The values an integer rule takes.
struct GameRuleRange {
    std::int32_t min{INT32_MIN}, max{INT32_MAX};
};
GameRuleRange game_rule_range(std::string_view name) noexcept;
// Applies a valid rule to the rules away from their defaults.
void set_game_rule(const contracts::VersionBundle& bundle, std::map<std::string, std::int32_t>& set, const contracts::SetGameRule& rule);
// A rule's value: as set, else the version's default (0 for a rule it lacks).
std::int32_t game_rule_value(const contracts::VersionBundle& bundle, const std::map<std::string, std::int32_t>& set,
                             std::string_view name);

// Chunks that drop out of the loaded set and come back (both official
// servers): a dropped chunk's pending ticks keep their remaining delay,
// counted from the tick after it dropped (`saved_at`); reloaded at `now`, a
// tick due at `due` is due at now + (due - saved_at), overdue ones at once.
// Its scheduled ticks and block entities run again from now + reload_resume;
// its block events at once.
inline constexpr Tick reload_resume = 2;
constexpr Tick reload_shift(Tick due, Tick saved_at, Tick now) noexcept {
    if (due >= saved_at) return now + (due - saved_at);
    return now - std::min(now, saved_at - due);
}

// Whether a chunk's random ticks run (RuleSet::random_ticks).
template <class Presences>
bool random_ticks_run(contracts::RandomTickRange range, std::uint8_t simulation_level, ChunkPos chunk, const Presences& presences) {
    if (range == contracts::RandomTickRange::entity_ticking) return entity_ticking(simulation_level);
    return block_ticking(simulation_level) && player_near(presences, chunk);
}

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
        ChunkLevel level{};                // from the load level
        std::uint8_t load{contracts::unloaded_level};
        std::uint8_t simulation{contracts::unloaded_level};
        // Saved: the tick from which its pending ticks wait (reload_shift).
        // Loaded: the tick its scheduled ticks and block entities run again
        // after a reload, while still ahead; else 0.
        Tick saved_at{};
        Tick resumes{};
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
    std::vector<Presence> presences;                            // ascending player
    // The last tick the level had players or forced chunks (contracts::idle_level_ticks).
    Tick last_active{};
    bool operator==(const LevelDump&) const = default;
};

// Canonical, storage-independent view of all simulation state, used to compare
// executors. Unloaded chunk contents are included: unloading must not lose them.
struct WorldDump {
    using Chunk = LevelDump::Chunk;
    using PendingTick = LevelDump::PendingTick;
    Tick time{};
    std::uint64_t next_entity_id{};
    Tick started{}; // the last LevelsStarted command's tick, less one: levels count as active then
    // Game rules away from the bundle's defaults (contracts::SetGameRule).
    std::map<std::string, std::int32_t> game_rules;
    // The day time (contracts::SetDayTime): the world's, as only the
    // overworld has a daylight cycle. It advances by one each tick after the
    // commands while doDaylightCycle is on.
    std::int64_t day_time{};
    // The world's weather (contracts::SetWeather), as the overworld's: only it has weather.
    Weather weather;
    std::vector<LevelDump> levels;        // ascending dimension; listed with a column, a presence or an active tick
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
