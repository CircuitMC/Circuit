# Storage evaluation

This note records the measurements behind the checkpoint encoding and the background checkpoints described in [persistence](PERSISTENCE.md). `storage_bench` (`tools/bench/storage_bench.cpp`) reproduces them:

```bash
xmake f -m release -y && xmake build storage_bench
build/linux/x86_64/release/storage_bench --chunks 64 --anvil /path/to/a/java/world
```

The numbers below come from a release build (GCC 13) on an Intel Core Ultra 7 270K Plus with 24 threads under WSL 2. File system times include `fsync` on the WSL virtual disk, which is slower than a native NVMe drive. Each time is the best of three runs.

## Worlds

| World | Columns | Blocks | Body (uncompressed) |
| --- | --- | --- | --- |
| `many_machines` scenario | 121 | 1,215 | 191 KiB |
| `drops_and_pushes` scenario | 49 | 925 | 51 KiB |
| Synthetic redstone server: a four-layer floor in every column, lamp lines in a quarter of them, stocked chests | 4,096 | 4,211,200 | 4,044 KiB |
| Dense random blocks: worst case for palettes | 64 | 1,048,576 | 395 KiB |
| A vanilla flat world, imported with substitution | 169 | 173,056 | 172 KiB |

Section palettes make the uncompressed body about 1 KiB per non-empty section for ordinary worlds and 1.6 KiB for random content.

## Compression

Synthetic redstone server, 4,044 KiB body:

| Codec | Size (KiB) | Compress (ms) | Decompress (ms) |
| --- | --- | --- | --- |
| zlib 1 | 69.1 | 3.7 | 1.5 |
| zlib 6 | 39.7 | 9.0 | 2.5 |
| zlib 9 | 32.4 | 40.9 | 2.4 |
| zstd 1 | 16.9 | 0.3 | 0.2 |
| zstd 3 | 12.1 | 0.3 | 0.2 |
| LZ4 | 45.5 | 0.2 | 0.1 |

The other worlds rank the codecs the same way. Random content does not compress: about 7 % with any codec.

**Decision: zlib level 1** (`CheckpointOptions::compression_level`). Compression costs little whichever codec is chosen; the other steps (below) cost far more. Level 1 is two to three times faster than level 6, and the files stay small: tens of KiB for thousands of columns.

zstd would be smaller and faster still. It is not used because it would add a build dependency for a saving that does not matter at these sizes. The benchmark measures zstd and LZ4 through the system libraries when they are present; Circuit does not link them. If checkpoints grow to sizes where compression matters, the format can name its codec in a new version.

## Where the time goes

Synthetic redstone server, 4,096 columns:

| Step | Time |
| --- | --- |
| Engine dump (one entry per block) | 306 ms |
| Encode the dump as a checkpoint | 83 ms |
| **Snapshot** (copy-on-write sections) | **0.9 ms** |
| Encode the snapshot, empty section cache | 39 ms |
| Encode the snapshot, no section changed | 5.4 ms |
| Write the checkpoint into a world directory, with `fsync` | 123 ms |
| Recovery: read, decode and restore a running engine | 379 ms |

A checkpoint built from a dump would stop the game thread for about 400 ms in this world, eight ticks. The server therefore checkpoints from a snapshot:
1. **On the game thread:** a snapshot of the engine, then the start of the new log segment (`WorldDirectory::begin_checkpoint`).
2. **On a writer thread:** encoding and writing (`finish_checkpoint`), while the game logs and runs further ticks.

The engine copies a section before writing to it while a snapshot holds it, so the snapshot stays fixed.

The game thread's share falls to the snapshot plus the four `fsync` calls that start the segment: the old segment, the new one, the directory and the manifest. All are needed before a later synced log record can count as durable. The server logs both times for each checkpoint. On the WSL disk, the game thread spends 4–26 ms per checkpoint, nearly all of it in `fsync`. A real client played a redstone script while autosaves ran every 40 ticks.

Recovery still goes through the per-block dump and takes most of its time there. It runs once, at startup.

## Deltas

Share of columns whose blocks or block entities changed between two checkpoints:

| Workload | Interval | Columns changed |
| --- | --- | --- |
| `observer_clocks` | 20 ticks | 3.2 % |
| `drops_and_pushes` | 20 ticks | 10.4 % |
| `many_machines` (machines in every column) | 20 and 200 ticks | 66.9 % |

A world with local activity changes a small part of its columns between checkpoints, so incremental checkpoints could save work. The work they would save is, however, encoding and not writing: files are small, and writing them is bound by `fsync` latency rather than size.

**Decision: an exact section cache, no on-disk deltas.**
- **The cache.** The writer keeps the encoding of each section it has seen, keyed by the shared, immutable section object itself, which the cache keeps alive. An unchanged section is never encoded twice: in the large world, encoding falls from 39 ms to 5.4 ms when nothing changed.
- **Exactness.** Identity of an immutable object is exact, so no hash can collide and no baseline can be missing.
- **The format.** On disk every checkpoint stays a complete snapshot, so recovery never depends on an older file.

On-disk deltas would need a retained or reproducible baseline, validated by identity and content hash, and they would make recovery depend on more files. Reconsider them if checkpoint files grow large enough for size to matter.

## Tests

`storage_tests` checks that encoding a snapshot gives the same bytes as encoding the dump of the same state. It covers:
- a cold section cache, a warm one, and none;
- the engine running on after the snapshot was taken.

Crash injection covers checkpoints finished three ticks after they began, with log records appended in between. Every recovered image holds every durable tick.
