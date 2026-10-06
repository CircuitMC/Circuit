# Releases and operation

## What a release contains

A release is a tagged commit of this repository and the build made from it. It includes:
- **Source and documents:** the source code, tests, build files and public documents.
- **Notices:** `LICENSE` (AGPL-3.0-only), `LICENSING.md` and `THIRD_PARTY_NOTICES.md`.

It contains no Minecraft code, assets, data generator output or mappings. The protocol and content tables are compiled from public documentation and the MIT-licensed minecraft-data dataset; `THIRD_PARTY_NOTICES.md` and `docs/SERVER.md` name the sources.

Binary builds link these system libraries:
- **zlib**, under the zlib License;
- **OpenSSL 3**, under Apache-2.0;
- **standalone Asio 1.34.2**, header-only, under the Boost Software License 1.0.

Ship their notices with any binary distribution. The benchmark tool can load zstd and LZ4 at run time when the system has them; neither is linked or distributed.

## Release checklist

1. **Clean tree:** `git status` is clean. `python3 tools/check_public_tree.py --staged` and the pre-push guard pass.
2. **Debug check:** `bash tools/check.sh` passes. It covers every test program and the Java bridge with a JDK present.
3. **Sanitizers:** ASan, UBSan and TSan builds of all test programs pass with no reports. Run TSan under `setarch -R`; see [the development guide](DEVELOPMENT.md#validation).
4. **External clients,** for each supported version:
   - the mineflayer suite against the vanilla server and against Circuit;
   - the real-client scripts through the client harness.

   See [client testing](DEVELOPMENT.md#client-testing).
5. **Target-version traces:** the traces of each supported version pass (`evidence_tests`). Re-record them when a skeleton changes.
6. **Version identity:** the version string (`circuit-server --version`) names the release. The verification record names the commit, the toolchain (GCC 13, xmake 3) and the dependency versions.
7. **Signed tag:** the tag and the release digests are signed by the project owner. Contributor rights for alternative licensing are confirmed as `LICENSING.md` requires.

## Supported scope

| Area | Supported | Not supported |
| --- | --- | --- |
| Versions | Java Edition 1.21.8 (protocol 772) and 1.21.4 (protocol 769), one per server | Other versions; clients of another protocol are refused |
| Gameplay | Creative building with the blocks the reference rules model; redstone, pistons, hoppers, chests, water, crops and item entities under circuit-reference-rules-v5 | Survival, mobs, container screens, lighting, other dimensions |
| Identity | Online mode with session verification; explicitly unverified offline mode | Signed chat |
| Worlds | World directories with checkpoints and an input log; Anvil import and export of 1.21.8 worlds | Anvil interchange for 1.21.4 |
| Extensions | Native modules (trusted code), Java plugins in a separate JVM | The Bukkit, Spigot or Paper APIs |

[The simulation guide](SIMULATION.md) lists the compatibility tier and known gaps of each mechanism.

## Operating limits

These are the measured or configured limits; plan capacity with margin:

| Resource | Limit or cost |
| --- | --- |
| Memory | About 16 KiB per non-empty 16×16×16 section, plus block entities and entities. A column with all 24 sections filled holds 384 KiB |
| Players | `max-players` (default 20) and `max-connections` (256). Tested with up to three clients at once |
| Tick time | See [performance](PERFORMANCE.md). A connected machine runs on one thread; independent machines in separate regions run in parallel |
| Checkpoint pause | The game thread takes a snapshot (about 1 ms per 4,000 columns) and starts a log segment: four `fsync` calls (old segment, new segment, directory, manifest), about 20 ms on the WSL test disk. Encoding and writing run on a writer thread |
| Recovery | About 90 ms per 1,000 columns of decoding and restoring, plus the replay of logged ticks since the checkpoint ([storage evaluation](STORAGE_EVALUATION.md)) |
| Input log | One record per tick, synced every `log-sync-ticks`. A crash loses at most the inputs since the last sync |
| Chunk sending | 32 chunks per player per tick, encoded in parallel |
| Anvil import | 65,536 chunks and 64 MiB of NBT per chunk by default |
| Java bridge | Frames up to 1 MiB, 4,096 queued messages, an 8 MiB outbox, 1,024 requests answered per tick; stopped `java-max-lag-ticks` (200) ticks behind |

## Upgrades, migration and recovery

- **Rule-set changes:** a world directory names the bundle and the rule set it was saved under. A server with other rules refuses to open it rather than reinterpret the state.
- **Moving to another supported version with the same rules:** use `--migrate-world`; the source is only read. See [persistence](PERSISTENCE.md#migrating-between-versions).
- **Back up a world directory:** copy it while the server is stopped, or copy the files the manifest names while it runs. A copy taken while the server runs may need the log tail discarded on recovery, which is reported.
- **After a crash:** start the server on the same directory. It recovers the latest durable state, reports what it discarded, and keeps damaged files as `damaged-<name>`.
- **If recovery refuses a world:** the error names the cause, for example no readable checkpoint, a corrupt log record or another rule set. The files are left untouched for diagnosis. Export the latest state of a healthy copy with `--export-anvil`, or restore a backup.
