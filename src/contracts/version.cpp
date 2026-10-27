// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/version.hpp>
#include <array>

namespace circuit::contracts {
namespace {
using enum CompatibilityTier;
using enum EvidenceSource;

// Compatibility table for circuit-reference-rules-v12. Every row claims a tier.
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
        "dust_steps). Conductors are the blocks both official servers report for every state (full-cube collision "
        "with named exceptions; tests/fixtures/collision/conductor-*.txt). Players' placement and dot toggling were "
        "compared with the official server through a protocol client. Not covered: dust on hoppers."},
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
        "observer_moved, piston_observed); flying engines whose moved observers power the slime blocks under them "
        "(flying_engine, flying_engine_more). Not covered: observers moved while powered."},
    MechanismEntry{"piston", l0_exact, l2_declared_deviation, harness_trace,
        "Two-tick moves through moving pistons; structure discovery and placement order, vacated cells in hash "
        "order, shape updates after every write but a destruction, the moved blocks' neighbor updates before an extending "
        "base changes (block_event_order), block dropping, destruction (piston_timing, "
        "piston_slime, piston_destroys, piston_observed, quasi_connectivity, door_more); a draw from the level random after each "
        "extension and retraction (piston_draws); entities pushed out of the space a moving block sweeps "
        "through each step in every direction (0.01 beyond it, at most 0.51 a tick on each axis; tnt_duper_turns), "
        "moving blocks colliding where "
        "they stand, moving slime blocks launching entities (item_piston_push, item_drop_piston, "
        "piston_entities, slime_entities); honey blocks as sticky blocks that do not hold slime, carrying what stands "
        "on them sideways, and a head's removal taking its base (honey_basics, honey_more); every block moved, "
        "destroyed, pushed only or left in place as the official servers do, arriving without its water and "
        "breaking at once where it cannot stay (moved_arrivals); documented pulse circuits built on pistons "
        "(pulse_generators, pulse_limiters) "
        "(push_classes). Gaps: neighbor updates "
        "after a move in first-change "
        "order are not compared."},
    MechanismEntry{"block_events", l0_exact, l0_exact, harness_trace,
        "Level-wide FIFO with set semantics, drained including additions; events of chunks that do not tick blocks "
        "wait in place until they do (piston_observed, piston_slime, piston_timing, block_event_order, block_event_rings)."},
    MechanismEntry{"hopper", l0_exact, l0_exact, harness_trace,
        "Push then pull, 8-tick cooldown, disabled while powered, item pickup, competition in block-entity order; "
        "a hopper receiving into an empty inventory from another hopper waits 8 game ticks, or 7 when it "
        "already ticked in that tick (hopper_chain, hopper_competition, double_chest, hopper_chained); items "
        "collected from the cell above and the bowl, and taken when they move inside the hopper "
        "(item_hopper_pickup, item_hopper_area, item_hopper_inside); the documented basic item sorter sorting a "
        "mixed stream across a chunk edge and overflowing (item_sorter, item_sorter_overflow). Not covered: "
        "blocks above other than air, stone and containers."},
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
        "nearest drops within reach when there are any; a new source between two sources on solid ground "
        "while waterSourceConversion is on; dust, levers, buttons, redstone torches, repeaters and comparators "
        "washed away with their drops, and registry blocks that do not block motion and hold no water but portals "
        "and structure voids; waterlogged blocks give their water, as sources, to the sides their collision shapes "
        "leave open after shape updates and once water is poured into them (water_channel, water_drop, water_washes, "
        "water_torch_button, fluid_sources, fluid_washes, waterlogged_flow; every state in "
        "tests/fixtures/collision/flows-*.txt). Not covered: sources above flowing water and three sources beside falling water."},
    MechanismEntry{"loot", l0_exact, l2_declared_deviation, harness_trace,
        "Block loot tables from the data packs: pools, alternatives, groups and sequences, block state, tool, random "
        "chance, table bonus and location conditions, constant, uniform and binomial counts and fortune's ore, uniform "
        "and binomial bonuses, rolled from each table's random sequence (Xoroshiro128++ from the world seed and the MD5 "
        "of its id, as minecraft.wiki \"Random sequence\" describes; fitted to every roll of loot_many, and the states "
        "left equal the official saved ones); a stack of none still takes its position from the level random (loot_rolls, "
        "loot_single, loot_many; ores, leaves, gravel and other blocks broken by players holding tools with fortune, silk "
        "touch or shears in loot_tools and loot_bonus, crops with fortune in farmland; with the operator's data packs). Gaps: "
        "weighted choices between "
        "entries (no block table has them) drop nothing; "
        "tables that copy components or block states or name dynamic entries do not compile; each level keeps its own "
        "sequences, where the official servers keep one set for the world."},
    MechanismEntry{"lava", l0_exact, l0_exact, harness_trace,
        "Lava as a registry block: 30-tick fluid ticks; water's spread, losing 2 levels a block and looking 2 blocks ahead for "
        "drops, water below counting as one; sources only with lavaSourceConversion (water's with waterSourceConversion); "
        "touching water above or beside it, waterlogged blocks too, a source becomes obsidian and flowing lava cobblestone; "
        "flowing down into water it makes stone; on soul soil beside blue ice it becomes basalt; it destroys what water washes "
        "away without drops (lava_flow, lava_water, lava_edges, fluid_sources, fluid_washes). "
        "Not covered: the Nether's faster lava, fire, sounds, and entities in lava."},
    MechanismEntry{"random_ticks", l0_exact, l2_declared_deviation, harness_trace,
        "As the official 1.21.8 server, fitted to every pick of farmland layers: chunk by chunk in the order of an open "
        "hash table of their keys (chunk 0, 0 first), randomTickSpeed precipitation checks from the level random (each "
        "hit advancing the position generator), then randomTickSpeed positions per section holding blocks that take "
        "random ticks or water, from the bottom up, from the level's generator v -> 3v + 1013904223; which states take "
        "random ticks as both official servers report for every state (tests/fixtures/collision/random-ticks-*.txt); "
        "farmland dries, is wetted by water and turns to dirt unless a crop or stem keeps it (random_ticks_farmland, "
        "random_ticks_chunks, random_ticks_sections); crops, stems, grass blocks and mycelium (crops, spreading). "
        "Random ticks run serially. Gaps: the table's order for chunks that start ticking at different times is "
        "Circuit's model (fitted to forced areas); 1.21.4 runs them only near players, which the harness lacks, so its "
        "order is not compared; random ticks of other blocks (leaves, saplings, sugar cane, cacti, lava, copper and the "
        "rest) do nothing, where the official servers draw and change them; precipitation effects (snow, ice, "
        "cauldrons, lightning) are not modeled."},
    MechanismEntry{"crops", l0_exact, l2_declared_deviation, harness_trace,
        "As the official 1.21.8 server in random ticks (crops_growth, stems_growth): wheat, carrots, potatoes, beetroots, "
        "torchflower crops and melon and pumpkin stems grow with light 9 or more at their cell, by nextInt((int)(25 / f) "
        "+ 1) == 0 of the level random, f from the farmland under and around them (moist or dry) and halved by the same "
        "crop beside them in both directions or on a diagonal; beetroots and torchflower crops first skip one tick in "
        "three; a torchflower crop past age 1 becomes a torchflower; a ripe stem draws a side and sets its fruit there "
        "on air over farmland or dirt, becoming the attached stem, which stands ripe again when the fruit goes. An "
        "update that finds no farmland below breaks them with their drops (loot_rolls). A pitcher crop draws its growth, "
        "then grows with light 8 or the sky, from age 3 with its upper half (eggs_blossoms). Gaps: crops do not break in "
        "darkness; bone meal is not modeled; 1.21.4's growth is not compared (its random ticks run only near "
        "players)."},
    MechanismEntry{"spreading", l0_exact, l2_declared_deviation, harness_trace,
        "As the official 1.21.8 server in random ticks (grass_spread): a grass block or mycelium under a block that its "
        "light cannot pass (stone, bottom slabs and stairs, dirt paths, two snow layers; not glass, top slabs, leaves, "
        "carpets or one snow layer) or under water becomes dirt; with light 9 or more above it tries four positions in "
        "x +-1, y -3..+1, z +-1, three draws each, turning dirt that could hold it and has no water above into the same "
        "block, snowy under snow; nylium under such a block becomes netherrack (eggs_blossoms). Gaps: nylium, moss and "
        "sculk do not spread (bone meal); 1.21.4's spreading is not compared (its random ticks run only near "
        "players)."},
    MechanismEntry{"plants", l0_exact, l2_declared_deviation, harness_trace,
        "As the official 1.21.8 server in random ticks (cane_cactus_growth, wart_berry_cocoa_growth, kelp_vines_growth): "
        "sugar cane and cacti with air above age to 15 and then grow a block when less than three tall (sugar cane at "
        "full height not aging); a cactus at age 8 with air around the cell above draws nextDouble and flowers at 0.1, "
        "or 0.25 three tall; nether wart (nextInt(10)), cocoa (nextInt(5)) and sweet berry bushes (nextInt(5), then "
        "light 9 above) age; kelp (0.14) and weeping, twisting and cave vines (0.1) draw nextDouble below age 25 and "
        "grow a head one older into water or air, cave vines drawing their berries (nextFloat < 0.11); kelp stands on "
        "kelp or a sturdy top but magma's; mushrooms draw nextInt(25) and, with fewer than five of their kind within "
        "4, 1, 4, walk five steps of three offsets (x, y as nextInt(2) - nextInt(2), z) and settle on the last where "
        "they could stay; bamboo shoots and stalks with air and light 9 above draw nextInt(3) and grow a block on 0, "
        "with leaves and thickness by the stalk below and a stop (nextFloat < 0.25) from 11 tall, a stalk thickening "
        "under a thicker one (bamboo_growth); eyeblossoms open by night and close by day in the overworld, drawing four "
        "doubles for their particles and giving each eyeblossom of their old state within 3, 2, 3 a block tick 5 d to "
        "10 d later; turtle eggs crack on sand when nextInt(500) is 0 (or by night's end), drawing their sound's pitch "
        "(eggs_blossoms). Gaps: chorus flowers and saplings are not modeled; hatching turtles are not spawned; "
        "1.21.4's growth is not compared (its random ticks run only near players)."},
    MechanismEntry{"furnace", l0_exact, l2_declared_deviation, harness_trace,
        "Furnaces, smokers and blast furnaces as both official servers run them (furnaces, furnaces_more; with the "
        "official data packs' cooking recipes): lit, they burn a tick of fuel; unlit with fuel and input their type "
        "cooks, they light by a fuel item for its burn time (half in smokers and blast furnaces; every item's burn "
        "time in tests/fixtures/collision/fuel-*.txt), a lava bucket leaving its bucket; the progress grows to the "
        "recipe's cooking time, then the input becomes its result; without fuel the progress falls by 2; a new input "
        "starts it over; a wet sponge dried over an empty bucket fills it; they show whether they are lit and "
        "comparators read them; hoppers fill the input from above and the fuel from the sides and take the result "
        "and empty buckets from below; players use their menus (client/furnace_menus); they keep what they cooked and "
        "give its experience when a player takes the result (at the player) or they are removed (after their contents; "
        "xp_furnaces, xp_explosion, client/furnace_xp). Gaps: the rest of an experience below 1, which the official "
        "servers give by an unseeded random's chance, Circuit gives by a random of its own; furnaces that cooked by "
        "several recipes give them in recipe id order, the official servers in a hash order; minecarts with hoppers are "
        "not modeled; checkpoints before version 28 bring their furnaces back empty."},
    MechanismEntry{"experience_orbs", l0_exact, l2_declared_deviation, harness_trace,
        "Experience orbs as both official servers run them without players near (orbs, orbs_merge, orbs_more; summoned "
        "with their value, count and age): gravity of 0.03 and drag, floating with their eye in water, sliding with the "
        "block's slipperiness, bouncing back from the ground at 0.4 of their speed (on 1.21.8 only when falling faster "
        "than gravity alone makes them, else they rest; on 1.21.4 always, and their vertical drag in double precision), "
        "taking in the orbs of their value whose ids are 40 apart every 20 ticks from their first, keeping the younger "
        "age, and going at age 6000; the nearest player within 8 blocks draws them, and a player who touches one takes "
        "one of its orbs every 2 ticks, gaining its experience with the official servers' levels and progress "
        "(client/orbs); experience at a point comes in orbs of the official values, each drawing a slot of 40 from the "
        "level random to join an orb of its value nearby, a new orb drawing its first motion from its own random "
        "(seeded with its id, as harness 0.36.0 seeds it); mined ores and sculk give their experience (xp_ores, "
        "xp_merge, xp_merge_more), none to Silk Touch; lava makes orbs jump by their own random. Gaps: how players draw "
        "orbs is not compared tick by tick; Mending; experience given outside the command phase gets its orbs' first "
        "motion at the commit (xp_explosion)."},
    MechanismEntry{"brewing", l0_exact, l2_declared_deviation, harness_trace,
        "Brewing stands as both official servers run them (brewing, brewing_hoppers; with the official data packs' "
        "brewing fuel and component encoding): blaze powder gives fuel for 20 brews when none is left; with fuel and a "
        "bottle that mixes with the ingredient they brew for 400 ticks, each such bottle becoming its mix (the 281 mixes "
        "of both servers' brewing reports), and stop when the ingredient changes or nothing is left to brew; the last "
        "ingredient leaves nothing, while dragon's breath of a larger stack throws out its glass bottle; they show which "
        "bottle slots hold something; hoppers reach the ingredient from above and the bottles and fuel from the sides, "
        "and take the bottles and a glass bottle in the ingredient slot from below; comparators read their fullness; a "
        "brewed bottle is a new one of its mix, without the old one's custom color or other components "
        "(client/brewing). Gaps: a thrown-out glass bottle is pushed out of the stand by an unseeded random and is not "
        "compared; whether a brew loaded from a saved world carries on is not observed."},
    MechanismEntry{"crafter", l0_exact, l2_declared_deviation, harness_trace,
        "Crafters as both official servers run them (crafters; with the official data packs' crafting recipes): power at "
        "the block (not above it) sets them triggered and crafts 4 ticks later by the first recipe that takes the grid, "
        "each slot giving one item; the result and then each remainder go into the container in front or are thrown "
        "out through the front whole, as dispensers throw items; they show that they crafted for 6 ticks; hoppers fill "
        "them evenly and take from them; comparators read their filled and disabled slots; players disable and enable "
        "empty slots in their windows (client/crafter_menu). Gaps: items thrown into a full container's cell are pushed "
        "out by an unseeded random and are not compared."},
    MechanismEntry{"lectern", l0_exact, l2_declared_deviation, harness_trace,
        "Lecterns as both official servers run them (lectern_pages; client/lectern): a writable or written book (the item "
        "tag lectern_books) goes on one without a book, used up outside creative mode; a lectern with a book opens its "
        "window, whose buttons turn to the previous or next page, to a page by its number (within the book) or take the "
        "book; a page turn powers the lectern until its block tick 2 ticks later (every neighbor, the block below "
        "strongly), a turn while it is powered adding nothing; comparators read 1 more than 14 times the page's place in "
        "the book (15 for one page); dust connects to it; a removed lectern's book pops out on top, a quarter of a block "
        "toward its front. Gaps: a book put on from the off hand; written books' pages resolved for the player."},
    MechanismEntry{"trapped_chest", l0_exact, l2_declared_deviation, harness_trace,
        "Trapped chests and chest openers as both official servers run them (trapped_chest_pairs; client/openers): "
        "trapped chests pair with trapped chests only and are containers as chests are; each chest half counts the "
        "players viewing it, queues its lid's block event with the count at every change and at a recheck every 5 "
        "ticks while viewed (viewers stay ones as far as their windows reach), and a trapped chest gives its count as power "
        "on every side and strongly to the block below, telling its neighbors and the block below's when the count "
        "changes; dust connects to trapped chests. Gaps: the open and close sounds and the game events sculk sensors "
        "hear are not modeled."},
    MechanismEntry{"bone_meal", l0_exact, l2_declared_deviation, harness_trace,
        "Dispensers use bone meal on the block in front as the official 1.21.8 server does (bonemeal_dispensers): crops "
        "and stems grow 2 + nextInt(4) stages (beetroots a third of that), torchflower and pitcher crops, cocoa and berry "
        "bushes one; bamboo grows 1 + nextInt(2) blocks; big dripleaves grow a block, small ones become big ones "
        "2 + nextInt(4) tall; petals get a flower; cave vines get berries; weeping and twisting vines grow heads while "
        "nextDouble() < 0.826, kelp one; the bone meal is used only when it acts (minecraft.wiki \"Bone Meal\"). Gaps: "
        "players' bone meal is not compared; saplings, grass, moss, mushrooms, nylium, glow lichen, sea pickles, "
        "seagrass, coral and propagules take none (their growth needs world generation's features or is not "
        "modeled)."},
    MechanismEntry{"dripstone", l0_exact, l2_declared_deviation, harness_trace,
        "Pointed dripstone stands on a face sturdy toward its tip or on dripstone pointing the same way; every update "
        "from above or below sets its thickness from the pieces in front of and behind it (tip, tip_merge, frustum, "
        "middle, base); one left unsupported from behind breaks with its drops a tick later as a stalagmite, or two "
        "ticks later turns with every piece below it into falling blocks, which break into their items where they "
        "cannot stand (dripstone_shapes). In random ticks it draws two floats: under water (or mud) or lava above the "
        "block it hangs from, a stalactite's drip below 45/256 or 15/256 gives the cauldron below its tip a block tick "
        "50 + its distance later, which fills it (or turns the mud to clay), and below 64/5625 under a dripstone block "
        "with water above it grows down or raises a stalagmite (dripstone_growth; minecraft.wiki \"Pointed "
        "Dripstone\"). Gaps: falling dripstone hurts nothing; 1.21.4's random ticks are not compared (they run only "
        "near players)."},
    MechanismEntry{"leaves", l0_exact, l2_declared_deviation, harness_trace,
        "As the official 1.21.8 server (leaves_decay): a leaf's update from a neighbor schedules its block tick a tick "
        "later (unless the neighbor is a log and it is at distance 1), the tick setting its distance, one more than the "
        "least of its neighbors' (a log or wood block 0, any other block 7); a leaf at distance 7 that is not "
        "persistent decays in a random tick with what its loot table rolls. A taken tick of a block without rules of "
        "its own does not keep the block from being scheduled again before it runs. Gaps: scaffolding's distance, "
        "players' persistent leaves and 1.21.4's decay are not compared."},
    MechanismEntry{"item_entity", l0_exact, l2_declared_deviation, harness_trace,
        "Double-precision gravity, drag, bouncing and collision with every state's collision boxes; items at "
        "rest move every fourth tick; currents and buoyancy in water; merging with the target versions' timing, "
        "reach and results; block drops and container scatter placed and launched with level-random draws; "
        "despawns at age 6000; pushed by moving blocks; falls counted, landing on the block that supports them "
        "(farmland draws); hurt by cacti; burnt by fire, soul fire and lava (and burning after them), but netherite, which "
        "floats in lava; lava reaching them after their moves on 1.21.8, at the start of their ticks on 1.21.4 "
        "(item_hopper_pickup, item_water_current, item_water_spread, cactus_damage, items_in_fire, entities_in_lava, item_merge, "
        "item_merge_moving, item_merge_crossing, item_merge_vertical, item_merge_timers, item_chest_scatter, "
        "item_water_still, item_water_channel, item_water_ice_corner, item_water_falling, item_water_fall); "
        "entities leave with an unloading chunk and come back a tick after it reloads, under new ids "
        "(item_chunk_edge). Ids of entities spawned outside the command phase are assigned in serial order. Gaps: "
        "reloaded entities come back in coordinate order, not the target "
        "versions' chunk-map order; lava, bubble columns and waterlogged blocks do not move items."},
    MechanismEntry{"chunk_tickets", l0_exact, l2_declared_deviation, harness_trace,
        "Typed tickets with a level, a square and a lifetime; load and simulation levels decide each chunk's "
        "status and what runs; changes take effect in each level's chunk stage after its scheduled ticks; "
        "dropped chunks keep their pending ticks' remaining delay and resume two ticks after a reload "
        "(ring_blocks, unload_reload). Gaps: new chunks load at once, dropped chunks unload at once."},
    MechanismEntry{"light", l0_exact, l0_exact, harness_trace,
        "Block light and sky light from each state's emission, dampening and occlusion shape, following the "
        "blocks as they stood at the end of the previous tick, read by rules in every phase (light_spread, sky_light, "
        "light_shapes, light_timing, mushrooms). Not covered: columns at the edge of the loaded area."},
    MechanismEntry{"game_rules", l0_exact, l2_declared_deviation, controlled_experiment,
        "The version's game rules are world state, set by a logged command within the bounds both official "
        "servers enforce (booleans 0 or 1, spawnChunkRadius 0-32, other integers unbounded); random ticks "
        "read randomTickSpeed, water and lava waterSourceConversion and lavaSourceConversion (fluid_sources). /gamerule gave the official replies for 45 commands on both versions. Gaps: the "
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
    MechanismEntry{"falling_block", l0_exact, l2_declared_deviation, harness_trace,
        "Sand, red sand, gravel, concrete powders and anvils fall two ticks after they are placed or a neighbor changes, "
        "when the block below can be replaced; the falling block moves like an item in the air and becomes its "
        "block where it lands if that cell can be replaced and the one below cannot, or drops its item; concrete "
        "powder becomes concrete when it falls into water (falling_drop, falling_column, falling_onto); an anvil "
        "wears as it lands by its own random, after more than a block, and is gone worn past damaged, its fall the "
        "version's (anvils_falling, anvils_falling_more, anvils_falling_far); "
        "in machines they ride slime, fall off ledges and from pulled supports and land on moving pistons "
        "(gravity_machines). Gaps: concrete powder beside water, scaffolding and suspicious blocks; falling "
        "blocks hurt nothing."},
    MechanismEntry{"tnt", l0_exact, l2_declared_deviation, harness_trace,
        "TNT with a redstone signal from a neighbor when it is placed, moved in by a piston or updated becomes "
        "primed TNT (the entity first, launched from the level random, then the block goes), unless the 1.21.8 "
        "rule tntExplodes is off; primed TNT falls, slides with 30% friction on any ground, is pushed by currents "
        "after its move and keeps its fuse with an unloading chunk. At the end of its fuse it explodes: rays with "
        "single-precision intensity and blast resistance, the reached cells in hash-set order shuffled by the level "
        "random, entities hurt and pushed by distance and exposure first (items destroyed or left with less "
        "health), then blocks cleared in order with their drops merged into stacks of 16 and TNT primed with a "
        "random fuse (tnt_ignition, tnt_motion, tnt_water_current, tnt_reload, tnt_explodes_disabled, "
        "tnt_redstone_ignition, tnt_obsidian, tnt_drops, tnt_void, tnt_entity_order, tnt_knockback, tnt_items, "
        "tnt_chain_edge, tnt_water, tnt_power). A named TNT duplicator, the Mytteri push duper, primes its TNT on "
        "every push while the block moves on, over eight pushes, in each direction and across chunk edges "
        "(tnt_duper, tnt_duper_turns, tnt_duper_edges, tnt_duper_corners). Gaps: fire, players and projectiles other than burning "
        "arrows do not ignite TNT; other duplicator designs are untested; "
        "entities of one chunk section are hurt in id order rather than the order they entered it; explosions "
        "do not hurt players yet."},
    MechanismEntry{"projectile", l0_exact, l2_declared_deviation, harness_trace,
        "Arrows and snowballs summoned with a velocity: arrows move, then lose 1% of their speed (40% in water) "
        "and 0.05 downwards, stick 0.05 short of the block shape they hit, count their life to 1,200 and fall again "
        "when no block is left beside them; burning arrows count their fire down in flight and prime TNT they "
        "hit; fire and lava their box reaches set them burning (160 and 300 ticks; lava before the count on 1.21.8, "
        "after it on 1.21.4), a stuck one's fire does not count; arrows and snowballs tilt big dripleaves they hit; "
        "snowballs fall by 0.03 and lose 1% before they move, and break on the first block (arrow_flight, arrow_tnt, "
        "items_in_fire, entities_in_lava, dripleaf_arrows). Gaps: projectiles pass through entities; bows, crossbows, dispensers and players do not shoot; "
        "currents do not push arrows; water does not put out burning arrows; other projectiles."},
    MechanismEntry{"vehicle", l0_exact, l2_declared_deviation, harness_trace,
        "Minecarts off and on rails and boats without riders, summoned: on rails minecarts follow the rail's line at up to "
        "0.4 a tick, keep 96% of their speed, gain speed down slopes and lose it up them, turn through curves, stop at blocks, "
        "are braked by unpowered powered rails and boosted by powered ones (minecart_rails, minecart_tracks); minecarts collide with minecarts' and "
        "boats' boxes, items, primed TNT and falling blocks with boats', and minecarts push minecarts by their yaw (minecart_meeting, "
        "minecart_meeting_more); off rails they fall, keep within 0.4 a tick, slow on slime blocks they land on "
        "(slime_landing), halve "
        "their speed on the ground and lose 5% in the air; boats float on water at a depth proportional "
        "buoyancy, sink under it, slide on land with the slipperiness of the blocks under them, are pushed by "
        "currents and land on the surface when they fall into water (minecart_ground, minecart_water, "
        "boat_motion, boat_land). Gaps: riders (a moving minecart would pick up the boats it touches), players and mobs pushing or entering vehicles, "
        "chest and furnace variants."},
    MechanismEntry{"redstone_torch", l0_exact, l2_declared_deviation, harness_trace,
        "Floor and wall torches power every neighbor but the block they are on, and the block above them "
        "strongly; power in that block (from a source, through a conductor or from dust) turns them off 2 "
        "ticks after an update, and its loss on again; every state change and removal updates the neighbors of "
        "each neighbor; the eighth turn-off of a position within 60 ticks burns a torch out for 160 ticks, "
        "turn-offs kept with their column; torches drop without their support and are washed away (torch_basics, "
        "water_torch_button). Gaps: players "
        "place torches on the clicked face rather than by their view; floor support comes from collision "
        "shapes; no burnout sound or smoke."},
    MechanismEntry{"button", l0_exact, l2_declared_deviation, harness_trace,
        "Floor, wall and ceiling buttons of every kind: used, they press, power their neighbors and their block "
        "strongly, update both, and release after 20 ticks (stone, polished blackstone) or 30 (wooden); wooden "
        "buttons are pressed by an arrow whose box meets their shape at the end of its tick, flying or stuck, "
        "and stay pressed while one does; removed while pressed, they update around; water washes them away "
        "(button_basics, water_torch_button). Gaps: a "
        "pending check of one button kind also serves another kind placed at the same position; other "
        "projectiles that press buttons are not modeled."},
    MechanismEntry{"pressure_plate", l0_exact, l2_declared_deviation, harness_trace,
        "Wooden, stone, polished blackstone and weighted plates of every kind sense the entities whose box meets "
        "the lower quarter of their cell, a pixel in from each side: an unpowered plate checks when an entity's "
        "move ends in its cell (resting items every fourth tick), a powered one 20 ticks (weighted: 10) after "
        "each check; wooden plates give 15 for any entity, a light weighted plate a level per entity, a heavy "
        "one a level per ten; changes are quiet and update around the plate and the block below it, which it "
        "powers strongly; water does not pass them (pressure_plates, plate_target_more). Gaps: no mobs or "
        "players press plates yet."},
    MechanismEntry{"target", l0_exact, l2_declared_deviation, harness_trace,
        "A projectile hitting a target block gives 1 to 15 by how near the center of the hit face it hit "
        "(the larger of the two offsets, in double precision), for 20 ticks after an arrow and 8 after a "
        "snowball; hits while it waits are ignored (target_block, plate_target_more). Gaps: other projectiles; "
        "a target placed with power keeps it."},
    MechanismEntry{"door", l0_exact, l2_declared_deviation, harness_trace,
        "Doors of every kind open and close at once when an update finds power around either half different from "
        "their powered state, and by hand unless iron; the change is quiet and the other half follows through its "
        "shape update; each opening and closing draws a float from the level random; a half without its other half "
        "or a lower half without a sturdy top below breaks, the lower one with the door's drop (door_basics). Players "
        "place doors with the hinge both official servers give them (tests/fixtures/placement/hinge.txt). Gaps: copper "
        "doors do not weather."},
    MechanismEntry{"trapdoor", l0_exact, l2_declared_deviation, harness_trace,
        "Trapdoors of every kind follow the power around them and open by hand unless iron, quietly, with a draw "
        "for the sound; entities collide with their open and closed shapes (door_basics). Gaps: waterlogging is "
        "stored only; players place side trapdoors in the bottom half; copper trapdoors do not weather."},
    MechanismEntry{"fence_gate", l0_exact, l2_declared_deviation, harness_trace,
        "Fence gates follow the power around them and open by hand, turning to face away from the player opened "
        "from their facing side (commands use them as a player facing south); a gate between walls is in a wall "
        "(door_basics). Gaps: walls are recognized by name."},
    MechanismEntry{"honey", l0_exact, l2_declared_deviation, harness_trace,
        "Honey blocks hold their neighbors in piston structures as slime does, but slime and honey do not hold "
        "each other; honey and soul sand slow what moves on them to 40%; items and primed TNT falling beside honey "
        "slide down it, TNT drawing twice from the level random as it does; honey moving sideways carries what "
        "stands on it (honey_basics, honey_slide, honey_entry, honey_more). Gaps: entities other than items and primed "
        "TNT do not slide; jumping is not modeled."},
    MechanismEntry{"rails", l0_exact, l2_declared_deviation, harness_trace,
        "Rails, powered, activator and detector rails take their shapes from the rails beside them as they appear and "
        "make those join them, curve (plain rails) with the official preferences, rise toward a higher rail, switch at "
        "three-way junctions on updates from signal sources, need a sturdy top below (and on the side they rise "
        "toward), and powered and activator rails pass power along up to eight rails of their kind (rail_shapes, "
        "rail_power); minecarts ride them (vehicle); detector rails sense minecarts in the middle of their cell, look again every "
        "20 ticks and power their ends, neighbors and the block below (detector_rails, detector_rails_more). Gaps: comparators read "
        "0 from detector rails and activator rails act on nothing, as only empty minecarts exist."},
    MechanismEntry{"coral_fan", l0_exact, l2_declared_deviation, harness_trace,
        "Coral fans and wall fans of every kind: a living fan without water near it schedules its death 60 to 99 ticks "
        "later with a level-random draw when placed and on every shape update, and dies dry unless water came back; "
        "fans need a sturdy top or face and break without it, a waterlogged one leaving its water; pistons break "
        "living fans and move dead ones (coral_fans). Gaps: currents that push entities do not see waterlogged fans' water."},
    MechanismEntry{"connections", l0_exact, l2_declared_deviation, harness_trace,
        "Fences, glass panes, iron bars and walls join their neighbors on shape updates and when a piston sets them "
        "down: wooden fences join wooden fences, nether brick fences their own kind, panes and bars each other and "
        "walls, walls walls and panes; fences and walls join fence gates turned across the side; all join a whole "
        "support face except of leaves, barriers, shulker boxes, pumpkins, melons and chorus flowers; walls rise "
        "under blocks covering their sides and keep or lose their post (connections, connections_moved; every state "
        "beside and above them in tests/fixtures/collision/connection-*.txt); players place them with their shapes "
        "(client/building). Gaps: currents that push entities do not see waterlogged ones' water."},
    MechanismEntry{"stairs", l0_exact, l2_declared_deviation, harness_trace,
        "Stairs of any material take straight, inner and outer corner shapes from the stairs of the same half beside "
        "them on shape updates from the sides and when a piston sets them down (stairs_shapes; every combination of "
        "stairs on their four sides in tests/fixtures/collision/stairs.txt); players place them facing their way, in the "
        "clicked half, with their shape (client/building). Gaps: currents that push entities do not see waterlogged ones' water."},
    MechanismEntry{"note_block", l0_exact, l2_declared_deviation, harness_trace,
        "Note blocks take the instrument of a mob head above them or else of the block below, when placed, moved "
        "and on a shape update from above or below; they play on a rising signal and when used (the next note), "
        "unless a block other than air is above and the instrument is not a head's; the block event draws a long "
        "from the level random for the sound (note_instruments, note_play). Gaps: player heads with a note block "
        "sound of their own play nothing."},
    MechanismEntry{"daylight_detector", l0_exact, l2_declared_deviation, harness_trace,
        "Daylight detectors set their output every 20 game ticks from the sky light in their cell less the sky's "
        "darkening by the day time, rain and thunder; inverted ones give 15 less that, normal ones scale it by the "
        "sun's angle; using one inverts it and sets its output at once (daylight_basics, daylight_cycle, "
        "daylight_more). Gaps: none known."},
    MechanismEntry{"dispenser", l0_exact, l2_declared_deviation, harness_trace,
        "Dispensers and droppers facing any side: a neighbor update that finds power at the block or above it "
        "triggers one 4 ticks later, which picks a random filled slot with level-random int draws; droppers fill "
        "the container in front or eject the item; dispensers eject items without a use, with the observed "
        "position, motion and draws, prime TNT (keep it when tntExplodes is off), empty water and lava buckets "
        "into blocks that hold water, air and what fluids replace, fill buckets from fluid sources, powder snow and "
        "blocks holding water, set minecarts on rails, and eject equipment, as there are no entities to equip; "
        "comparators read them and hoppers fill and empty them (dispensers, dispensers_more, dispenser_cases, "
        "dispenser_items, dispenser_tnt_rule, dispenser_buckets). Gaps: projectiles, fire, "
        "bone meal, shears, boats on water, bottles, honeycomb, glowstone and water bottles at their targets, "
        "golem and wither heads, buckets of fish, axolotls, tadpoles and powder snow, minecarts and mobs, and "
        "equipping players are not modeled: those "
        "items stay."},
    MechanismEntry{"entity_order", l0_exact, l2_declared_deviation, harness_trace,
        "Entities tick in ascending id order once per tick; one merged away before its turn does not tick "
        "(item_hopper_pickup, item_water_current, item_merge, item_merge_moving). Gap: compared for item "
        "entities only."},
    MechanismEntry{"block_entity_order", l0_exact, l0_exact, harness_trace,
        "Block entities tick in creation order; moving pistons finish in creation order (piston_slime, "
        "piston_observed) and competing hoppers act in placement order (hopper_competition). Not covered: "
        "order after a chunk reload."},
    MechanismEntry{"support", l0_exact, l0_exact, harness_trace,
        "Dust needs a whole top face or a hopper below it, repeaters and comparators a whole top face, levers, "
        "buttons and wall torches a whole face to hang on, floor torches and pressure plates the middle of the top; "
        "an update that finds the support gone breaks them with their drops (support, lever_attachment, "
        "torch_basics, button_basics, pressure_plates). Torches, lanterns, ladders, carpets, flowers and saplings "
        "need theirs too and break when it goes (supported_blocks); signs and banners need a solid block (signs_beds); "
        "grasses, ferns, bushes, petals, fungi, roots, wither roses, azaleas, crops, nether wart and bamboo need the "
        "ground of their kind (plant_grounds; every state in tests/fixtures/collision/plants-*.txt); sugar cane water beside "
        "its ground, cacti nothing solid beside them, lily pads water or ice and sea pickles a top, sugar cane and cacti "
        "breaking a tick after an update (plant_supports; tests/fixtures/collision/plant-contexts-*.txt); vines, hanging "
        "roots, spore blossoms, pale hanging moss and hanging propagules what holds them, vines turning between heads and "
        "bodies (vines, with the operator's data packs); small and big dripleaves and stems, big dripleaves tilting under "
        "entities and back with a signal (dripleaves) and at once under arrows and snowballs (dripleaf_arrows); mushrooms their ground or darkness (mushrooms); farmland turns to "
        "dirt a tick after a solid block is set on it (farmland). Support shapes "
        "and solidity of every state come from the registry (tests/fixtures/collision/support-*.txt). The drops' "
        "positions follow item_entity; without data packs, plants whose loot rolls random numbers drop nothing (with "
        "them, see loot)."},
    MechanismEntry{"fire", l0_exact, l2_declared_deviation, harness_trace,
        "Fire stays on a sturdy top or beside a flammable block (soul fire on soul sand and soul soil), takes the faces "
        "of flammable neighbors when nothing below bears or feeds it, and goes when an update finds it unsupported. "
        "Every state of fire draws its next block tick (30 to 39 ticks) from the level random; in it, with doFireTick "
        "(on 1.21.8 also a player within 128 blocks of the chunk, or allowFireTicksAwayFromPlayer), it ages, burns out, "
        "burns its neighbors by their burn odds (TNT primed) and spreads to cells beside flammable blocks by their "
        "ignite odds and the difficulty; netherrack and magma blocks keep it (fire_basics, fire_stone, fire_netherrack, "
        "fire_planks, fire_log, fire_tnt, fire_wool, fire_spread; the odds from minecraft.wiki \"Fire\", which blocks burn in "
        "tests/fixtures/collision/fire-*.txt); items and arrows in fire burn (items_in_fire, see item_entity and "
        "projectile). Lava sets fire in its random ticks (taken twice: as a block and as its fluid), stepping up to two "
        "cells up and around to air beside a block lava ignites, or setting fire on such a block beside it "
        "(lava_fire; which blocks lava ignites in tests/fixtures/collision/lava-ignites-*.txt). Gaps: rain and humid "
        "biomes do not put it out sooner; other entities in fire, and lightning and flint and steel setting fire are "
        "not modeled; the player range on 1.21.8 is that of "
        "random ticks, not observed for fire; the pending tick of fire that went out stands for any block without rules "
        "of its own set there before it is due (as their ticks do for each other), and Anvil export leaves it out."},
    MechanismEntry{"weathering", l0_exact, l2_declared_deviation, harness_trace,
        "As the official 1.21.8 server in random ticks (melt_weather): ice melts when its block light is more than 11 "
        "less its light dampening, a snow layer goes with its drops above block light 11; lit redstone ore goes out; "
        "budding amethyst draws nextInt(5) and a side (nextInt(6)), setting a small bud in air or water there or growing "
        "a bud facing that side; copper that can still weather draws nextFloat() < 64 / 1125 and, unless less "
        "weathered copper is within a taxicab distance of 4, weathers at nextFloat() < m c^2 (minecraft.wiki "
        "\"Oxidation\"), copper doors (by their lower halves, the upper following) and trapdoors too (eggs_blossoms). "
        "Gaps: rain, snowfall and freezing are not modeled; 1.21.4's random ticks are not compared (they run only near "
        "players)."},
    MechanismEntry{"snow_and_ice", l0_exact, l2_declared_deviation, harness_trace,
        "Snow layers lie on whole collision tops, honey, soul sand, mud and eight layers of snow, but not on ice, packed "
        "ice or barriers, and an update that finds them unsupported breaks them without drops; grass blocks, podzol and "
        "mycelium are snowy under snow layers, snow blocks and powder snow; ice and frosted ice a player breaks without "
        "silk touch become water over a solid block or a liquid; frosted ice draws its block ticks from the level "
        "random and ages and melts in them by its frosted neighbors and the light, and melts when an update from frosted "
        "ice leaves it fewer than two frosted neighbors; snow drops its snowballs for players and explosions; players "
        "stack snow layers up to eight and any block they place replaces a single layer (snow_layers, ice_break, "
        "frosted_ice, frosted_ice_ticks, snow_explosion; every state under a snow layer in "
        "tests/fixtures/collision/snow-*.txt; a protocol client's 40 placed states). Gaps: melting and freezing by light "
        "and weather in random ticks; powder snow; snow and ice in the Nether are not compared."},
    MechanismEntry{"container_menus", l0_exact, l2_declared_deviation, controlled_experiment,
        "Player windows and chest, double chest, hopper, furnace, crafter, brewing stand, crafting table, stonecutter, smithing "
        "table, grindstone, loom, anvil, enchanting table and lectern menus with every click mode, applied as commands. "
        "A 48-step client scenario (every click mode, double chests, hoppers, the own window, throws and "
        "pickups) gives the same windows, carried stacks and inventories as the official 1.21.8 and 1.21.4 "
        "servers. With data packs the own window's 2x2 grid crafts: a 48-step scenario (results, every way "
        "of taking them, shapes, remainders, transmuting) matches both official servers, and a 26-step scenario "
        "in furnace, smoker and blast furnace windows (slot rules, quick moves by recipe and fuel, cooking, their "
        "properties) matches both too, as does a 47-step scenario at crafting tables (3x3 recipes, quick moves into "
        "and out of the grid, the grid given back on closing and on breaking, dropped on death and on leaving), and a "
        "27-step scenario at a stonecutter (recipe buttons, every way of taking the result, the choice cleared by "
        "another input) with the clients' recipe data, and a 24-step scenario at a smithing table (upgrades keeping "
        "components, trims and their registry ids), and a 20-step scenario in a crafter's window (slot toggles, the "
        "result shown), a 25-step scenario at a grindstone (curses kept, durabilities merged, books) and a 32-step "
        "scenario at a loom (pattern buttons, the choice following the slots, layers), and a 36-step scenario at a brewing "
        "stand (slot rules, quick moves to fuel, ingredient and bottles, brews stopped and started by the ingredient, "
        "brewed bottles losing their other components, the brew time and fuel properties), and a 48-step scenario at an anvil "
        "(repairs with materials and by combining, enchantments from books, typed names kept between items, filtered and "
        "limited, the 39-level cap for renaming, too expensive results, levels taken, the /xp command), and a 97-step scenario "
        "and a 64-step survey at enchanting tables (offers for 0 to 16 bookshelves, blocked and raised ones, 35 items, a "
        "player's seed, refusals for levels and lapis, enchanting a sword and a book). Gaps: no "
        "special recipes or recipe book; a grindstone's experience draws from the level random, so the client "
        "scenario checks only its range (client/furnace_xp); an anvil wears by a random of Circuit's own (the official "
        "servers' is unseeded); a new player's enchantment seed and the one after enchanting "
        "are Circuit's own; trim materials of 1.21.5 "
        "and later from the vanilla materials' items; no harness traces of menus."},
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
// registries and game rules (fire acts away from players only with 1.21.8's
// allowFireTicksAwayFromPlayer), and where random ticks run
// (RandomTickRange), when lava reaches entities (LavaContact), how
// experience orbs come to rest (OrbMotion) and the fall an anvil wears by
// (FallDistance), which the official servers showed differ.
constexpr RuleSet rules_1_21_8{"circuit-reference-rules-v12", RngPolicy::level_random_in_target_order, reference_v4_mechanisms,
                               "circuit-blocks-1", "harness traces of 1.21.8 (tests/fixtures/harness)", RandomTickRange::entity_ticking,
                               LavaContact::after_move, OrbMotion::settling, FallDistance::with_landing};
constexpr RuleSet rules_1_21_4{"circuit-reference-rules-v12", RngPolicy::level_random_in_target_order, reference_v4_mechanisms,
                               "circuit-blocks-1",
                               "harness traces of 1.21.4 (tests/fixtures/harness-1.21.4), each identical to its 1.21.8 recording "
                               "(fire's with 1.21.8's allowFireTicksAwayFromPlayer on) but those of entities in lava, which "
                               "reaches them at the start of their ticks, and of experience orbs, which bounce on the ground "
                               "and drag in double precision, and of anvils, which wear by their fall before landing; random "
                               "ticks run only near players (observed on the official 1.21.4 server)",
                               RandomTickRange::block_ticking_near_player, LavaContact::tick_start, OrbMotion::bouncing,
                               FallDistance::before_landing};

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
