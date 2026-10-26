// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/errors.hpp>
#include <circuit/contracts/ids.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Normalized external intents. Network, plugin and test code submit these
// values; the simulation validates them under the selected rules. A command
// never carries a pointer into world storage.
namespace circuit::contracts {

struct SetBlock {
    BlockPos pos;
    std::uint32_t state{}; // Encoded by the selected rule set's block registry.
    std::optional<Generation> expected_generation; // Rejects a write prepared for an older chunk lifetime.
    bool operator==(const SetBlock&) const = default;
};
// Interaction with a block, as a player's use: toggles a lever, cycles a
// repeater's delay, toggles a comparator's mode.
struct UseBlock {
    BlockPos pos;
    bool operator==(const UseBlock&) const = default;
};
struct InsertItem {
    BlockPos pos;
    std::uint8_t slot{};
    ItemStack stack;
    bool operator==(const InsertItem&) const = default;
};
struct SpawnItem {
    Vec3 position;
    Vec3 velocity;
    ItemStack stack;
    std::uint16_t pickup_delay{}; // 0 to 32767 (never picked up)
    std::int32_t age{};           // -32768 (never ages) or 0 to 32767
    bool operator==(const SpawnItem&) const = default;
};
// An entity summoned as /summon does it: of a type the rules support
// (minecraft:tnt, minecraft:arrow, minecraft:snowball), with a velocity,
// primed TNT's fuse and the ticks it burns for.
struct SummonEntity {
    std::string type; // resource id, e.g. "minecraft:arrow"
    Vec3 position;
    Vec3 velocity;
    std::int32_t fuse{80};
    std::int32_t fire{};
    bool operator==(const SummonEntity&) const = default;
};
struct RemoveEntity {
    EntityId id;
    bool operator==(const RemoveEntity&) const = default;
};
// Chunk tickets, as in the target versions (minecraft.wiki "Chunk", observed
// on both official servers). A ticket gives the chunks of a square around
// its center its level; every chunk further out takes one level more per
// chunk of Chebyshev distance, and a chunk takes the lowest level of any
// ticket. A chunk's load level decides whether it is loaded and its status:
// 31 and lower entity ticking, 32 block ticking, 33 border, higher unloaded.
// Its simulation level, from the tickets that simulate (all but a player's
// loading ticket), decides what runs: at 32 and lower scheduled ticks, block
// events and block entities, at 31 and lower entities too (random ticks:
// RuleSet::random_ticks). Ticket ids are unique over all levels. Changes
// take effect at the end of the command phase.
enum class TicketType : std::uint8_t {
    forced,            // /forceload, level 31; persists; keeps its level from idling
    start,             // the spawn chunks: level 32 - spawnChunkRadius
    player_loading,    // the chunks a player's view distance loads, level 31; does not simulate
    player_simulation, // a player's simulation distance: level 31 - distance at the player's chunk; simulates
                       // the chunks other tickets load, without loading any
    post_teleport,     // after a teleport, level 32 (33 through an end portal), for 5 ticks
    portal,            // where an entity arrives through a portal, level 30, for 300 ticks
    ender_pearl,       // a thrown ender pearl's chunk, level 31, for 40 ticks while it flies
    dragon,            // the dragon fight, level 24
    unknown,           // a chunk needed for one tick, level 33
};
inline constexpr std::uint8_t ticket_type_count = 9;
inline constexpr std::uint8_t unloaded_level = 34;      // and higher: not loaded

struct AddTicket {
    std::uint32_t ticket{};
    ChunkPos center;
    std::uint8_t radius{};       // the square of chunks within this distance takes the level
    TicketType type{};
    std::uint8_t level{31};
    std::uint32_t lifetime{};    // ticks until it is removed; 0: until removed
    bool operator==(const AddTicket&) const = default;
};
struct RemoveTicket {
    std::uint32_t ticket{};
    bool operator==(const RemoveTicket&) const = default;
};

// Player inventories use the slot numbers of the player's own window: 0 the
// crafting result, 1-4 the crafting grid, 5-8 armor, 9-35 the main inventory,
// 36-44 the hotbar and 45 the off hand.
inline constexpr std::uint8_t player_slots = 46;
inline constexpr std::uint8_t hotbar_first = 36;
inline constexpr std::uint8_t offhand_slot = 45;
// The crafting table window a player has open holds its result and 3x3 grid
// (window slots 0-9); slot records number them from table_first (after the
// cursor, 46).
inline constexpr std::uint8_t table_slots = 10;
inline constexpr std::uint8_t table_first = 47;
// The stonecutter window a player has open holds its input and result
// (window slots 0-1), recorded as slots 57-58.
inline constexpr std::uint8_t cutter_slots = 2;
inline constexpr std::uint8_t cutter_first = 57;
// The smithing table window: template, base, addition and result (window
// slots 0-3), recorded as slots 59-62.
inline constexpr std::uint8_t smith_slots = 4;
inline constexpr std::uint8_t smith_first = 59;
// The grindstone window: two inputs and the result (window slots 0-2),
// recorded as slots 63-65.
inline constexpr std::uint8_t grind_slots = 3;
inline constexpr std::uint8_t grind_first = 63;
// The loom window: banner, dye, pattern item and result (window slots
// 0-3), recorded as slots 66-69.
inline constexpr std::uint8_t loom_slots = 4;
inline constexpr std::uint8_t loom_first = 66;

// A creative player sets a slot of their inventory (an empty stack clears it).
struct SetPlayerSlot {
    PlayerKey player;
    std::uint8_t slot{};
    ItemStack stack;
    bool operator==(const SetPlayerSlot&) const = default;
};

// A player's click on a block face, resolved when it is applied, against the
// state that earlier commands of the same tick left, including the item in
// the clicking hand's slot. Admission checks only what the world does not
// decide: which of the two possible positions, the clicked block or its
// neighbor on the face, the player reaches (`reach`) and which no player
// stands in (`free`). When applied:
// - a usable block is used, unless the player sneaks while holding an item;
// - otherwise the held item places its block, oriented by the player's facing
//   and the face: it replaces the clicked block if that one is replaceable,
//   or goes next to it, if that position is reached, free when the block has
//   collision, replaceable and supports the block. In survival the item is
//   used up.
struct PlayerUse {
    PlayerKey player;
    std::uint8_t slot{};                  // the clicking hand's inventory slot (36-44 or 45)
    BlockPos pos;                         // the clicked block
    Direction face{};                     // the clicked face
    Direction horizontal{Direction::north}; // the player's horizontal facing
    Direction look{Direction::north};     // the player's facing, including up and down
    bool sneaking{};
    bool survival{};
    std::uint8_t reach{};                 // bit 0: `pos`, bit 1: its neighbor on `face`
    std::uint8_t free{};                  // bit 0: `pos`, bit 1: its neighbor, free of players
    // Where the click hit `pos`, within the block (0 to 1 on each axis).
    float cursor_x{0.5f};
    float cursor_y{0.5f};
    float cursor_z{0.5f};
    // The player's yaw in degrees (0 south, 90 west), for blocks that turn to
    // the player in sixteen steps.
    float yaw{};
    bool operator==(const PlayerUse&) const = default;
};
// The block a player's view meets first: the block, the face it enters and
// where it hits it (0 to 1 on each axis).
struct BlockHit {
    BlockPos pos;
    Direction face{};
    float cursor_x{};
    float cursor_y{};
    float cursor_z{};
    bool operator==(const BlockHit&) const = default;
};
// A player uses the held item along their view rather than on a block (a
// bucket), resolved when applied. The view starts at `eye` and goes `reach`
// blocks along `yaw` and `pitch` (degrees: yaw 0 south and 90 west, pitch 90
// straight down); `hit` is the block it meets first, as the client reported
// clicking it, absent when it meets none within reach. An empty bucket
// takes the first fluid source on the view before that block, or from the
// block; a filled one pours into the block or beside it. In survival the
// bucket changes in the hand.
struct PlayerUseItem {
    PlayerKey player;
    std::uint8_t slot{}; // the using hand's inventory slot (36-44 or 45)
    Vec3 eye;
    float yaw{};
    float pitch{};
    double reach{};
    bool survival{};
    std::optional<BlockHit> hit;
    bool operator==(const PlayerUseItem&) const = default;
};
// A player breaks a block, resolved when applied: air and piston parts
// cannot be broken. In survival the block drops what the held tool harvests,
// containers drop their contents, and the tool wears.
struct PlayerBreak {
    PlayerKey player;
    BlockPos pos;
    bool survival{};
    std::uint8_t slot{hotbar_first}; // the main hand's inventory slot
    bool operator==(const PlayerBreak&) const = default;
};
// A player picks up an item entity they touch; applied only once its pickup
// delay is over, as far as the inventory has room. `selected` is the main
// hand's inventory slot, which items fill first. A creative player takes the
// whole entity even without room.
struct PickUp {
    PlayerKey player;
    EntityId entity;
    std::uint8_t selected{};
    bool creative{};
    bool operator==(const PickUp&) const = default;
};
// A player drops one item (or the whole stack) of an inventory slot, spawned
// at `position` with `velocity` and a pickup delay of 40 ticks.
struct DropItem {
    PlayerKey player;
    std::uint8_t slot{};
    bool whole_stack{};
    Vec3 position;
    Vec3 velocity;
    bool operator==(const DropItem&) const = default;
};

// A click in a player's open window, applied by the rules as the target
// version's container menus apply it: the player's own window (no
// container) or the menu of the container at `container` (a chest, a double
// chest or a hopper), followed by the player's inventory. `mode` and `button`
// are the protocol's click type and button; slot -999 is outside the window.
// Items thrown out of the window spawn at `drop_position` with `drop_velocity`.
struct ContainerClick {
    PlayerKey player;
    std::optional<BlockPos> container;
    std::int16_t slot{};
    std::uint8_t button{};
    std::uint8_t mode{};
    bool creative{};
    Vec3 drop_position;
    Vec3 drop_velocity;
    bool operator==(const ContainerClick&) const = default;
};
// A player closes their window: the carried stack and the crafting grids
// (the own window's and an open crafting table's) go back into the
// inventory; what does not fit drops. When `drop` (the player left), they
// all drop instead, as the target versions drop a departed player's menus.
struct CloseContainer {
    PlayerKey player;
    Vec3 drop_position;
    Vec3 drop_velocity;
    bool drop{};
    bool operator==(const CloseContainer&) const = default;
};

// A player dies: the whole inventory, then the crafting grid and the carried
// stack, drops around `position` (the eyes less 0.3). Items dropped where no
// entity can exist, such as below the world, are lost.
struct PlayerDeath {
    PlayerKey player;
    Vec3 position;
    bool operator==(const PlayerDeath&) const = default;
};

// A player in the command's level: where they are and whether they are a
// spectator, for the rules that depend on players (random ticks near players
// in 1.21.4; whether a level is idle). `present = false` takes them out. A
// player is in at most one level: presence in one removes it from the others.
struct PlayerPresence {
    PlayerKey player;
    Vec3 position;
    bool spectator{};
    bool present{true};
    bool operator==(const PlayerPresence&) const = default;
};

// The server started (or restarted): every level counts as active at this
// tick, as the target versions' levels do when they are created
// (contracts::idle_level_ticks).
struct LevelsStarted {
    bool operator==(const LevelsStarted&) const = default;
};

// A game rule of the world takes a value. The bundle's schema names the
// rules and their types; a boolean is 0 or 1. Game rules belong to the
// world, not to a level.
inline constexpr std::size_t max_game_rule_name = 64;
// Records store a summoned entity's type in at most 64 characters of a
// resource id.
inline constexpr std::size_t max_entity_type = 64;
[[nodiscard]] inline bool valid_entity_type(std::string_view type) noexcept {
    return !type.empty() && type.size() <= max_entity_type
        && std::ranges::all_of(type, [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == ':'; });
}
struct SetGameRule {
    std::string name;
    std::int32_t value{};
    bool operator==(const SetGameRule&) const = default;
};

// The world's day time (ticks; 24000 a day) takes a value, never negative.
// Like the game rules it belongs to the world: only the overworld has a
// daylight cycle.
struct SetDayTime {
    std::int64_t time{};
    bool operator==(const SetDayTime&) const = default;
};

// The world's weather is set as /weather sets it (gameplay/weather.hpp):
// clear, rain or thunder for a duration in ticks, or the kind's default
// duration, drawn by the rules, when it is 0.
enum class WeatherKind : std::uint8_t { clear, rain, thunder };
inline constexpr std::uint8_t weather_kind_count = 3;
struct SetWeather {
    WeatherKind kind{};
    std::int32_t duration{};
    bool operator==(const SetWeather&) const = default;
};

// The world's difficulty takes a value: 0 peaceful, 1 easy, 2 normal, 3
// hard. Like the game rules it belongs to the world.
inline constexpr std::uint8_t max_difficulty = 3;
struct SetDifficulty {
    std::uint8_t difficulty{};
    bool operator==(const SetDifficulty&) const = default;
};

// A player presses a button of their window (the protocol's container
// button click): a stonecutter's recipe, by its place in the input's list.
// `container` is the window's block.
struct ContainerButton {
    PlayerKey player;
    std::optional<BlockPos> container;
    std::int32_t button{};
    bool operator==(const ContainerButton&) const = default;
};

// A player enables or disables a slot of their window (a crafter's grid
// slot, 0-8). `container` is the window's block.
struct SetSlotEnabled {
    PlayerKey player;
    std::optional<BlockPos> container;
    std::uint8_t slot{};
    bool enabled{};
    bool operator==(const SetSlotEnabled&) const = default;
};

using CommandBody = std::variant<SetBlock, UseBlock, InsertItem, SpawnItem, RemoveEntity, AddTicket, RemoveTicket,
                                 PlayerUse, PlayerBreak, SetPlayerSlot, PickUp, DropItem, ContainerClick, CloseContainer,
                                 PlayerDeath, PlayerPresence, LevelsStarted, SetGameRule, SetDayTime, SetWeather, SummonEntity, PlayerUseItem,
                                 SetDifficulty, ContainerButton, SetSlotEnabled>;

// Admission order is the recorded decision that orders inputs from different
// sources. Network arrival timing on a particular thread never orders commands.
// A command acts in one level: its positions, tickets and entities are that
// level's. Player inventories belong to no level.
struct Command {
    std::uint64_t admission{};
    CommandBody body;
    DimensionId dimension{};
    bool operator==(const Command&) const = default;
};

// Single-owner (coordinator) queue. Cancellation succeeds only before the
// command is drained into a tick.
class CommandQueue {
public:
    // `first_admission` continues the numbering of an earlier run, so that
    // admissions stay strictly increasing across restarts.
    explicit CommandQueue(std::size_t capacity = 65536, std::uint64_t first_admission = 1)
        : capacity_(capacity), next_(first_admission) {}
    Result<std::uint64_t> admit(CommandBody body, DimensionId dimension = DimensionId::overworld) {
        if (pending_.size() >= capacity_) return std::unexpected(ErrorCode::capacity_exceeded);
        // Records store a game rule's name in at most 64 letters.
        if (const auto* rule = std::get_if<SetGameRule>(&body);
            rule && (rule->name.empty() || rule->name.size() > max_game_rule_name
                     || !std::ranges::all_of(rule->name, [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); })))
            return std::unexpected(ErrorCode::invalid_argument);
        if (const auto* day = std::get_if<SetDayTime>(&body); day && day->time < 0) return std::unexpected(ErrorCode::invalid_argument);
        if (const auto* set = std::get_if<SetDifficulty>(&body); set && set->difficulty > max_difficulty)
            return std::unexpected(ErrorCode::invalid_argument);
        if (const auto* summon = std::get_if<SummonEntity>(&body); summon && !valid_entity_type(summon->type))
            return std::unexpected(ErrorCode::invalid_argument);
        if (const auto* weather = std::get_if<SetWeather>(&body);
            weather && (static_cast<std::uint8_t>(weather->kind) >= weather_kind_count || weather->duration < 0))
            return std::unexpected(ErrorCode::invalid_argument);
        if (next_ == UINT64_MAX) return std::unexpected(ErrorCode::counter_overflow);
        const auto id = next_++;
        pending_.emplace(id, std::pair{std::move(body), dimension});
        return id;
    }
    bool cancel(std::uint64_t admission) { return pending_.erase(admission) == 1; }
    [[nodiscard]] std::size_t size() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t next_admission() const noexcept { return next_; }
    std::vector<Command> drain() {
        std::vector<Command> out;
        out.reserve(pending_.size());
        for (auto& [id, entry] : pending_) out.push_back({id, std::move(entry.first), entry.second});
        pending_.clear();
        return out;
    }

private:
    std::size_t capacity_;
    std::uint64_t next_;
    std::map<std::uint64_t, std::pair<CommandBody, DimensionId>> pending_;
};

} // namespace circuit::contracts
