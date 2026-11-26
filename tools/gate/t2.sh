#!/usr/bin/env bash
# T2, the merge gate: builds and tests copies of one tree, never the tree itself.
#
#   tools/gate/t2.sh --tree <checkout> --work <directory> [options]
#
#   --tree DIR         the checkout that holds the candidate
#   --work DIR         where the copies, their builds and the logs go (kept between runs, so a
#                      sanitizer tree is rebuilt incrementally; delete it to start cold)
#   --rev REV          the commit to test (default HEAD of --tree); the copy is that commit's
#                      tracked files only
#   --working-files    test the files as they are in --tree instead of a commit
#   --trees "LIST"     which copies to make, from: debug asan ubsan tsan (default: all four)
#   --jobs N           compile jobs per tree (default CIRCUIT_BUILD_JOBS or 6)
#   --test-jobs N      test programs side by side per tree (default CIRCUIT_TEST_JOBS or 6)
#   --side-by-side N   trees at a time (default 1)
#
# The debug copy is built from nothing with the compiler cache off and must have zero warnings;
# it also runs the ledger checks. Every copy runs every test
# program with the official jars required. The gate scripts are the ones next to this file,
# not the candidate's. One line per step; exit status 0 only when every step passed.
set -u

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; }

gate_directory=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tools/gate/lib.sh
source "$gate_directory/lib.sh"

tree=
work=
rev=HEAD
working_files=0
trees="debug asan ubsan tsan"
jobs=${CIRCUIT_BUILD_JOBS:-6}
test_jobs=${CIRCUIT_TEST_JOBS:-6}
side_by_side=1
while (($#)); do
    case $1 in
        --tree) tree=${2:-}; shift ;;
        --work) work=${2:-}; shift ;;
        --rev) rev=${2:-}; shift ;;
        --working-files) working_files=1 ;;
        --trees) trees=${2:-}; shift ;;
        --jobs) jobs=${2:-}; shift ;;
        --test-jobs) test_jobs=${2:-}; shift ;;
        --side-by-side) side_by_side=${2:-}; shift ;;
        -h | --help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done
[[ -n $tree && -n $work && -n $trees ]] || { usage >&2; exit 2; }
[[ $jobs =~ ^[1-9][0-9]*$ && $test_jobs =~ ^[1-9][0-9]*$ && $side_by_side =~ ^[1-9][0-9]*$ ]] || { usage >&2; exit 2; }
for kind in $trees; do
    case $kind in debug | asan | ubsan | tsan) ;; *) echo "t2.sh: unknown tree '$kind'" >&2; exit 2 ;; esac
done
tree=$(cd "$tree" 2>/dev/null && pwd) || { echo "t2.sh: --tree is not a directory" >&2; exit 2; }
mkdir -p "$work" || exit 2
work=$(cd "$work" && pwd)
case $work/ in "$tree"/*) echo "t2.sh: --work must be outside --tree" >&2; exit 2 ;; esac
command -v rsync >/dev/null || { echo "t2.sh: rsync is required" >&2; exit 2; }

export XMAKE_GLOBALDIR="${XMAKE_GLOBALDIR:-$work/.xmake-global}"
excludes=(--exclude=/build --exclude=/.xmake --exclude=/.xmake-global --exclude=/.local --exclude=/.git
          --exclude=/.circuit-worktree.env)
started=$(gate_now)

# The files under test: a commit's tracked files, or the working files.
if ((working_files)); then
    source_directory=$tree
    label="working files of $tree"
else
    commit=$(git -C "$tree" rev-parse --verify --quiet "$rev^{commit}") || { echo "t2.sh: $rev is not a commit in $tree" >&2; exit 2; }
    source_directory=$work/source
    rm -rf "$source_directory"
    mkdir -p "$source_directory"
    git -C "$tree" archive --format=tar "$commit" | tar -x -C "$source_directory" || { echo "t2.sh: cannot export $commit" >&2; exit 2; }
    label="commit ${commit:0:12} of $tree"
    if [[ -n $(git -C "$tree" status --porcelain) ]]; then
        echo "note: $tree has uncommitted changes; they are not part of this gate"
    fi
fi
echo "== T2 merge gate: $label; trees: $trees; work directory $work"

# The steps every tree must report; one that is missing afterwards did not run.
expected_steps() {
    local kind=$1 program
    echo "$kind/copy"
    echo "$kind/build"
    if [[ $kind == debug ]]; then echo "$kind/warnings"; fi
    for program in "${GATE_PROGRAMS[@]}"; do echo "$kind/$program"; done
    if [[ $kind == debug ]]; then
        echo "$kind/ledger"
        echo "$kind/ledger-tools"
    fi
}

run_tree() {
    local kind=$1 copy=$work/$1/tree logs=$work/$1/logs difference start clean=0 zero_warnings=0
    gate_steps=()
    mkdir -p "$copy" "$logs"
    rm -f "$logs"/*.rc "$work/$kind/steps"
    start=$(gate_now)
    if ! rsync -rlpc --delete "${excludes[@]}" "$source_directory/" "$copy/" >"$logs/copy.log" 2>&1; then
        gate_record FAIL "$kind/copy" $(($(gate_now) - start)) "rsync failed, see $logs/copy.log"
    else
        difference=$(rsync -rlpcn --delete --itemize-changes "${excludes[@]}" "$source_directory/" "$copy/" | grep -v '^\.d' | head -3)
        if [[ -n $difference ]]; then
            gate_record FAIL "$kind/copy" $(($(gate_now) - start)) "the copy differs: $difference"
        else
            gate_record PASS "$kind/copy" $(($(gate_now) - start)) "$copy equals the $label"
            if [[ $kind == debug ]]; then
                clean=1
                zero_warnings=1
            fi
            if gate_build "$kind/" "$copy" "$kind" "$jobs" "$logs" "$clean" "$zero_warnings"; then
                gate_run_programs "$kind/" "$copy" "$kind" "$test_jobs" "$logs" 1
            fi
            if [[ $kind == debug ]]; then
                gate_tree_checks "$kind/" "$copy" "$logs"
            fi
        fi
    fi
    printf '%s\n' "${gate_steps[@]}" >"$work/$kind/steps"
}

gate_require_jars ""
jar_steps=("${gate_steps[@]}")

running=0
for kind in $trees; do
    while ((running >= side_by_side)); do
        wait -n
        running=$((running - 1))
    done
    mkdir -p "$work/$kind"
    rm -f "$work/$kind/steps"
    (run_tree "$kind") &
    running=$((running + 1))
done
wait

# Collect what every tree reported; anything expected and absent did not run.
gate_steps=("${jar_steps[@]}")
for kind in $trees; do
    reported=()
    if [[ -s $work/$kind/steps ]]; then
        mapfile -t reported <"$work/$kind/steps"
    fi
    gate_steps+=("${reported[@]}")
    while read -r expected; do
        found=0
        for line in "${reported[@]}"; do
            IFS='|' read -r _ name _ <<<"$line"
            if [[ $name == "$expected" || $name == "$expected["* ]]; then
                found=1
                break
            fi
        done
        if ((!found)); then
            gate_steps+=("NOT-RUN|$expected|0|an earlier step of the $kind tree did not pass")
        fi
    done < <(expected_steps "$kind")
done

gate_summary "T2 merge gate, $label: $(($(gate_now) - started)) s"
