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
struct AddTicket {
    std::uint32_t ticket{};
    ChunkPos center;
    std::uint8_t radius{};
    bool operator==(const AddTicket&) const = default;
};
struct RemoveTicket {
    std::uint32_t ticket{};
    bool operator==(const RemoveTicket&) const = default;
};

using CommandBody = std::variant<SetBlock, UseBlock, InsertItem, SpawnItem, RemoveEntity, AddTicket, RemoveTicket>;

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
    explicit CommandQueue(std::size_t capacity = 65536) : capacity_(capacity) {}
    Result<std::uint64_t> admit(CommandBody body) {
        if (pending_.size() >= capacity_) return std::unexpected(ErrorCode::capacity_exceeded);
        if (next_ == UINT64_MAX) return std::unexpected(ErrorCode::counter_overflow);
        const auto id = next_++;
        pending_.emplace(id, std::move(body));
        return id;
    }
    bool cancel(std::uint64_t admission) { return pending_.erase(admission) == 1; }
    [[nodiscard]] std::size_t size() const noexcept { return pending_.size(); }
    std::vector<Command> drain() {
        std::vector<Command> out;
        out.reserve(pending_.size());
        for (auto& [id, body] : pending_) out.push_back({id, std::move(body)});
        pending_.clear();
        return out;
    }

private:
    std::size_t capacity_;
    std::uint64_t next_{1};
    std::map<std::uint64_t, CommandBody> pending_;
};

} // namespace circuit::contracts
