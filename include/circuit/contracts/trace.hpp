// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/contracts/time.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// Observable trace records. A record's OrderKey is its position in the serial
// reference order; two executions are equivalent only if their records are
// identical, including order. Payload fields a/b/c are defined per kind.
namespace circuit::contracts {

enum class TraceKind : std::uint8_t {
    command_applied = 1,     // a = admission
    command_rejected = 2,    // a = admission, b = ErrorCode
    block_changed = 3,       // pos, a = old state, b = new state
    tick_scheduled = 4,      // pos, a = list << 8 | block, b = due tick, c = priority + 128
    tick_executed = 5,       // pos, a = list << 8 | block, b = 1 if the block still matched
    block_event_queued = 6,  // pos, a = block, b = event, c = param
    block_event_executed = 7,// pos, a = block, b = event, c = 1 if the block still matched
    chain_truncated = 8,     // pos = source of the update that hit the limit, a = updates delivered
    inventory_changed = 9,   // pos, a = slot, b = old stack, c = new stack (item << 8 | count)
    entity_spawned = 10,     // pos = block position, a = entity id, b = item << 8 | count
    entity_changed_chunk = 11,// a = entity id, b = old chunk, c = new chunk (packed)
    entity_removed = 12,     // pos = block position, a = entity id, b = reason
    chunk_loaded = 13,       // pos = chunk origin, a = generation, b = level
    chunk_unloaded = 14,     // pos = chunk origin, a = generation
    chunk_level_changed = 15,// pos = chunk origin, a = old level, b = new level
    piston_blocked = 16,     // pos = piston
    entity_stack_changed = 17,// a = entity id, b = old stack, c = new stack
    signal_changed = 18,     // pos, a = old output signal, b = new output signal (block-entity state)
};

struct TraceRecord {
    OrderKey key;
    TraceKind kind{};
    BlockPos pos;
    std::uint64_t a{}, b{}, c{};
    bool operator==(const TraceRecord&) const = default;
};

constexpr std::uint64_t pack_chunk(ChunkPos c) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) << 32) | static_cast<std::uint32_t>(c.z);
}
constexpr std::uint64_t pack_stack(ItemStack s) noexcept {
    return (static_cast<std::uint64_t>(s.item) << 8) | s.count;
}

std::string describe(const TraceRecord& record);

// Index of the first record that differs, or nullopt if both are identical.
// A length difference reports the first index past the shorter sequence.
std::optional<std::size_t> first_divergence(std::span<const TraceRecord> a, std::span<const TraceRecord> b);

} // namespace circuit::contracts
