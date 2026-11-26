#!/usr/bin/env bash
# Tests of gate_program_list (tools/gate/lib.sh): the gates must fail when a test program of the
# build is missing from GATE_PROGRAMS, or when GATE_PROGRAMS names a program the build lacks.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=tools/gate/lib.sh
source "$root/tools/gate/lib.sh"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0

expect() {
    local want=$1 label=$2 tree=$3 rc
    gate_steps=()
    gate_program_list "" "$tree" >"$work/out" 2>&1
    rc=$?
    if [[ $want == pass && $rc -ne 0 ]] || [[ $want == fail && $rc -eq 0 ]]; then
        echo "FAIL $label: expected $want, got exit $rc: $(cat "$work/out")"
        failures=$((failures + 1))
    else
        echo "ok   $label: $(cut -c1-100 "$work/out")"
    fi
}

# The repository as it is.
expect pass "the tree's test targets are the gate's programs" "$root"

# A new test target that the gate list does not name.
mkdir -p "$work/added"
cp "$root/xmake.lua" "$work/added/xmake.lua"
printf '\ntarget("newly_added_tests")\n    set_kind("binary")\ntarget_end()\n' >>"$work/added/xmake.lua"
expect fail "a test target missing from GATE_PROGRAMS" "$work/added"

# A program the gate list names that is no longer built.
mkdir -p "$work/removed"
grep -v '^target("rules_tests")' "$root/xmake.lua" >"$work/removed/xmake.lua"
expect fail "a gate program that is no longer a target" "$work/removed"

# No build file at all.
mkdir -p "$work/empty"
expect fail "a tree without xmake.lua" "$work/empty"

# A program dropped from the gate list.
saved=("${GATE_PROGRAMS[@]}")
GATE_PROGRAMS=("${saved[@]:1}")
expect fail "a program dropped from GATE_PROGRAMS" "$root"
GATE_PROGRAMS=("${saved[@]}")

if ((failures)); then
    echo "gate_program_list_tests: $failures failed"
    exit 1
fi
echo "gate_program_list_tests: OK"
