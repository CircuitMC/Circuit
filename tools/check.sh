#!/usr/bin/env bash
# Framework checks of this tree.
#
#   tools/check.sh           configure debug, build every target, run the test programs one after
#                            another, check the compatibility ledger
#   tools/check.sh --lane    incremental debug build with zero warnings, then every test program
#                            side by side with the official jars required; one line per step and
#                            a non-zero exit when any step fails or does not run
#   tools/check.sh --full    --lane plus the ledger checks
#
# Options of --lane and --full:
#   --jobs N        compile jobs (default CIRCUIT_BUILD_JOBS or 6)
#   --test-jobs N   test programs side by side (default CIRCUIT_TEST_JOBS or 6)
#   --logs DIR      log directory (default build/gate/<mode>)
#   --rebuild       compile every file again, so that every file can warn
set -euo pipefail
cd "$(dirname "$0")/.."

usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; }

gate=
jobs=${CIRCUIT_BUILD_JOBS:-6}
test_jobs=${CIRCUIT_TEST_JOBS:-6}
logs=
rebuild=0
while (($#)); do
    case $1 in
        --lane) gate=lane ;;
        --full) gate=full ;;
        --jobs) jobs=${2:?--jobs needs a number}; shift ;;
        --test-jobs) test_jobs=${2:?--test-jobs needs a number}; shift ;;
        --logs) logs=${2:?--logs needs a directory}; shift ;;
        --rebuild) rebuild=1 ;;
        -h | --help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

if [[ -z $gate ]]; then
    export XMAKE_GLOBALDIR="${XMAKE_GLOBALDIR:-$PWD/.xmake-global}"
    xmake f -m debug -y
    xmake build -y --all
    xmake run contract_tests
    xmake run rules_tests
    xmake run evidence_tests
    xmake run protocol_tests
    xmake run data_tests
    xmake run content_tests
    xmake run storage_tests
    xmake run anvil_tests
    python3 tools/compat_ledger.py check
    python3 tests/tools/compat_ledger_tests.py
    exit 0
fi

[[ $jobs =~ ^[1-9][0-9]*$ && $test_jobs =~ ^[1-9][0-9]*$ ]] || { usage >&2; exit 2; }

# A worktree made by tools/worktree.sh carries its slot's settings.
if [[ -f .circuit-worktree.env ]]; then
    # shellcheck disable=SC1091
    source ./.circuit-worktree.env
fi
export XMAKE_GLOBALDIR="${XMAKE_GLOBALDIR:-$PWD/.xmake-global}"
# shellcheck source=tools/gate/lib.sh
source tools/gate/lib.sh
set +e

tree=$PWD
logs=${logs:-$tree/build/gate/$gate}
mkdir -p "$logs" || exit 1
logs=$(cd "$logs" && pwd)
started=$(gate_now)
echo "== tools/check.sh --$gate in $tree ($(git rev-parse --short HEAD 2>/dev/null || echo 'no commit')), logs in $logs"

gate_require_jars ""
if gate_build "" "$tree" debug "$jobs" "$logs" 0 1; then
    built=1
else
    built=0
fi
if ((built && rebuild)); then
    # A second pass that compiles every file, so warnings of files the first pass skipped show.
    (cd "$tree" && timeout "${CIRCUIT_GATE_BUILD_TIMEOUT:-3600}" xmake build -y -r -j "$jobs" --all) >"$logs/rebuild-debug.log" 2>&1
    rc=$?
    warnings=$(grep -c 'warning:' "$logs/rebuild-debug.log")
    if ((rc != 0 || warnings != 0)); then
        gate_record FAIL rebuild-warnings 0 "exit $rc, $warnings warnings in $logs/rebuild-debug.log"
        built=0
    else
        gate_record PASS rebuild-warnings 0 "0 warnings in $(grep -c 'compiling\.debug' "$logs/rebuild-debug.log") compiled files"
    fi
fi
if ((built)); then
    gate_run_programs "" "$tree" debug "$test_jobs" "$logs" 1
else
    for program in "${GATE_PROGRAMS[@]}"; do
        gate_record NOT-RUN "$program" 0 "the build did not pass"
    done
fi

if [[ $gate == full ]]; then
    gate_tree_checks "" "$tree" "$logs"
fi

gate_summary "tools/check.sh --$gate: $(($(gate_now) - started)) s"
