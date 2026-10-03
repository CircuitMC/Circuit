# Circuit Roadmap

Circuit targets Minecraft Java Edition with C++23, xmake, and standalone Asio. Java Edition 1.21.8 is the initial implementation target. A second complete version bundle will validate the version-selection architecture.

## Part I — Simulation and technical gameplay

The first major deliverable is a headless simulation core with independently checked behavior. This work takes priority over networking, persistence, and extensions. Technical-gameplay cases inform the contracts and reference model throughout development.

- [ ] **M1 — Simulation contracts:** define stable identities, owner generations, logical time, version bundles, and normalized commands.
- [ ] **M2 — Reference world:** implement an independent sequential model and replayable scenarios for a minimal world.
- [ ] **M3 — Parallel simulation:** implement chunk ownership, event propagation, coordinated operations, and differential validation against the reference model.
- [ ] **M7 — Technical gameplay:** validate redstone, pistons, containers, fluids, and entity interactions across chunk boundaries.

Completion requires stable state and event traces under different scheduling choices, version-specific behavioral evidence, and explicit coverage limits. A client connection is not a prerequisite for the headless core.

## Part II — Integration and additional capabilities

- [ ] **M4 — Playable vertical slice:** add the initial version's connection flow, identity providers, authoritative actions, and world interaction.
- [ ] **M5 — Persistence:** add consistent checkpoints, crash recovery, and vanilla import/export within a documented compatibility range.
- [ ] **M6 — Extension ecosystem:** complete native plugin/mod capabilities, the Circuit Java bridge, and technical tools exposed through extension APIs.
- [ ] **M8 — Optimization and version expansion:** measure storage and scheduling improvements, complete a second version bundle, and prepare a release compatibility matrix.

**Foundation:** M0 provides the build, executor, synthetic fixtures, native plugin example, and module interfaces. This baseline does not complete any Part I milestone.

## Simulation and compatibility

- [ ] Separate time advancement, immediate causal events, and future scheduled events.
- [ ] Preserve event order and intermediate states required by each version's rules.
- [ ] Coordinate multi-owner operations without duplicate updates or partial publication.
- [ ] Compare state and event traces across worker counts and scheduling orders.
- [ ] Select one authoritative version bundle per server instance; reject unsupported configurations.
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

## Release readiness

- [ ] Publish reproducible correctness and performance results, including known limitations.
- [ ] Provide documented build identity and verifiable release provenance.
- [ ] Review contributor rights and third-party notices for AGPLv3 and alternative commercial licensing.
- [ ] Define supported platforms, migration paths, operational limits, and recovery procedures.

Milestone completion requires the relevant behavioral and integration tests. Framework fixtures and declared version candidates do not constitute playable version support.
