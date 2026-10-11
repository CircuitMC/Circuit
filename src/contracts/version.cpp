// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/version.hpp>
#include <array>

namespace circuit::contracts {
namespace {
using enum CompatibilityTier;
using enum EvidenceSource;

// Compatibility table for circuit-reference-rules-v10. Every row claims a tier.
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
        "Grows only with light 9 or more at its own cell. Gap: grows with probability 1/3 per random tick, ignoring "
        "hydration and support."},
    MechanismEntry{"item_entity", l0_exact, l2_declared_deviation, documented_behavior,
        "Fixed-point gravity, drag and axis collision; despawns after 6000 ticks; pushed by moving blocks. Ids "
        "of entities spawned outside the command phase are assigned in serial order. Gaps: no merging; cannot "
        "enter unloaded chunks; physics not compared with the target version."},
    MechanismEntry{"chunk_tickets", l0_exact, l2_declared_deviation, harness_trace,
        "Typed tickets with a level, a square and a lifetime; load and simulation levels decide each chunk's "
        "status and what runs; changes take effect in each level's chunk stage after its scheduled ticks; "
        "dropped chunks keep their pending ticks' remaining delay and resume two ticks after a reload "
        "(ring_blocks, unload_reload). Gaps: new chunks load at once, dropped chunks unload at once."},
    MechanismEntry{"light", l0_exact, l0_exact, harness_trace,
        "Block light and sky light from each state's emission, dampening and occlusion shape, following the "
        "blocks as they stood at the end of the previous tick (light_spread, sky_light, light_shapes, "
        "light_timing). Not covered: columns at the edge of the loaded area."},
    MechanismEntry{"game_rules", l0_exact, l2_declared_deviation, controlled_experiment,
        "The version's game rules are world state, set by a logged command within the bounds both official "
        "servers enforce (booleans 0 or 1, spawnChunkRadius 0-32, other integers unbounded); random ticks "
        "read randomTickSpeed. /gamerule gave the official replies for 45 commands on both versions. Gaps: the "
        "other rules are kept but change nothing; compared through a protocol client, not harness traces."},
    MechanismEntry{"day_time", l0_exact, l2_declared_deviation, controlled_experiment,
        "The world's day time advances by one after each tick's commands while doDaylightCycle is on; a "
        "logged command sets it. 59 /time commands gave the official replies and set_time packets on both "
        "versions. Gaps: nothing reads the time of day yet (daylight detectors, sleeping, mobs); compared "
        "through a protocol client, not harness traces."},
    MechanismEntry{"weather", l1_region_equivalent, l2_declared_deviation, controlled_experiment,
        "Rain and thunder flags with their timers and the clear timer, advanced after the commands while "
        "doWeatherCycle is on, and levels following the flags by 0.01 a tick in single precision; /weather as "
        "a logged command. 16 /weather commands gave 1,348 identical replies and events on both versions. "
        "Gaps: durations come from Circuit's own stream, not the level's random sequence; no lightning, snow, "
        "ice or other effects of rain; compared through a protocol client, not harness traces."},
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
        "servers. With data packs the own window's 2x2 grid crafts: a 48-step scenario (results, every way "
        "of taking them, shapes, remainders, transmuting) matches both official servers. Gaps: no crafting "
        "table, special recipes or recipe book; no harness traces of menus."},
    MechanismEntry{"survival_actions", l0_exact, l2_declared_deviation, documented_behavior,
        "Placement uses up items; dig progress from hardness, tool speed and tier, ground and water; drops by "
        "tool (stone gives cobblestone), container contents and tool wear; pickup by box with the target "
        "version's slot order and pickup delays (10 for block drops, 40 for thrown items); a dead player's "
        "inventory drops in the target version's order. Gaps: thrown items have no random spread and death "
        "drops spread in a fixed pattern; modeled blocks have fixed drops, and blocks without rules drop from "
        "their data-pack loot tables only where these draw no random numbers. Health, hunger and movement "
        "are checked by the server, not by the rules."},
};

constexpr RuleLimits vanilla_like_limits{
    .max_chained_neighbor_updates = 1'000'000,
    .max_scheduled_ticks_per_list = 65'536,
    .max_block_events_per_tick = 1'000'000,
    .max_ticket_radius = 32, // a view distance of 32
    .max_load_radius = 34,   // and the two levels beyond it that are loaded
    .max_simulation_distance = 8,
};

// Game rules of each release, from minecraft.wiki "Game rule" (revision
// 3099622, August 2025; the Java Edition rules and their history): 1.21.5
// added allowFireTicksAwayFromPlayer and tntExplodes, 1.21.6 locatorBar.
// minecartMaxSpeed exists with the minecart improvements experiment. The
// official servers' /gamerule command trees list the same 55 (1.21.8) and
// 52 (1.21.4) rules besides it (observed 2026-10-10).
constexpr std::array game_rules_1_21_8{
    GameRule{"allowFireTicksAwayFromPlayer", GameRuleType::boolean, 0, false},
    GameRule{"announceAdvancements", GameRuleType::boolean, 1, false},
    GameRule{"blockExplosionDropDecay", GameRuleType::boolean, 1, false},
    GameRule{"commandBlockOutput", GameRuleType::boolean, 1, false},
    GameRule{"commandModificationBlockLimit", GameRuleType::integer, 32768, false},
    GameRule{"disableElytraMovementCheck", GameRuleType::boolean, 0, false},
    GameRule{"disablePlayerMovementCheck", GameRuleType::boolean, 0, false},
    GameRule{"disableRaids", GameRuleType::boolean, 0, false},
    GameRule{"doDaylightCycle", GameRuleType::boolean, 1, false},
    GameRule{"doEntityDrops", GameRuleType::boolean, 1, false},
    GameRule{"doFireTick", GameRuleType::boolean, 1, false},
    GameRule{"doImmediateRespawn", GameRuleType::boolean, 0, false},
    GameRule{"doInsomnia", GameRuleType::boolean, 1, false},
    GameRule{"doLimitedCrafting", GameRuleType::boolean, 0, false},
    GameRule{"doMobLoot", GameRuleType::boolean, 1, false},
    GameRule{"doMobSpawning", GameRuleType::boolean, 1, false},
    GameRule{"doPatrolSpawning", GameRuleType::boolean, 1, false},
    GameRule{"doTileDrops", GameRuleType::boolean, 1, false},
    GameRule{"doTraderSpawning", GameRuleType::boolean, 1, false},
    GameRule{"doVinesSpread", GameRuleType::boolean, 1, false},
    GameRule{"doWardenSpawning", GameRuleType::boolean, 1, false},
    GameRule{"doWeatherCycle", GameRuleType::boolean, 1, false},
    GameRule{"drowningDamage", GameRuleType::boolean, 1, false},
    GameRule{"enderPearlsVanishOnDeath", GameRuleType::boolean, 1, false},
    GameRule{"fallDamage", GameRuleType::boolean, 1, false},
    GameRule{"fireDamage", GameRuleType::boolean, 1, false},
    GameRule{"forgiveDeadPlayers", GameRuleType::boolean, 1, false},
    GameRule{"freezeDamage", GameRuleType::boolean, 1, false},
    GameRule{"globalSoundEvents", GameRuleType::boolean, 1, false},
    GameRule{"keepInventory", GameRuleType::boolean, 0, false},
    GameRule{"lavaSourceConversion", GameRuleType::boolean, 0, false},
    GameRule{"locatorBar", GameRuleType::boolean, 1, false},
    GameRule{"logAdminCommands", GameRuleType::boolean, 1, false},
    GameRule{"maxCommandChainLength", GameRuleType::integer, 65536, false},
    GameRule{"maxCommandForkCount", GameRuleType::integer, 65536, false},
    GameRule{"maxEntityCramming", GameRuleType::integer, 24, false},
    GameRule{"minecartMaxSpeed", GameRuleType::integer, 8, true},
    GameRule{"mobExplosionDropDecay", GameRuleType::boolean, 1, false},
    GameRule{"mobGriefing", GameRuleType::boolean, 1, false},
    GameRule{"naturalRegeneration", GameRuleType::boolean, 1, false},
    GameRule{"playersNetherPortalCreativeDelay", GameRuleType::integer, 0, false},
    GameRule{"playersNetherPortalDefaultDelay", GameRuleType::integer, 80, false},
    GameRule{"playersSleepingPercentage", GameRuleType::integer, 100, false},
    GameRule{"projectilesCanBreakBlocks", GameRuleType::boolean, 1, false},
    GameRule{"randomTickSpeed", GameRuleType::integer, 3, false},
    GameRule{"reducedDebugInfo", GameRuleType::boolean, 0, false},
    GameRule{"sendCommandFeedback", GameRuleType::boolean, 1, false},
    GameRule{"showDeathMessages", GameRuleType::boolean, 1, false},
    GameRule{"snowAccumulationHeight", GameRuleType::integer, 1, false},
    GameRule{"spawnChunkRadius", GameRuleType::integer, 2, false},
    GameRule{"spawnRadius", GameRuleType::integer, 10, false},
    GameRule{"spectatorsGenerateChunks", GameRuleType::boolean, 1, false},
    GameRule{"tntExplodes", GameRuleType::boolean, 1, false},
    GameRule{"tntExplosionDropDecay", GameRuleType::boolean, 0, false},
    GameRule{"universalAnger", GameRuleType::boolean, 0, false},
    GameRule{"waterSourceConversion", GameRuleType::boolean, 1, false},
};
constexpr std::array game_rules_1_21_4{
    GameRule{"announceAdvancements", GameRuleType::boolean, 1, false},
    GameRule{"blockExplosionDropDecay", GameRuleType::boolean, 1, false},
    GameRule{"commandBlockOutput", GameRuleType::boolean, 1, false},
    GameRule{"commandModificationBlockLimit", GameRuleType::integer, 32768, false},
    GameRule{"disableElytraMovementCheck", GameRuleType::boolean, 0, false},
    GameRule{"disablePlayerMovementCheck", GameRuleType::boolean, 0, false},
    GameRule{"disableRaids", GameRuleType::boolean, 0, false},
    GameRule{"doDaylightCycle", GameRuleType::boolean, 1, false},
    GameRule{"doEntityDrops", GameRuleType::boolean, 1, false},
    GameRule{"doFireTick", GameRuleType::boolean, 1, false},
    GameRule{"doImmediateRespawn", GameRuleType::boolean, 0, false},
    GameRule{"doInsomnia", GameRuleType::boolean, 1, false},
    GameRule{"doLimitedCrafting", GameRuleType::boolean, 0, false},
    GameRule{"doMobLoot", GameRuleType::boolean, 1, false},
    GameRule{"doMobSpawning", GameRuleType::boolean, 1, false},
    GameRule{"doPatrolSpawning", GameRuleType::boolean, 1, false},
    GameRule{"doTileDrops", GameRuleType::boolean, 1, false},
    GameRule{"doTraderSpawning", GameRuleType::boolean, 1, false},
    GameRule{"doVinesSpread", GameRuleType::boolean, 1, false},
    GameRule{"doWardenSpawning", GameRuleType::boolean, 1, false},
    GameRule{"doWeatherCycle", GameRuleType::boolean, 1, false},
    GameRule{"drowningDamage", GameRuleType::boolean, 1, false},
    GameRule{"enderPearlsVanishOnDeath", GameRuleType::boolean, 1, false},
    GameRule{"fallDamage", GameRuleType::boolean, 1, false},
    GameRule{"fireDamage", GameRuleType::boolean, 1, false},
    GameRule{"forgiveDeadPlayers", GameRuleType::boolean, 1, false},
    GameRule{"freezeDamage", GameRuleType::boolean, 1, false},
    GameRule{"globalSoundEvents", GameRuleType::boolean, 1, false},
    GameRule{"keepInventory", GameRuleType::boolean, 0, false},
    GameRule{"lavaSourceConversion", GameRuleType::boolean, 0, false},
    GameRule{"logAdminCommands", GameRuleType::boolean, 1, false},
    GameRule{"maxCommandChainLength", GameRuleType::integer, 65536, false},
    GameRule{"maxCommandForkCount", GameRuleType::integer, 65536, false},
    GameRule{"maxEntityCramming", GameRuleType::integer, 24, false},
    GameRule{"minecartMaxSpeed", GameRuleType::integer, 8, true},
    GameRule{"mobExplosionDropDecay", GameRuleType::boolean, 1, false},
    GameRule{"mobGriefing", GameRuleType::boolean, 1, false},
    GameRule{"naturalRegeneration", GameRuleType::boolean, 1, false},
    GameRule{"playersNetherPortalCreativeDelay", GameRuleType::integer, 0, false},
    GameRule{"playersNetherPortalDefaultDelay", GameRuleType::integer, 80, false},
    GameRule{"playersSleepingPercentage", GameRuleType::integer, 100, false},
    GameRule{"projectilesCanBreakBlocks", GameRuleType::boolean, 1, false},
    GameRule{"randomTickSpeed", GameRuleType::integer, 3, false},
    GameRule{"reducedDebugInfo", GameRuleType::boolean, 0, false},
    GameRule{"sendCommandFeedback", GameRuleType::boolean, 1, false},
    GameRule{"showDeathMessages", GameRuleType::boolean, 1, false},
    GameRule{"snowAccumulationHeight", GameRuleType::integer, 1, false},
    GameRule{"spawnChunkRadius", GameRuleType::integer, 2, false},
    GameRule{"spawnRadius", GameRuleType::integer, 10, false},
    GameRule{"spectatorsGenerateChunks", GameRuleType::boolean, 1, false},
    GameRule{"tntExplosionDropDecay", GameRuleType::boolean, 0, false},
    GameRule{"universalAnger", GameRuleType::boolean, 0, false},
    GameRule{"waterSourceConversion", GameRuleType::boolean, 1, false},
};

// Both supported versions run the same reference rules, each by evidence of
// its own: the rules were checked against harness traces of 1.21.8, and the
// same scenarios recorded on 1.21.4 are identical (evidence_tests compares
// every pair). What differs is the bundles' own: content, component schema,
// registries and game rules, and where random ticks run (RandomTickRange),
// which the official servers showed differs.
constexpr RuleSet rules_1_21_8{"circuit-reference-rules-v10", RngPolicy::per_section_derived_stream, reference_v4_mechanisms,
                               "circuit-blocks-1", "harness traces of 1.21.8 (tests/fixtures/harness)", RandomTickRange::entity_ticking};
constexpr RuleSet rules_1_21_4{"circuit-reference-rules-v10", RngPolicy::per_section_derived_stream, reference_v4_mechanisms,
                               "circuit-blocks-1",
                               "harness traces of 1.21.4 (tests/fixtures/harness-1.21.4), each identical to its 1.21.8 recording; "
                               "random ticks run only near players (observed on the official 1.21.4 server)",
                               RandomTickRange::block_ticking_near_player};

constexpr std::array bundles{
    VersionBundle{
        .id = "java-1.21.8",
        .game_version = "1.21.8",
        .protocol_candidate = "java-1.21.8",
        .rules = rules_1_21_8,
        .schema = {.content_release = "1.21.8", .component_schema = 772, .data_pack_format = 81, .resource_pack_format = 64,
                   .registries = "java-1.21.8",
                   .server_jar_sha1 = "6bce4ef400e4efaa63a13d5e6f6b500be969ef81", .game_rules = game_rules_1_21_8},
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
        .rules = rules_1_21_4,
        .schema = {.content_release = "1.21.4", .component_schema = 769, .data_pack_format = 61, .resource_pack_format = 46,
                   .registries = "java-1.21.4",
                   .server_jar_sha1 = "4707d00eb834b446575d89a61a11b5d548d8c001", .game_rules = game_rules_1_21_4},
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
        .schema = {},
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
