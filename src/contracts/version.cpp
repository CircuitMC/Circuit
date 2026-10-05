// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/version.hpp>
#include <array>

namespace circuit::contracts {
namespace {
using enum CompatibilityTier;
using enum EvidenceSource;

// Compatibility table for circuit-reference-rules-v2. Nothing is checked
// against the target version yet: no row claims L0. L2 rows record known
// simplifications; rows without a claim are unverified.
constexpr std::array reference_v2_mechanisms{
    MechanismEntry{"neighbor_updates", l0_exact, std::nullopt, documented_behavior,
        "Order west, east, down, up, north, south; emissions during one delivery run depth-first in "
        "emission order. A single update kind serves both neighbor and shape updates. Unverified."},
    MechanismEntry{"neighbor_chain_limit", l0_exact, std::nullopt, documented_behavior,
        "A chain counts deliveries until its stack empties; reaching the limit discards the rest of "
        "the chain. Unverified."},
    MechanismEntry{"scheduled_ticks", l0_exact, std::nullopt, documented_behavior,
        "Order (due, priority, sub-order) with one level-wide sub-order counter shared by block and "
        "fluid ticks; at most one pending tick per (position, block). Ticks in unloaded or non-ticking "
        "chunks keep their keys. Priorities are Circuit-assigned."},
    MechanismEntry{"redstone_wire", l0_exact, l2_declared_deviation, documented_behavior,
        "Connects horizontally on one level only; emits its level in every direction except up; no "
        "power conduction through solid blocks."},
    MechanismEntry{"repeater", l0_exact, l2_declared_deviation, documented_behavior,
        "Delay 2-8 game ticks, pulse extension to its delay, priority -1; use cycles the delay; no locking."},
    MechanismEntry{"redstone_lamp", l0_exact, std::nullopt, documented_behavior,
        "Lights immediately; turns off 4 game ticks after losing power."},
    MechanismEntry{"observer", l0_exact, l2_declared_deviation, documented_behavior,
        "Reacts to any update from its observed position; 2-tick delay and 2-tick pulse."},
    MechanismEntry{"piston", l0_exact, l2_declared_deviation, documented_behavior,
        "Structures of at most 12 blocks, slime attachment and quasi-connectivity. A block event starts the "
        "move: the base extends at once, while the head and moved blocks, or on retraction the base and pulled "
        "blocks, become moving pistons that finish in the block-entity phase two ticks later. A sticky piston "
        "retracting before its push finished leaves the block behind. Dust, levers, repeaters, comparators, "
        "water and wheat are destroyed without drops; entities are not pushed. Phase placement and drop "
        "timing are unverified."},
    MechanismEntry{"block_events", l0_exact, std::nullopt, documented_behavior,
        "Level-wide FIFO with set semantics, drained including additions; events in non-ticking or "
        "unloaded chunks are deferred in order."},
    MechanismEntry{"hopper", l0_exact, l2_declared_deviation, documented_behavior,
        "Push then pull, 8-tick cooldown after any transfer, disabled while powered, picks up item "
        "entities in the cell above; no chained-hopper cooldown adjustments."},
    MechanismEntry{"double_chest", l0_exact, std::nullopt, documented_behavior,
        "Halves with the same facing pair as left (clockwise) and right (counter-clockwise). A half claims a "
        "single chest of the same facing at its partner position; otherwise it becomes single, also when its "
        "partner disappears. Hoppers and comparators see one 54-slot container, left half first. The pairing "
        "convention and slot order are unverified."},
    MechanismEntry{"comparator", l0_exact, l2_declared_deviation, documented_behavior,
        "Compare and subtract modes, 2-tick delay at priority 0, output held in a block entity. Rear input is "
        "power or the fullness of a container behind it, also through stone, obsidian or a lamp; side inputs "
        "come from dust, repeaters, comparators, redstone blocks and observers. No other analog sources."},
    MechanismEntry{"water", l0_exact, l2_declared_deviation, documented_behavior,
        "5-tick fluid ticks; spreads into air only, horizontally to every side without shortest-path "
        "preference; falling water from above."},
    MechanismEntry{"random_ticks", l1_region_equivalent, std::nullopt, documented_behavior,
        "Speed 3 per section; positions and rule draws use a stream derived from (seed, tick, chunk, "
        "section) instead of a level-wide stream; chunks in coordinate order instead of shuffled."},
    MechanismEntry{"wheat", l0_exact, l2_declared_deviation, documented_behavior,
        "Grows with probability 1/3 per random tick; ignores light, hydration and support."},
    MechanismEntry{"item_entity", l0_exact, l2_declared_deviation, documented_behavior,
        "Fixed-point gravity, drag and axis collision; despawns after 6000 ticks; no merging; cannot "
        "enter unloaded chunks."},
    MechanismEntry{"chunk_tickets", l0_exact, l2_declared_deviation, documented_behavior,
        "A ticket of radius r makes chunks within Chebyshev distance r ticking and the next ring "
        "border-loaded; no ticket types or level propagation."},
    MechanismEntry{"entity_order", l0_exact, std::nullopt, documented_behavior,
        "Entities tick in ascending id order once per tick."},
    MechanismEntry{"block_entity_order", l0_exact, std::nullopt, documented_behavior,
        "Block entities tick in creation order."},
};

constexpr RuleLimits vanilla_like_limits{
    .max_chained_neighbor_updates = 1'000'000,
    .max_scheduled_ticks_per_list = 65'536,
    .random_tick_speed = 3,
    .max_block_events_per_tick = 1'000'000,
    .max_ticket_radius = 8,
};

constexpr std::array bundles{
    VersionBundle{
        .id = "java-1.21.8",
        .game_version = "1.21.8",
        .protocol_candidate = "java-1.21.8",
        .rules = RuleSet{"circuit-reference-rules-v2", RngPolicy::per_section_derived_stream,
                         reference_v2_mechanisms},
        .world = {-64, 384},
        .limits = vanilla_like_limits,
        .generator = "empty",
        .save_conversion = false,
        .play_supported = false,
    },
    VersionBundle{
        .id = "java-1.20.4",
        .game_version = "1.20.4",
        .protocol_candidate = "java-1.20.4",
        .rules = std::nullopt,
        .world = {-64, 384},
        .limits = vanilla_like_limits,
        .generator = "none",
        .save_conversion = false,
        .play_supported = false,
    },
};
} // namespace

std::span<const VersionBundle> BundleRegistry::candidates() noexcept { return bundles; }

std::optional<const VersionBundle*> BundleRegistry::find(std::string_view id) noexcept {
    for (const auto& bundle : bundles)
        if (bundle.id == id) return &bundle;
    return std::nullopt;
}

Result<const VersionBundle*> BundleRegistry::select_for_simulation(std::string_view id) noexcept {
    const auto found = find(id);
    if (!found) return std::unexpected(ErrorCode::unknown_version);
    if (!(*found)->rules) return std::unexpected(ErrorCode::unsupported_version);
    return *found;
}

} // namespace circuit::contracts
