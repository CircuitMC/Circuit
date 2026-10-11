// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <compare>
#include <cstdint>
#include <string_view>

namespace circuit::contracts {

// Logical game time. It advances once per executed tick and never by wall clock.
using Tick = std::uint64_t;

// Reference phase order of one tick. Within a phase, work is a sequence of
// roots with a phase-specific total order; immediate reactions run inside
// their root. The executor must reproduce this order independently of threads.
enum class Phase : std::uint8_t {
    commands,       // admitted external inputs; observes the previous tick's time
    block_ticks,    // scheduled block ticks: (due, priority, sub-order)
    fluid_ticks,    // scheduled fluid ticks: (due, priority, sub-order)
    chunks,         // ticket expiry and chunk-level changes of the level
    random_ticks,   // per ticking chunk and section
    block_events,   // level-wide FIFO, drained including events added while draining
    entities,       // ascending entity id
    block_entities, // ascending block-entity creation order
};
inline constexpr std::array<Phase, 8> phase_order{
    Phase::commands, Phase::block_ticks, Phase::fluid_ticks, Phase::chunks, Phase::random_ticks,
    Phase::block_events, Phase::entities, Phase::block_entities};

constexpr std::string_view phase_name(Phase p) noexcept {
    switch (p) {
    case Phase::commands: return "commands";
    case Phase::block_ticks: return "block_ticks";
    case Phase::fluid_ticks: return "fluid_ticks";
    case Phase::chunks: return "chunks";
    case Phase::random_ticks: return "random_ticks";
    case Phase::block_events: return "block_events";
    case Phase::entities: return "entities";
    case Phase::block_entities: return "block_entities";
    }
    return "unknown";
}

enum class TickList : std::uint8_t { block, fluid };

// Position of an observable action in the serial reference order: tick,
// level, phase, root index within the phase, and action index within the
// root. The command phase runs once, before any level ticks; it counts as the
// first level's (the overworld's). The other phases run level by level in
// level order (DimensionId).
struct OrderKey {
    Tick tick{};
    Phase phase{};
    std::uint64_t root{};
    std::uint32_t seq{};
    std::uint8_t level{}; // DimensionId of the level whose phase this is
    bool operator==(const OrderKey&) const = default;
    constexpr std::strong_ordering operator<=>(const OrderKey& o) const noexcept {
        if (const auto c = tick <=> o.tick; c != 0) return c;
        if (const auto c = level <=> o.level; c != 0) return c;
        if (const auto c = phase <=> o.phase; c != 0) return c;
        if (const auto c = root <=> o.root; c != 0) return c;
        return seq <=> o.seq;
    }
};

} // namespace circuit::contracts
