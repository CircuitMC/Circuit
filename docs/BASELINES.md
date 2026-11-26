# Baselines

Tick times of the official server, Paper and Folia on Circuit's recorded workloads, measured from outside the servers. They are the reference the restructured Circuit is measured against: the single-threaded runtime against the official server and Paper (once Circuit runs as a server again), islands against Folia. They are not a comparison with Circuit; that comparison uses the same scripts, chunk range, game rules and timing boundary on both sides, after checking that both advanced the same ticks with the same observable results.

## Setup

- **Machine:** Intel Core Ultra 7 270K Plus, 24 logical CPUs, 188 GB, Linux 6.18 (WSL2); OpenJDK 21.0.13; every server with `-Xms4G -Xmx4G` and G1, one server at a time, nothing else running.
- **Servers:** the official servers 1.21.8 and 1.21.4 (the jars whose SHA-1 the version manifests in `compatibility/versions/` record); Paper 1.21.8 build 60 and 1.21.4 build 232; Folia 1.21.8 build 6 and 1.21.4 build 6 (the latter an alpha build), with the SHA-256 the PaperMC download service published. Released jars, unmodified; no server source was read.
- **World:** a void flat world, offline, peaceful, view and simulation distance 4, `max-tick-time=-1`; the fixture runner's game rules (`randomTickSpeed 0`, no daylight, weather or mob spawning) and `spawnChunkRadius 0`; the workload's area force-loaded with one chunk of margin.
- **Workloads:** the five recorded workloads of `tests/fixtures/workloads/` (`tools/generate_workloads.py`): 64 dust clocks, hopper lines, 256 observer clocks, piston engines and a water flood. Their `set` actions became `/setblock`, their `insert` actions a `/setblock` of the container with its items. On the official server and Paper each action landed on its fixture tick (`/tick freeze` and `/tick step`); Folia has no `/tick`, so its actions were sent in tick order.
- **Measurement:** after the fixture's settle time, a window of 600 ticks at 20 ticks per second. The official server and Paper: `/tick query` every 5 seconds (the server's average and P50, P95 and P99 over its last 100 ticks). Folia: `/tps` every 5 seconds, the MSPT of the overworld region holding the workload (Folia reports no percentiles). The game time before and after the window gave the ticks advanced; on the official server and Paper, `/execute if block` on up to 64 positions checked that the build was in place (Folia's `/execute` fails over RCON).
- **Repetitions:** three fresh servers per server, version and workload, on 2026-11-26.

## Results

Times in milliseconds per tick. "Mean" is the median over the three repetitions of each repetition's mean; P50 the median of the repetitions' medians; P95 and P99 the median of each repetition's highest sample; "Spread" is (max - min) / median of the three means.

| Server | Version | Workload | Reps | Mean ms/tick (median of reps) | P50 | P95 | P99 | Spread of means | Ticks advanced | Blocks present |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| folia | 1.21.4 | dust_clocks.ctrace | 3 | 4.690 | — | — | — | 79.0% | 607, 607, 607 | — |
| folia | 1.21.4 | hopper_lines.ctrace | 3 | 0.500 | — | — | — | 45.7% | 607, 607, 607 | — |
| folia | 1.21.4 | observer_clocks.ctrace | 3 | 0.923 | — | — | — | 9.4% | 607, 607, 607 | — |
| folia | 1.21.4 | piston_engines.ctrace | 3 | 0.998 | — | — | — | 1.3% | 607, 607, 607 | — |
| folia | 1.21.4 | water_flood.ctrace | 3 | 0.175 | — | — | — | 16.2% | 607, 607, 607 | — |
| folia | 1.21.8 | dust_clocks.ctrace | 3 | 4.880 | — | — | — | 12.5% | 607, 607, 607 | — |
| folia | 1.21.8 | hopper_lines.ctrace | 3 | 0.427 | — | — | — | 12.1% | 607, 607, 607 | — |
| folia | 1.21.8 | observer_clocks.ctrace | 3 | 1.077 | — | — | — | 22.4% | 607, 607, 607 | — |
| folia | 1.21.8 | piston_engines.ctrace | 3 | 1.192 | — | — | — | 24.5% | 607, 607, 607 | — |
| folia | 1.21.8 | water_flood.ctrace | 3 | 0.213 | — | — | — | 8.6% | 607, 607, 607 | — |
| official | 1.21.4 | dust_clocks.ctrace | 3 | 26.717 | 0.500 | 254.600 | 333.000 | 83.2% | 602, 607, 600 | 64/64, 64/64, 64/64 |
| official | 1.21.4 | hopper_lines.ctrace | 3 | 0.367 | 0.300 | 1.000 | 2.400 | 31.8% | 600, 600, 601 | 64/64, 64/64, 64/64 |
| official | 1.21.4 | observer_clocks.ctrace | 3 | 0.650 | 0.550 | 1.400 | 8.900 | 2.6% | 600, 600, 600 | 64/64, 64/64, 64/64 |
| official | 1.21.4 | piston_engines.ctrace | 3 | 0.883 | 0.700 | 2.500 | 3.800 | 7.5% | 600, 600, 601 | 64/64, 64/64, 64/64 |
| official | 1.21.4 | water_flood.ctrace | 3 | 0.233 | 0.150 | 0.700 | 1.800 | 135.7% | 601, 600, 600 | 65/65, 65/65, 65/65 |
| official | 1.21.8 | dust_clocks.ctrace | 3 | 5.733 | 0.450 | 33.400 | 62.700 | 16.3% | 600, 600, 600 | 64/64, 64/64, 64/64 |
| official | 1.21.8 | hopper_lines.ctrace | 3 | 0.483 | 0.300 | 1.500 | 3.000 | 27.6% | 600, 600, 601 | 64/64, 64/64, 64/64 |
| official | 1.21.8 | observer_clocks.ctrace | 3 | 0.767 | 0.550 | 1.900 | 3.600 | 19.6% | 601, 600, 600 | 64/64, 64/64, 64/64 |
| official | 1.21.8 | piston_engines.ctrace | 3 | 1.033 | 0.750 | 2.800 | 11.600 | 33.9% | 600, 600, 600 | 64/64, 64/64, 64/64 |
| official | 1.21.8 | water_flood.ctrace | 3 | 0.383 | 0.250 | 2.400 | 6.400 | 26.1% | 600, 600, 601 | 65/65, 65/65, 65/65 |
| paper | 1.21.4 | dust_clocks.ctrace | 3 | 6.350 | 0.600 | 46.200 | 64.000 | 19.7% | 600, 600, 601 | 64/64, 64/64, 64/64 |
| paper | 1.21.4 | hopper_lines.ctrace | 3 | 1.583 | 0.750 | 7.800 | 18.200 | 44.2% | 600, 600, 600 | 64/64, 64/64, 64/64 |
| paper | 1.21.4 | observer_clocks.ctrace | 3 | 0.917 | 0.800 | 2.300 | 16.200 | 67.3% | 600, 600, 600 | 64/64, 64/64, 64/64 |
| paper | 1.21.4 | piston_engines.ctrace | 3 | 1.300 | 1.000 | 3.600 | 16.800 | 71.8% | 600, 600, 600 | 64/64, 64/64, 64/64 |
| paper | 1.21.4 | water_flood.ctrace | 3 | 0.550 | 0.450 | 1.800 | 16.800 | 97.0% | 601, 600, 601 | 65/65, 65/65, 65/65 |
| paper | 1.21.8 | dust_clocks.ctrace | 3 | 5.067 | 0.650 | 32.300 | 37.800 | 7.9% | 600, 601, 600 | 64/64, 64/64, 64/64 |
| paper | 1.21.8 | hopper_lines.ctrace | 3 | 0.800 | 0.600 | 3.200 | 19.000 | 6.3% | 601, 600, 600 | 64/64, 64/64, 64/64 |
| paper | 1.21.8 | observer_clocks.ctrace | 3 | 0.967 | 0.750 | 2.700 | 18.200 | 12.1% | 600, 601, 600 | 64/64, 64/64, 64/64 |
| paper | 1.21.8 | piston_engines.ctrace | 3 | 1.183 | 0.950 | 3.600 | 14.900 | 16.9% | 600, 600, 600 | 64/64, 64/64, 64/64 |
| paper | 1.21.8 | water_flood.ctrace | 3 | 0.533 | 0.400 | 1.800 | 17.000 | 9.4% | 600, 600, 600 | 65/65, 65/65, 65/65 |

## Reading them

- **Every run kept 20 ticks per second.** The ticks advanced are the window's 600 (Folia's 607 include its sampling overhead), and every checked block was in place.
- **The dust clocks are bursty.** Their median tick is under 1 ms on every server, but the clocks' periodic updates give P95 values of 30 to 50 ms, and on the official 1.21.4 server 255 ms, with a mean of 26.7 ms and a spread of 83 % across the repetitions. Means alone hide this; comparisons use the percentiles.
- **Small values spread widely.** The water flood and hopper lines take well under 1 ms a tick; their relative spread (up to 136 %) is noise around small numbers, and differences between servers under about 0.3 ms are not meaningful at this sample size.
- **Folia's numbers are its region's MSPT,** a different report from the official server's tick query; they are comparable with each other, and with the others only in their mean.
- **The first generation's engine** measured 0.12 to 2.49 ms on the same workloads at one worker (the simulation guide's workload baseline), inside a test harness without networking or chunk sending; those numbers are not comparable with whole servers either.

## Reproducing

The measuring tool lives outside this repository because it runs the servers' jars, which require the Minecraft EULA. It drives each server over RCON as described above; its output is one JSON record per run with every raw report.
