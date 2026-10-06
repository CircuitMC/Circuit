// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/version.hpp>
#include <array>

namespace circuit::contracts {
namespace {
using enum CompatibilityTier;
using enum EvidenceSource;

// Compatibility table for circuit-reference-rules-v4. Every row claims a tier.
// A row claims L0 only when harness fixtures recorded on the target version
// pass and no gap is known; "Not covered" names aspects no fixture exercises.
// Every other row claims L2 and lists its gaps, which include behavior not yet
// compared with the target version. Fixture names refer to
// tests/fixtures/harness/.
constexpr std::array reference_v4_mechanisms{
    MechanismEntry{"command_timing", l0_exact, l0_exact, harness_trace,
        "Inputs apply between ticks: delays they start count from the previous tick (lamp_timing, "
        "vertical_slice, comparator_signal)."},
    MechanismEntry{"neighbor_updates", l0_exact, l2_declared_deviation, harness_trace,
        "Neighbor updates (west, east, down, up, north, south) and shape updates (west, east, north, south, "
        "down, up) are queued entries run depth-first in emission order; dust updates the neighbors of a "
        "7-position set in hash order (vertical_slice, chain_limit, piston_observed). Gaps: levers do not "
        "update the block they are attached to; indirect (diagonal) shape updates are not modeled."},
    MechanismEntry{"neighbor_chain_limit", l0_exact, l0_exact, harness_trace,
        "A chain counts the entries queued in it (neighbor-update sets, single updates and shape updates); at "
        "the limit new entries are dropped while queued entries still run (chain_limit). Not covered: the "
        "shape-update nesting limit of 512."},
    MechanismEntry{"scheduled_ticks", l0_exact, l0_exact, harness_trace,
        "Order (due, priority, sub-order); a drained tick counts as scheduled until it runs (observer_clock); "
        "priorities: repeaters -3 when feeding a diode that does not face back, -2 when turning off or ending "
        "an extended pulse, else -1; comparators -1 when feeding a diode, else 0; lamps and observers 0 "
        "(tick_priority, comparator_priority, diode_priority). Not covered: the per-tick drain limit and ticks "
        "in non-ticking chunks."},
    MechanismEntry{"redstone_wire", l0_exact, l2_declared_deviation, harness_trace,
        "Power decays by one per block; a change updates the neighbors of a 7-position set in hash order "
        "(vertical_slice, chain_limit). Gaps: no connections up or down steps; no power through solid blocks."},
    MechanismEntry{"repeater", l0_exact, l2_declared_deviation, harness_trace,
        "Delay 2-8 game ticks, pulse extension, quiet state change and front updates (vertical_slice, "
        "observer_pulse, tick_priority, diode_priority). Gaps: no locking; no power through solid blocks."},
    MechanismEntry{"redstone_lamp", l0_exact, l0_exact, harness_trace,
        "Lights at once and turns off 4 game ticks after losing power, without neighbor updates (lamp_timing, "
        "vertical_slice, tick_priority)."},
    MechanismEntry{"observer", l0_exact, l0_exact, harness_trace,
        "Fires 2 ticks after a shape update from its observed side, including piston writes, and after being "
        "moved by a piston; 2-tick pulse; output updates at its back (observer_pulse, observer_clock, "
        "observer_moved, piston_observed). Not covered: observers moved while powered."},
    MechanismEntry{"piston", l0_exact, l2_declared_deviation, harness_trace,
        "Two-tick moves through moving pistons; structure discovery and placement order, vacated cells in hash "
        "order, shape updates after every write, block dropping, destruction (piston_timing, piston_slime, "
        "piston_destroys, piston_observed, quasi_connectivity). Gaps: drops at the block center without "
        "random offsets; items pushed half a block per step; neighbor updates after a move in first-change "
        "order are not compared."},
    MechanismEntry{"block_events", l0_exact, l0_exact, harness_trace,
        "Level-wide FIFO with set semantics, drained including additions (piston_observed, piston_slime, "
        "piston_timing). Not covered: deferral in non-ticking chunks."},
    MechanismEntry{"hopper", l0_exact, l2_declared_deviation, harness_trace,
        "Push then pull, 8-tick cooldown, disabled while powered, item pickup, competition in block-entity order "
        "(hopper_chain, hopper_competition, double_chest). Gap: no chained-hopper cooldown adjustment."},
    MechanismEntry{"double_chest", l0_exact, l0_exact, harness_trace,
        "A left half pairs clockwise of its facing; a half claims a same-facing single chest; pairing changes "
        "are shape-only; hoppers and comparators see the right half's slots first (double_chest, "
        "double_chest_left_input)."},
    MechanismEntry{"comparator", l0_exact, l2_declared_deviation, harness_trace,
        "Compare and subtract, 2-tick delay, output in a block entity, container reading also through stone, "
        "obsidian and lamps, side inputs, immediate refresh on a mode change (comparator_signal, "
        "comparator_priority, diode_priority, double_chest). Gaps: no rear power through solid blocks; "
        "containers are the only analog source in the registry."},
    MechanismEntry{"water", l0_exact, l2_declared_deviation, harness_trace,
        "5-tick fluid ticks and distance-limited spread match water_channel. Gap: spreads to every side without "
        "the target version's preference for nearby drops."},
    MechanismEntry{"random_ticks", l1_region_equivalent, l2_declared_deviation, documented_behavior,
        "Speed from the game rule per section; positions and rule draws use a stream derived from (seed, tick, "
        "chunk, section) instead of a level-wide stream. Gaps: chunks run in coordinate order instead of a "
        "shuffled order; not compared with the target version."},
    MechanismEntry{"wheat", l0_exact, l2_declared_deviation, documented_behavior,
        "Gap: grows with probability 1/3 per random tick, ignoring light, hydration and support."},
    MechanismEntry{"item_entity", l0_exact, l2_declared_deviation, documented_behavior,
        "Fixed-point gravity, drag and axis collision; despawns after 6000 ticks; pushed by moving blocks. Ids "
        "of entities spawned outside the command phase are assigned in serial order. Gaps: no merging; cannot "
        "enter unloaded chunks; physics not compared with the target version."},
    MechanismEntry{"chunk_tickets", l0_exact, l2_declared_deviation, documented_behavior,
        "A ticket of radius r makes chunks within Chebyshev distance r ticking and the next ring border-loaded. "
        "Gaps: no ticket types or level propagation; unloaded chunks keep pending ticks with absolute due "
        "times."},
    MechanismEntry{"entity_order", l0_exact, l2_declared_deviation, documented_behavior,
        "Entities tick in ascending id order once per tick. Gap: not compared with the target version "
        "(circuit-trace v1 has no entity observations)."},
    MechanismEntry{"block_entity_order", l0_exact, l0_exact, harness_trace,
        "Block entities tick in creation order; moving pistons finish in creation order (piston_slime, "
        "piston_observed) and competing hoppers act in placement order (hopper_competition). Not covered: "
        "order after a chunk reload."},
    MechanismEntry{"support", l0_exact, l2_declared_deviation, harness_trace,
        "Gap: not modeled; dust, repeaters, comparators and levers survive without support, while the target "
        "version removes them. Fixtures place a supporting floor."},
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
        .rules = RuleSet{"circuit-reference-rules-v4", RngPolicy::per_section_derived_stream,
                         reference_v4_mechanisms},
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
