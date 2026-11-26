# Circuit Roadmap

Circuit targets Minecraft Java Edition 1.21.8 and 1.21.4, one version per server process, with C++23 and xmake. The roadmap below follows the core restructuring of November 2026. The first generation of the project, tagged `pre-refactor`, is summarized at the end.

Phases 1 to 3 run side by side in separate worktrees. Phases 4 and 5 build on the runtime of phase 1 and come in that order.

## Phase 0 — Ground

- [x] Tag the first generation and keep it reachable (`pre-refactor`).
- [x] Remove the region-parallel engine, the network server, the host-side player code, the extension system and the scaffolds; keep the contracts, rules, oracle, evidence, protocol, content data and storage.
- [x] Run every kept test program side by side, with the official jars required by the gate.
- [ ] A committed performance baseline: the oracle and the official server on the recorded workloads.
- [ ] A black-box harness that measures the official server, Paper and Folia on the same workloads.

## Phase 1 — Single-threaded runtime

- [ ] Inline block access in `RuleContext`, with the slow path behind the virtual call.
- [ ] A world store built for speed: hashed column index, paletted sections, active sets, time-ordered scheduled ticks, incremental chunk levels and light from changed sections.
- [ ] A strict executor in vanilla order behind a stable engine interface, equal to the oracle on every trace, scenario and seed.
- [ ] Storage and Anvil interchange on the new runtime.
- [ ] Faster than Paper on one thread on the recorded workloads, with identical results.

## Phase 2 — Networking on PACE

- [ ] PACE as a pinned xmake package; no Asio anywhere.
- [ ] Login, status and configuration as coroutines, with compression, encryption and session verification.
- [ ] A per-tick outbox with batched writes and frames encoded once for many recipients.
- [ ] Several reactors above a configurable player count.
- [ ] Limits and failure cases: timeouts, floods, slow readers, refused verification.

## Phase 3 — Entities and players in the engine

- [ ] An entity record with a common header and per-kind payloads, each with its own codec and version.
- [ ] A living-entity base (health, effects, attributes, equipment, damage) shared by players and mobs.
- [ ] Players as entities inside the rule tick, with the survival behavior of the first generation ported as rules.
- [ ] The player view: a chunk-to-viewer index, chunk and light encoding from the runtime, entity tracking, no per-tick work quadratic in players.
- [ ] A playable server again on both versions, checked by recorded packet streams and the real client.

## Phase 4 — Islands

- [ ] Islands: connected groups of chunks with their own clock, random sequences, command inbox and change feed.
- [ ] Merging and splitting islands as players, tickets and machines move, with a documented time semantics.
- [ ] Circuit's own compute scheduler over all cores; no global 20 TPS barrier.
- [ ] Per-island consistent saves and recovery.
- [ ] Faster than Folia on the same workloads; differences from vanilla documented.

## Phase 5 — Threads inside an island

- [ ] Chunk workloads with cost estimates calibrated on the running machine.
- [ ] Dividing an island whose estimated work does not fit in a tick into connected sub-islands with their own order.
- [ ] A declared treatment of dependencies between neighboring sub-islands.
- [ ] Help from idle threads on the outer chunks of a share that falls behind.
- [ ] A speedup that is positive and grows with threads on large islands.

## Phase 6 — Breadth

- [ ] Complete block, item and production families.
- [ ] World generation from parameter data with provenance.
- [ ] Mobs, spawning, villages and raids.
- [ ] Commands and data packs.
- [ ] An extension API designed for islands.
- [ ] Complete world interchange and release qualification on both versions.

## First generation (`pre-refactor`)

The first generation completed a headless simulation core with a region-parallel engine, a playable creative and survival server for both versions, persistence with crash recovery and Anvil interchange, native and Java extensions and technical tools. Its gameplay rules, evidence, protocol and storage carry over; its runtime, server and extensions are replaced by the phases above. The rules match 225 official traces on 1.21.8 and 220 on 1.21.4.
