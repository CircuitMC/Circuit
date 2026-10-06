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
    commands,       // admitted external inputs, then chunk-level changes; observes the previous tick's time
    block_ticks,    // scheduled block ticks: (due, priority, sub-order)
    fluid_ticks,    // scheduled fluid ticks: (due, priority, sub-order)
    random_ticks,   // per ticking chunk and section
    block_events,   // level-wide FIFO, drained including events added while draining
    entities,       // ascending entity id
    block_entities, // ascending block-entity creation order
};
inline constexpr std::array<Phase, 7> phase_order{
    Phase::commands, Phase::block_ticks, Phase::fluid_ticks, Phase::random_ticks,
    Phase::block_events, Phase::entities, Phase::block_entities};

constexpr std::string_view phase_name(Phase p) noexcept {
    switch (p) {
    case Phase::commands: return "commands";
    case Phase::block_ticks: return "block_ticks";
    case Phase::fluid_ticks: return "fluid_ticks";
    case Phase::random_ticks: return "random_ticks";
    case Phase::block_events: return "block_events";
    case Phase::entities: return "entities";
    case Phase::block_entities: return "block_entities";
    }
    return "unknown";
}

enum class TickList : std::uint8_t { block, fluid };

// Position of an observable action in the serial reference order:
// tick, phase, root index within the phase, and action index within the root.
struct OrderKey {
    Tick tick{};
    Phase phase{};
    std::uint64_t root{};
    std::uint32_t seq{};
    auto operator<=>(const OrderKey&) const = default;
};

} // namespace circuit::contracts
