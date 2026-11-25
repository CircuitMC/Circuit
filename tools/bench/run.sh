#!/usr/bin/env bash
# Runs engine_bench over workloads and worker counts and writes Markdown tables.
#
#   tools/bench/run.sh [options] [workload[@ticks] ...]
#     --reps N        repetitions of every cell (3)
#     --workers LIST  worker counts, quoted and separated by spaces ("1 2 4 8 16")
#     --ticks N       timed ticks of a workload that names none (600)
#     --first-cpu N   a run with W workers is pinned to CPUs N to N+W-1 (8)
#     --out DIR       output directory (build/bench/<UTC date and time>)
#     --task-times    pass --task-times to engine_bench (rule work and critical path)
#     --build         configure the release mode and build engine_bench first
#     --bin FILE      the engine_bench program to run (the release build of this tree)
#     --label TEXT    copied into every output line (-)
#     --list          print the default workloads and exit
#
# A workload is a .ctrace or .wl file, or the name of a recorded workload
# (tests/fixtures/workloads) or of a constructed one (tools/bench/workloads.py).
# Without workloads the baseline set of docs/PERFORMANCE.md runs.
#
# The program must be a release build:
#   xmake f -m release -y && xmake build -y engine_bench
# (--build does this; the tree is then configured for release, and
# tools/check.sh restores the debug configuration.)
#
# Output: runs.txt (one RUN line per run), machine.txt, and table.md with the
# median of the repetitions per cell. The exit status is 1 when a workload's
# digest differs between runs.
set -euo pipefail
cd "$(dirname "$0")/../.."

reps=3 workers="1 2 4 8 16" ticks=600 first_cpu=8 out="" build=0 list=0 bin="" label=-
extra=()
workloads=()
while (($#)); do
    case "$1" in
        --reps) reps=$2; shift 2 ;;
        --workers) workers=$2; shift 2 ;;
        --ticks) ticks=$2; shift 2 ;;
        --first-cpu) first_cpu=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --task-times) extra+=(--task-times); shift ;;
        --build) build=1; shift ;;
        --bin) bin=$2; shift 2 ;;
        --label) label=$2; shift 2 ;;
        --list) list=1; shift ;;
        -h|--help) sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --*) echo "run.sh: unknown option $1" >&2; exit 2 ;;
        *) workloads+=("$1"); shift ;;
    esac
done
if ((${#workloads[@]} == 0)); then
    # The idle worlds are steady, and the largest takes over 100 ms a tick:
    # 200 timed ticks are enough for them.
    workloads=(observer_clocks dust_clocks hopper_lines piston_engines water_flood
               iso_observer_16 iso_dust_16 iso_hopper_16 iso_piston_16 iso_dropper_16 iso_items_16
               idle_16_stone idle_16_grass players_1@200 players_4@200 players_16@200 players_64@200)
fi
if ((list)); then printf '%s\n' "${workloads[@]}"; exit 0; fi

if ((build)); then
    args=(-m release -y)
    if [[ -n "${CIRCUIT_ASIO_INCLUDE:-}" ]]; then args+=("--asio_include=$CIRCUIT_ASIO_INCLUDE"); fi
    export XMAKE_GLOBALDIR="${XMAKE_GLOBALDIR:-$PWD/.xmake-global}"
    xmake f "${args[@]}"
    xmake build -y engine_bench
fi
if [[ -z "$bin" ]]; then
    bin=$(find build -path '*/release/engine_bench' -type f 2>/dev/null | head -n 1)
fi
if [[ -z "$bin" || ! -x "$bin" ]]; then
    echo "run.sh: no release build of engine_bench; run with --build" >&2
    exit 2
fi

generated=build/bench/workloads
python3 tools/bench/workloads.py "$generated" >/dev/null
out=${out:-build/bench/$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$out"
: > "$out/runs.txt"

cpus=$(nproc --all)
{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)$(git diff --quiet HEAD 2>/dev/null || echo ' (modified)')"
    echo "binary: $bin"
    echo "cpu: $(sed -n 's/^model name[^:]*: //p' /proc/cpuinfo | head -n 1), $cpus logical CPUs"
    echo "kernel: $(uname -sr)"
    echo "compiler: $(g++ --version 2>/dev/null | head -n 1 || echo unknown)"
    echo "repetitions: $reps; workers: $workers; first CPU: $first_cpu"
    echo "load average before: $(cut -d' ' -f1-3 /proc/loadavg)"
} > "$out/machine.txt"

for item in "${workloads[@]}"; do
    name=${item%@*}
    n=$ticks
    if [[ "$item" == *@* ]]; then n=${item##*@}; fi
    if [[ -f "$name" ]]; then file=$name
    elif [[ -f "tests/fixtures/workloads/$name.ctrace" ]]; then file=tests/fixtures/workloads/$name.ctrace
    elif [[ -f "$generated/$name.wl" ]]; then file=$generated/$name.wl
    else echo "run.sh: no workload $name" >&2; exit 2
    fi
    # One pass over the worker counts per repetition, so that a change of the
    # machine's state during a workload reaches every worker count and shows
    # in the spread instead of looking like an effect of the worker count.
    for ((r = 1; r <= reps; ++r)); do
        for w in $workers; do
            last=$((first_cpu + w - 1))
            if ((last >= cpus)); then
                echo "run.sh: $w workers from CPU $first_cpu need CPU $last; the machine has $cpus" >&2
                exit 2
            fi
            taskset -c "$first_cpu-$last" "$bin" "$file" --workers "$w" --ticks "$n" --label "$label" "${extra[@]}" >> "$out/runs.txt"
        done
    done
    echo "done: $name ($(cut -d' ' -f1-3 /proc/loadavg))" >&2
done
echo "load average after: $(cut -d' ' -f1-3 /proc/loadavg)" >> "$out/machine.txt"

status=0
{
    echo "## Tick time"
    echo
    python3 tools/bench/table.py "$out/runs.txt" scaling
    echo
    echo "## Workloads and results"
    echo
    python3 tools/bench/table.py "$out/runs.txt" digests || status=1
    echo
    echo "## Time per phase (microseconds per tick)"
    echo
    python3 tools/bench/table.py "$out/runs.txt" phases
    echo
    echo "## Machine"
    echo
    sed 's/^/- /' "$out/machine.txt"
} > "$out/table.md"
echo "$out/table.md"
exit $status
