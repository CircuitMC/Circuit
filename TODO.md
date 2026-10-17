# Circuit Roadmap

Circuit targets Minecraft Java Edition with C++23, xmake, and standalone Asio. Java Edition 1.21.8 is the initial implementation target. Java Edition 1.21.4 is a second bundle for the same declared gameplay subset. Full server-side compatibility for both versions is the next major deliverable.

## Part I — Simulation and technical gameplay

The first major deliverable is a headless simulation core with independently checked behavior. This work takes priority over networking, persistence, and extensions. Technical-gameplay cases inform the contracts and reference model throughout development.

- [x] **M1 — Simulation contracts:** define stable identities, owner generations, logical time, version bundles, and normalized commands.
- [x] **M2 — Reference world:** implement an independent sequential model and replayable scenarios for a minimal world.
- [x] **M3 — Parallel simulation:** implement chunk-region ownership, event propagation, coordinated operations, and differential validation against the reference model.
- [x] **M7 — Technical gameplay:** validate redstone, pistons, containers, fluids, and entity interactions across chunk boundaries.

Completion requires stable state and event traces under different scheduling choices, version-specific behavioral evidence, and explicit coverage limits. A client connection is not a prerequisite for the headless core.

Part I is complete for its declared scope on Java Edition 1.21.8 and 1.21.4. The reference rules pass 30 traces recorded on each target version. Each mechanism declares a compatibility tier: fifteen claim exact (L0) behavior, and the rest declare their gaps (L2). The simulation guide lists the coverage limits. Closing the remaining L2 gaps, such as entity observations in traces, continues as compatibility work.

## Part II — Integration and additional capabilities

- [x] **M4 — Playable vertical slice:** add the initial version's connection flow, identity providers, authoritative actions, and world interaction.
- [x] **M5 — Persistence:** add consistent checkpoints, crash recovery, and vanilla import/export within a documented compatibility range.
- [x] **M6 — Extension ecosystem:** complete native plugin/mod capabilities, the Circuit Java bridge, and technical tools exposed through extension APIs.
- [x] **M8 — Optimization and version expansion:** measure storage and scheduling improvements, complete a second version bundle, and prepare a release compatibility matrix.

**Foundation:** M0 provides the build, executor, synthetic fixtures, native plugin example, and module interfaces. This baseline does not complete any Part I milestone.

## Part III — Complete vanilla compatibility

Parts I and II completed their original subset, not the complete game. The [vanilla completion roadmap](docs/VANILLA_COMPLETION.md) defines the target, current gaps, version differences, dependencies and acceptance gates. Each phase must validate both 1.21.4 and 1.21.8; registry entries and protocol IDs do not count as implemented gameplay.

- [x] **C0 — Baselines and evidence:** exact version/profile manifests, a feature/evidence ledger, entity/RNG/lifecycle observations and independent comparisons ([ledger](docs/VANILLA_LEDGER.md)).
- [x] **C1 — Complete data model:** extensible block states, item/entity/block-entity components, versioned schemas and registry/pack foundations.
- [x] **C2 — World foundations:** dimension-scoped ownership, vanilla ticket levels, chunk lifecycle, lighting and environmental state.
- [x] **C3 — Entities and TNT:** general entity lifecycle/physics, non-living entities (falling blocks, arrows, snowballs, minecarts off rails, boats) and baseline TNT ignition, fuse and explosions.
- [ ] **C4 — Technical gameplay:** missing components and exact machine behavior, including version-specific TNT duplication and cross-chunk acceptance.
- [ ] **C5 — Blocks, items and production:** complete block families, item use, fluids/growth, recipes/loot and functional menus.
- [ ] **C6 — Complete player gameplay:** modes, movement, combat, equipment, effects, food and experience.
- [ ] **C7 — Worlds and dimensions:** same-seed terrain/biomes/structures, presets, portals and travel.
- [ ] **C8 — Living world:** mob AI/navigation, spawning/farms, villages/trading/raids, bosses and version-specific creatures.
- [ ] **C9 — Commands and data packs:** grammar/permissions, functions/selectors, advancements, scoreboards, gamerules and server operations.
- [ ] **C10 — Complete client integration:** all server-visible state, packets, metadata, audio/visual events, signed chat, packs, maps and dialogs/waypoints.
- [ ] **C11 — Complete world interchange:** both target formats, all persistent data, tested migrations and interruption recovery.
- [ ] **C12 — Release qualification:** complete version/profile matrices, survival/machine/farm campaigns, real clients, endurance and performance under accepted semantics.

Persist and expose new state to clients as each feature is delivered. C10 and C11 close the remaining integration inventory. Full compatibility remains open while mandatory behavior is missing, unverified or knowingly different. The checklists below retain the original foundation scope and continuing work.

## Simulation and compatibility

- [x] Separate time advancement, immediate causal events, and future scheduled events.
- [ ] Preserve event order and intermediate states required by each version's rules.
- [x] Coordinate multi-owner operations without duplicate updates or partial publication.
- [x] Compare state and event traces across worker counts, region layouts, and seeded schedule exploration; reproduce failures from their seeds.
- [x] Declare a compatibility tier for each mechanism: exact trace and RNG equivalence, region-local equivalence, or a documented deviation.
- [x] Reproduce level-wide ordering rules, such as scheduled-tick order, block-event order, and update-chain limits, without depending on thread timing.
- [x] Schedule simulation through dynamic chunk regions that merge on interaction or repeated conflict and split at stage boundaries.
- [x] Define global inputs: chunk load levels, player input order, publication of generated chunks, and tick-overrun catch-up.
- [x] Capture reference traces from an instrumented target-version server kept outside this repository; implement behavior from specifications and traces under a clean-room policy.
- [x] Select one authoritative version bundle per server instance; reject unsupported configurations.
- [x] Document protocol, content, behavior, and extension compatibility separately ([compatibility](docs/COMPATIBILITY.md)).

## Plugins and technical tools

- [x] Provide an inheritance-based C++ SDK with shared-library loading, ABI checks, and explicit lifetimes.
- [x] Support startup-time mod registration with dependency order and a frozen registry. Content and rules stay fixed by the rule set.
- [x] Run Circuit Java API plugins through a bounded, versioned bridge.
- [x] Evaluate Bukkit/Spigot/Paper compatibility as a separate project with its own compatibility matrix.
- [x] Add tick profiling, freeze/step controls, chunk and block inspection, redstone traces and item-flow counters.
- [ ] Add spawning statistics and entity inspection beyond item entities, once the rules model mobs.

## Storage and validation

- [x] Persist consistent world checkpoints and verify recovery under interrupted writes.
- [x] Import and export supported vanilla saves without overwriting source data or silently discarding unsupported content.
- [x] Compare palette encoding and compression options using representative worlds.
- [x] Store sparse changes only when the exact baseline can be verified or retained. Encoding reuses unchanged sections, identified exactly; files stay complete snapshots.
- [x] Validate movement, reach, cooldowns, inventory conservation, and resource consumption using authoritative version rules. Survival play follows the target version: movement against block shapes, reach, dig time, hurt cooldowns, hunger, item use and container clicks, checked against the official servers. Items other than blocks have no use yet, so item cooldowns do not arise.
- [x] Reject malformed, non-finite, overflowing, replayed, and out-of-state inputs.
- [x] Keep identity verification separate from protocol and simulation.

## Performance and parallelism

- [x] Profile representative target-version workloads and estimate the serial fraction under the declared compatibility tiers before optimizing.
- [x] Evaluate lower-risk parallel layers: world generation, lighting, storage I/O, packet encoding and compression, and read-only pathfinding. Storage I/O and chunk-packet encoding run in parallel; the others are not needed yet or not applicable.
- [x] Report speedups only with comparable semantics, fixed inputs, hardware details, and tail latency.

## Release readiness

- [x] Publish reproducible correctness and performance results, including known limitations.
- [x] Provide documented build identity and verifiable release provenance. Signing a release remains the project owner's step.
- [ ] Review contributor rights and third-party notices for AGPLv3 and alternative commercial licensing. The notices are current; the contributor rights review is the owner's.
- [x] Define supported platforms, migration paths, operational limits, and recovery procedures.

Milestone completion requires the relevant behavioral and integration tests. Framework fixtures and declared version candidates do not constitute playable version support.
