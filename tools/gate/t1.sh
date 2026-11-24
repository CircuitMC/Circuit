#!/usr/bin/env bash
# T1, the lane step gate: tools/check.sh --lane, then the client scenarios named on the
# command line, run against a copy of the server this tree just built.
#
#   tools/gate/t1.sh [check options] [--tag TAG]... [scenario...]
#
#   check options: --jobs N, --test-jobs N, --logs DIR, --rebuild (see tools/check.sh)
#
# With no scenario and no tag the gate is the lane check alone. Scenarios need, from the
# environment (a slot's .circuit-worktree.env provides the first two and the jars):
#   CIRCUIT_PORT_BASE            the slot's first port; the server listens on base + 2
#   CIRCUIT_WORK                 the slot's work directory
#   CIRCUIT_SCENARIO_MANIFEST    the manifest that pins one reference per scenario and version
#   CIRCUIT_PROBE_DIR            the directory of the probes
#   CIRCUIT_REFERENCE_DIR        the directory of the official reference outputs
#   CIRCUIT_SCENARIO_CPUS        optional CPU list that keeps the probes off busy cores
# One line per step; exit status 0 only when every step passed.
set -u

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; }

gate_directory=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$gate_directory/../.." && pwd)
cd "$tree" || exit 2
if [[ -f .circuit-worktree.env ]]; then
    # shellcheck disable=SC1091
    source ./.circuit-worktree.env
fi
# shellcheck source=tools/gate/lib.sh
source "$gate_directory/lib.sh"

check_options=()
selection=()
while (($#)); do
    case $1 in
        --jobs | --test-jobs | --logs) check_options+=("$1" "${2:?$1 needs a value}"); shift ;;
        --rebuild) check_options+=("$1") ;;
        --tag) selection+=(--tag "${2:?--tag needs a name}"); shift ;;
        -h | --help) usage; exit 0 ;;
        -*) usage >&2; exit 2 ;;
        *) selection+=("$1") ;;
    esac
    shift
done

started=$(gate_now)
start=$(gate_now)
bash tools/check.sh --lane "${check_options[@]}"
lane=$?
echo
if ((lane == 0)); then
    gate_record PASS lane $(($(gate_now) - start)) "tools/check.sh --lane"
else
    gate_record FAIL lane $(($(gate_now) - start)) "tools/check.sh --lane exited $lane"
fi

run_scenarios() {
    local variable missing=() work out server binary version jar
    for variable in CIRCUIT_PORT_BASE CIRCUIT_WORK CIRCUIT_SCENARIO_MANIFEST CIRCUIT_PROBE_DIR CIRCUIT_REFERENCE_DIR \
                    "${GATE_JAR_VARIABLES[@]}"; do
        [[ -n ${!variable:-} ]] || missing+=("$variable")
    done
    if ((${#missing[@]})); then
        gate_record NOT-RUN scenarios 0 "not set: ${missing[*]}"
        return 1
    fi
    if ((lane != 0)); then
        gate_record NOT-RUN scenarios 0 "the lane check did not pass"
        return 1
    fi
    if ! binary=$(gate_binary_directory "$tree" debug) || [[ ! -x $binary/circuit-server ]]; then
        gate_record NOT-RUN scenarios 0 "no built circuit-server in $tree"
        return 1
    fi
    work=$CIRCUIT_WORK/t1
    out=$work/out
    rm -rf "$out"
    mkdir -p "$out" || return 1
    # A copy, so that a later build cannot replace the server under a running probe.
    server=$work/circuit-server
    if ! cp "$binary/circuit-server" "$server.new" || ! mv "$server.new" "$server"; then
        gate_record NOT-RUN scenarios 0 "cannot copy the server to $work"
        return 1
    fi
    for version in 1_21_8 1_21_4; do
        jar=CIRCUIT_VANILLA_JAR_$version
        printf 'game-version=java-%s\nvanilla-server-jar=%s\n' "${version//_/.}" "${!jar}" >"$work/$version.properties"
    done
    start=$(gate_now)
    python3 "$gate_directory/scenarios.py" run --manifest "$CIRCUIT_SCENARIO_MANIFEST" \
        --references "$CIRCUIT_REFERENCE_DIR" --probes "$CIRCUIT_PROBE_DIR" --server "$server" \
        --config "1.21.8=$work/1_21_8.properties" --config "1.21.4=$work/1_21_4.properties" \
        --port $((CIRCUIT_PORT_BASE + 2)) --work "$work" --out "$out" \
        ${CIRCUIT_SCENARIO_CPUS:+--cpus "$CIRCUIT_SCENARIO_CPUS"} "${selection[@]}"
    local rc=$?
    echo
    if ((rc == 0)); then
        gate_record PASS scenarios $(($(gate_now) - start)) "${selection[*]}; outputs in $out"
    else
        gate_record FAIL scenarios $(($(gate_now) - start)) "scenarios.py exited $rc; outputs in $out"
        return 1
    fi
}

if ((${#selection[@]})); then
    run_scenarios
else
    echo "no client scenario named: the gate is the lane check alone"
fi

gate_summary "T1 lane step gate in $tree: $(($(gate_now) - started)) s"
