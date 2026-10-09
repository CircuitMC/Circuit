// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/version.hpp>
#include <array>

namespace circuit::contracts {
namespace {
using enum CompatibilityTier;
using enum EvidenceSource;

// Compatibility table for circuit-reference-rules-v7. Every row claims a tier.
// A row claims L0 only when harness fixtures recorded on the target version
// pass and no gap is known; "Not covered" names aspects no fixture exercises.
// Every other row claims L2 and lists its gaps, which include behavior not yet
// compared with the target version. Fixture names refer to
// tests/fixtures/harness/.
constexpr std::array reference_v4_mechanisms{
    MechanismEntry{"command_timing", l0_exact, l0_exact, harness_trace,
        "Inputs apply between ticks: delays they start count from the previous tick (lamp_timing, "
        "vertical_slice, comparator_signal)."},
    MechanismEntry{"neighbor_updates", l0_exact, l0_exact, harness_trace,
        "Neighbor updates (west, east, down, up, north, south) and shape updates (west, east, north, south, "
        "down, up) are queued entries run depth-first in emission order; dust updates the neighbors of a "
        "7-position set in hash order and gives diagonal dust shape updates; a lever updates around itself "
        "and around the block it hangs on; removal and placement reactions precede the neighbor and shape "
        "updates of a change (vertical_slice, chain_limit, piston_observed, lever_attachment, dust_steps, "
        "support). Not covered: removal updates of dust that a piston destroys."},
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
    MechanismEntry{"redstone_wire", l0_exact, l0_exact, harness_trace,
        "Power decays by one per block and climbs or descends a step unless a conductor cuts it; dust reads "
        "power that sources give conductors strongly, powers the block below it and the blocks it points "
        "into, and keeps its shape as stored state, dots included; a change updates the neighbors of a "
        "7-position set in hash order (vertical_slice, chain_limit, strong_power, weak_power, dust_shape, "
        "dust_steps). Players' placement and dot toggling were compared with the official server through a "
        "protocol client. Not covered: dust on hoppers."},
    MechanismEntry{"repeater", l0_exact, l0_exact, harness_trace,
        "Delay 2-8 game ticks, pulse extension, quiet state change and front updates; strong power into the "
        "block in front; locked while a powered diode faces into either side (vertical_slice, observer_pulse, "
        "tick_priority, diode_priority, strong_power, repeater_lock)."},
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
    MechanismEntry{"hopper", l0_exact, l0_exact, harness_trace,
        "Push then pull, 8-tick cooldown, disabled while powered, item pickup, competition in block-entity order; "
        "a hopper receiving into an empty inventory from another hopper waits 8 game ticks, or 7 when it "
        "already ticked in that tick (hopper_chain, hopper_competition, double_chest, hopper_chained). Not "
        "covered: pickup of item entities that a trace cannot observe (circuit-trace v1)."},
    MechanismEntry{"double_chest", l0_exact, l0_exact, harness_trace,
        "A left half pairs clockwise of its facing; a half claims a same-facing single chest; pairing changes "
        "are shape-only; hoppers and comparators see the right half's slots first (double_chest, "
        "double_chest_left_input)."},
    MechanismEntry{"comparator", l0_exact, l0_exact, harness_trace,
        "Compare and subtract, 2-tick delay, output in a block entity, container reading also through "
        "conductors, rear power through conductors, side inputs without it, immediate refresh on a mode change "
        "(comparator_signal, comparator_priority, diode_priority, double_chest, weak_power). Not covered: a "
        "container behind a conductor replacing a weaker rear signal. Containers are the only analog source "
        "in the registry."},
    MechanismEntry{"water", l0_exact, l0_exact, harness_trace,
        "5-tick fluid ticks; distance-limited spread in the order north, south, west, east, only toward the "
        "nearest drops within reach when there are any; a new source between two sources on solid ground; "
        "dust, levers, repeaters and comparators washed away with their drops (water_channel, water_drop, "
        "water_washes). Not covered: sources above flowing water and three sources beside falling water."},
    MechanismEntry{"random_ticks", l1_region_equivalent, l2_declared_deviation, documented_behavior,
        "Speed from the game rule per section; sections without blocks that react are skipped, as in the target "
        "version; positions and rule draws use a stream derived from (seed, tick, chunk, section) instead of a "
        "level-wide stream. Gaps: chunks run in coordinate order instead of a "
        "shuffled order; not compared with the target version."},
    MechanismEntry{"wheat", l0_exact, l2_declared_deviation, documented_behavior,
        "Gap: grows with probability 1/3 per random tick, ignoring light, hydration and support."},
    MechanismEntry{"item_entity", l0_exact, l2_declared_deviation, documented_behavior,
        "Fixed-point gravity, drag and axis collision; despawns after 6000 ticks; pushed by moving blocks. Ids "
        "of entities spawned outside the command phase are assigned in serial order. Gaps: no merging; cannot "
        "enter unloaded chunks; physics not compared with the target version."},
    MechanismEntry{"chunk_tickets", l0_exact, l2_declared_deviation, documented_behavior,
        "A ticket of radius r makes chunks within Chebyshev distance r ticking and loads the chunks within "
        "max(r + 1, its load radius) without ticking (border level); players' load radii are their view distances. "
        "Gaps: no ticket types or level propagation; unloaded chunks keep pending ticks with absolute due "
        "times."},
    MechanismEntry{"entity_order", l0_exact, l2_declared_deviation, documented_behavior,
        "Entities tick in ascending id order once per tick. Gap: not compared with the target version "
        "(circuit-trace v1 has no entity observations)."},
    MechanismEntry{"block_entity_order", l0_exact, l0_exact, harness_trace,
        "Block entities tick in creation order; moving pistons finish in creation order (piston_slime, "
        "piston_observed) and competing hoppers act in placement order (hopper_competition). Not covered: "
        "order after a chunk reload."},
    MechanismEntry{"support", l0_exact, l0_exact, harness_trace,
        "Dust needs a whole top face or a hopper below it, repeaters and comparators a whole top face, levers "
        "a whole face to hang on; an update that finds the support gone breaks them with their drops "
        "(support, lever_attachment). The drops' positions follow item_entity."},
    MechanismEntry{"container_menus", l0_exact, l2_declared_deviation, controlled_experiment,
        "Player windows and chest, double chest and hopper menus with every click mode, applied as commands. "
        "A 48-step client scenario (every click mode, double chests, hoppers, the own window, throws and "
        "pickups) gives the same windows, carried stacks and inventories as the official 1.21.8 and 1.21.4 "
        "servers. Gaps: no crafting (the result slot stays empty); no harness traces of menus."},
    MechanismEntry{"survival_actions", l0_exact, l2_declared_deviation, documented_behavior,
        "Placement uses up items; dig progress from hardness, tool speed and tier, ground and water; drops by "
        "tool (stone gives cobblestone), container contents and tool wear; pickup by box with the target "
        "version's slot order and pickup delays (10 for block drops, 40 for thrown items); a dead player's "
        "inventory drops in the target version's order. Gaps: thrown items have no random spread and death "
        "drops spread in a fixed pattern; loot is limited to the modeled blocks; no crafting. Health, hunger "
        "and movement are checked by the server, not by the rules."},
};

constexpr RuleLimits vanilla_like_limits{
    .max_chained_neighbor_updates = 1'000'000,
    .max_scheduled_ticks_per_list = 65'536,
    .random_tick_speed = 3,
    .max_block_events_per_tick = 1'000'000,
    .max_ticket_radius = 8,
    .max_load_radius = 33, // a view distance of 32, plus one
};

// Both supported versions run the same reference rules: the 27 harness traces
// recorded on 1.21.4 are identical to those recorded on 1.21.8.
constexpr RuleSet reference_rules{"circuit-reference-rules-v7", RngPolicy::per_section_derived_stream, reference_v4_mechanisms,
                                  "circuit-blocks-1"};

constexpr std::array bundles{
    VersionBundle{
        .id = "java-1.21.8",
        .game_version = "1.21.8",
        .protocol_candidate = "java-1.21.8",
        .rules = reference_rules,
        .world = {-64, 384},
        .limits = vanilla_like_limits,
        .generator = "empty",
        .protocol = 772,
        .data_version = 4440,
        .save_conversion = true,
        .play_supported = true,
    },
    VersionBundle{
        .id = "java-1.21.4",
        .game_version = "1.21.4",
        .protocol_candidate = "java-1.21.4",
        .rules = reference_rules,
        .world = {-64, 384},
        .limits = vanilla_like_limits,
        .generator = "empty",
        .protocol = 769,
        .data_version = 4189,
        .save_conversion = false,
        .play_supported = true,
    },
    VersionBundle{
        .id = "java-1.20.4",
        .game_version = "1.20.4",
        .protocol_candidate = "java-1.20.4",
        .rules = std::nullopt,
        .world = {-64, 384},
        .limits = vanilla_like_limits,
        .generator = "none",
        .protocol = 0,
        .data_version = 0,
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
