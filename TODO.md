# Circuit Roadmap

Circuit targets Minecraft Java Edition with C++23, xmake, and standalone Asio. Java Edition 1.21.8 is the initial implementation target. A second complete version bundle will validate the version-selection architecture.

## Part I — Simulation and technical gameplay

The first major deliverable is a headless simulation core with independently checked behavior. This work takes priority over networking, persistence, and extensions. Technical-gameplay cases inform the contracts and reference model throughout development.

- [x] **M1 — Simulation contracts:** define stable identities, owner generations, logical time, version bundles, and normalized commands.
- [x] **M2 — Reference world:** implement an independent sequential model and replayable scenarios for a minimal world.
- [x] **M3 — Parallel simulation:** implement chunk-region ownership, event propagation, coordinated operations, and differential validation against the reference model.
- [x] **M7 — Technical gameplay:** validate redstone, pistons, containers, fluids, and entity interactions across chunk boundaries.

Completion requires stable state and event traces under different scheduling choices, version-specific behavioral evidence, and explicit coverage limits. A client connection is not a prerequisite for the headless core.

Part I is complete for its declared scope on Java Edition 1.21.8. The reference rules pass 20 traces recorded on the target version. Each mechanism declares a compatibility tier: eight claim exact (L0) behavior, and the rest declare their gaps (L2). The simulation guide lists the coverage limits. Closing L2 gaps, such as power through solid blocks and entity observations in traces, continues as compatibility work.

## Part II — Integration and additional capabilities

- [ ] **M4 — Playable vertical slice:** add the initial version's connection flow, identity providers, authoritative actions, and world interaction.
- [ ] **M5 — Persistence:** add consistent checkpoints, crash recovery, and vanilla import/export within a documented compatibility range.
- [ ] **M6 — Extension ecosystem:** complete native plugin/mod capabilities, the Circuit Java bridge, and technical tools exposed through extension APIs.
- [ ] **M8 — Optimization and version expansion:** measure storage and scheduling improvements, complete a second version bundle, and prepare a release compatibility matrix.

**Foundation:** M0 provides the build, executor, synthetic fixtures, native plugin example, and module interfaces. This baseline does not complete any Part I milestone.

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
- [ ] Document protocol, content, behavior, and extension compatibility separately.

## Plugins and technical tools

- [ ] Provide an inheritance-based C++ SDK with shared-library loading, ABI checks, and explicit lifetimes.
- [ ] Support startup-time mod registration of content and rules.
- [ ] Run Circuit Java API plugins through a bounded, versioned bridge.
- [ ] Evaluate Bukkit/Spigot/Paper compatibility as a separate project with its own compatibility matrix.
- [ ] Add tick and chunk profiling, freeze/step controls, entity inspection, redstone traces, item-flow counters, and spawning statistics.

## Storage and validation

- [ ] Persist consistent world checkpoints and verify recovery under interrupted writes.
- [ ] Import and export supported vanilla saves without overwriting source data or silently discarding unsupported content.
- [ ] Compare palette encoding and compression options using representative worlds.
- [ ] Store sparse changes only when the exact baseline can be verified or retained.
- [ ] Validate movement, reach, cooldowns, inventory conservation, and resource consumption using authoritative version rules.
- [ ] Reject malformed, non-finite, overflowing, replayed, and out-of-state inputs.
- [ ] Keep identity verification separate from protocol and simulation.

## Performance and parallelism

- [x] Profile representative target-version workloads and estimate the serial fraction under the declared compatibility tiers before optimizing.
- [ ] Evaluate lower-risk parallel layers: world generation, lighting, storage I/O, packet encoding and compression, and read-only pathfinding.
- [ ] Report speedups only with comparable semantics, fixed inputs, hardware details, and tail latency.

## Release readiness

- [ ] Publish reproducible correctness and performance results, including known limitations.
- [ ] Provide documented build identity and verifiable release provenance.
- [ ] Review contributor rights and third-party notices for AGPLv3 and alternative commercial licensing.
- [ ] Define supported platforms, migration paths, operational limits, and recovery procedures.

Milestone completion requires the relevant behavioral and integration tests. Framework fixtures and declared version candidates do not constitute playable version support.
