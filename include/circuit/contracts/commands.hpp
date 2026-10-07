// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/errors.hpp>
#include <circuit/contracts/ids.hpp>
#include <cstdint>
#include <map>
#include <optional>
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
    FixedVec position;
    FixedVec velocity;
    ItemStack stack;
    bool operator==(const SpawnItem&) const = default;
};
struct RemoveEntity {
    EntityId id;
    bool operator==(const RemoveEntity&) const = default;
};
// Tickets determine which chunks are loaded and ticking. Changes take effect
// at the end of the command phase, never in the middle of a simulation phase.
// Chunks within Chebyshev distance `radius` of the center tick; chunks within
// max(radius + 1, load_radius) are loaded without ticking (a player's view).
struct AddTicket {
    std::uint32_t ticket{};
    ChunkPos center;
    std::uint8_t radius{};
    std::uint8_t load_radius{};
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
    bool operator==(const PlayerUse&) const = default;
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
    FixedVec position;
    FixedVec velocity;
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
    FixedVec drop_position;
    FixedVec drop_velocity;
    bool operator==(const ContainerClick&) const = default;
};
// A player closes their window: the carried stack and the crafting grid go
// back into the inventory; what does not fit drops.
struct CloseContainer {
    PlayerKey player;
    FixedVec drop_position;
    FixedVec drop_velocity;
    bool operator==(const CloseContainer&) const = default;
};

using CommandBody = std::variant<SetBlock, UseBlock, InsertItem, SpawnItem, RemoveEntity, AddTicket, RemoveTicket,
                                 PlayerUse, PlayerBreak, SetPlayerSlot, PickUp, DropItem, ContainerClick, CloseContainer>;

// Admission order is the recorded decision that orders inputs from different
// sources. Network arrival timing on a particular thread never orders commands.
struct Command {
    std::uint64_t admission{};
    CommandBody body;
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
    Result<std::uint64_t> admit(CommandBody body) {
        if (pending_.size() >= capacity_) return std::unexpected(ErrorCode::capacity_exceeded);
        if (next_ == UINT64_MAX) return std::unexpected(ErrorCode::counter_overflow);
        const auto id = next_++;
        pending_.emplace(id, std::move(body));
        return id;
    }
    bool cancel(std::uint64_t admission) { return pending_.erase(admission) == 1; }
    [[nodiscard]] std::size_t size() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t next_admission() const noexcept { return next_; }
    std::vector<Command> drain() {
        std::vector<Command> out;
        out.reserve(pending_.size());
        for (auto& [id, body] : pending_) out.push_back({id, std::move(body)});
        pending_.clear();
        return out;
    }

private:
    std::size_t capacity_;
    std::uint64_t next_;
    std::map<std::uint64_t, CommandBody> pending_;
};

} // namespace circuit::contracts
