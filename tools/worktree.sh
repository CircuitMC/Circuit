#!/usr/bin/env bash
# Linked worktrees for parallel lanes.
#
# Slot k (1 to 4) is the directory <main>-wt-<k> next to the main checkout, on branch
# lane/<lane>. Each slot has its own build directory, a port range and a work directory, and
# reads the git-ignored governing documents of the main checkout through $CIRCUIT_MAIN; they
# are never copied into a worktree.
#
#   tools/worktree.sh add <k> <lane> [--no-configure]   create slot k on lane/<lane> (from main,
#                                     or attach the branch when it exists); <lane> with a slash
#                                     names an existing branch to attach as it is
#   tools/worktree.sh env <k> [--write]                 print the slot's exports; --write stores
#                                     them again in the slot's .circuit-worktree.env
#   tools/worktree.sh list                              slots, branches, state, ports, hooks
#   tools/worktree.sh retarget <k> <lane>               switch a clean slot to another lane branch
#   tools/worktree.sh remove <k>                        remove a slot; refused when it is dirty
#                                     or has commits that are on no remote branch and not in main
#   tools/worktree.sh install-hooks [--set-path]        copy tools/hooks/ into the hooks directory
#                                     (main checkout only); --set-path points core.hooksPath at
#                                     <main>/.local/public-hooks first
#
# Ports of slot k start at 25600 + 20k: +0 and +1 official servers (1.21.8, 1.21.4), +2 and +3
# Circuit under client scenarios (1.21.8, 1.21.4), +5 and +6 trace harnesses, +10 to +19 shards.
#
# Environment read by add and env:
#   CIRCUIT_WORK_ROOT          parent of the slots' work directories (default ~/circuit-work)
#   CIRCUIT_XMAKE_GLOBALDIR    the xmake global directory shared by all slots (default
#                              <main>/.xmake-global); install packages there one at a time
#   CIRCUIT_VANILLA_JAR_1_21_8, CIRCUIT_VANILLA_JAR_1_21_4
#                              used when set; otherwise discovered from the main checkout
set -euo pipefail

ENV_FILE=.circuit-worktree.env
MAX_SLOTS=4
PORT_ORIGIN=25600
PORT_SPAN=20
HOOKS=(pre-push reference-transaction)

usage() { sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "worktree.sh: $*" >&2; exit 1; }

script_dir=$(cd "$(dirname "$0")" && pwd)
here=$(git -C "$script_dir" rev-parse --show-toplevel) || die "not inside a git checkout"
main=$(git -C "$here" worktree list --porcelain | sed -n '1s/^worktree //p')
[[ -n $main && -d $main ]] || die "cannot find the main checkout"

slot_path() { printf '%s/%s-wt-%s\n' "$(dirname "$main")" "$(basename "$main")" "$1"; }

need_slot() {
    [[ ${1:-} =~ ^[1-9][0-9]*$ ]] || die "a slot is a number from 1 to $MAX_SLOTS"
    ((10#$1 <= MAX_SLOTS)) || die "there are only $MAX_SLOTS slots; a fifth worktree is refused"
}

# Prints the branch for a lane argument: lane/<lane>, or the argument itself when it has a slash.
branch_of() {
    local lane=${1:-}
    [[ $lane =~ ^[a-z0-9][a-z0-9._/-]*$ && $lane != */ && $lane != *..* ]] || die "bad lane name: '$lane'"
    if [[ $lane == */* ]]; then printf '%s\n' "$lane"; else printf 'lane/%s\n' "$lane"; fi
}

is_worktree() {
    git -C "$main" worktree list --porcelain | grep -Fxq "worktree $1"
}

linked_count() {
    local total
    total=$(git -C "$main" worktree list --porcelain | grep -c '^worktree ')
    echo $((total - 1))
}

need_existing_slot() {
    need_slot "${1:-}"
    local path
    path=$(slot_path "$1")
    is_worktree "$path" || die "slot $1 does not exist ($path is not a worktree of $main)"
}

# Clean means no change and no untracked file; the slot's own settings file does not count.
is_clean() { [[ -z $(git -C "$1" status --porcelain -- . ":(exclude)$ENV_FILE") ]]; }

# find_jar <variable> <directory of the calibration server next to the main checkout>
find_jar() {
    local value=${!1:-} sibling
    sibling=$(dirname "$main")/circuit-clienttest/$2/server.jar
    if [[ -n $value && -r $value ]]; then
        realpath "$value"
    elif [[ -r $sibling ]]; then
        realpath "$sibling"
    else
        echo
    fi
}

# env_text <slot> <path>: the exports of a slot.
env_text() {
    local slot=$1 path=$2 branch lane work jar8 jar4
    branch=$(git -C "$path" symbolic-ref --quiet --short HEAD 2>/dev/null || echo detached)
    lane=${branch#lane/}
    work=${CIRCUIT_WORK_ROOT:-$HOME/circuit-work}/wt-$slot
    jar8=$(find_jar CIRCUIT_VANILLA_JAR_1_21_8 vanilla-chests)
    jar4=$(find_jar CIRCUIT_VANILLA_JAR_1_21_4 vanilla-suite-1.21.4)
    echo "# Written by tools/worktree.sh for slot $slot; git-ignored. Use: source ./$ENV_FILE"
    echo "# The governing documents stay in the main checkout: read them under \$CIRCUIT_MAIN."
    printf 'export CIRCUIT_MAIN=%q\n' "$main"
    printf 'export CIRCUIT_SLOT=%q\n' "$slot"
    printf 'export CIRCUIT_LANE=%q\n' "$lane"
    printf 'export CIRCUIT_PORT_BASE=%q\n' $((PORT_ORIGIN + PORT_SPAN * slot))
    printf 'export CIRCUIT_WORK=%q\n' "$work"
    printf 'export XMAKE_GLOBALDIR=%q\n' "${CIRCUIT_XMAKE_GLOBALDIR:-$main/.xmake-global}"
    printf 'export CIRCUIT_VANILLA_JAR_1_21_8=%q\n' "$jar8"
    printf 'export CIRCUIT_VANILLA_JAR_1_21_4=%q\n' "$jar4"
    printf 'export CIRCUIT_BUILD_JOBS=%q\n' "${CIRCUIT_BUILD_JOBS:-6}"
    printf 'export CIRCUIT_TEST_JOBS=%q\n' "${CIRCUIT_TEST_JOBS:-6}"
}

write_env() {
    local slot=$1 path=$2 temporary
    git -C "$path" check-ignore -q "$ENV_FILE" || die "$ENV_FILE is not git-ignored in $path; nothing written"
    temporary=$(mktemp "$path/$ENV_FILE.XXXXXX")
    env_text "$slot" "$path" >"$temporary"
    mv "$temporary" "$path/$ENV_FILE"
    (
        # shellcheck disable=SC1090
        source "$path/$ENV_FILE"
        mkdir -p "$CIRCUIT_WORK"
        [[ -n $CIRCUIT_VANILLA_JAR_1_21_8 && -n $CIRCUIT_VANILLA_JAR_1_21_4 ]] ||
            echo "worktree.sh: an official jar was not found; gates that require the jars will fail" >&2
    )
}

configure() {
    local path=$1
    (
        # shellcheck disable=SC1090
        source "$path/$ENV_FILE"
        cd "$path"
        local arguments=(-m debug -y)
        timeout 600 xmake f "${arguments[@]}"
    ) || die "xmake configuration failed in $path"
}

# branch_source <branch>: prints how the branch is obtained (local, remote or new).
branch_source() {
    local branch=$1
    if git -C "$main" show-ref --verify --quiet "refs/heads/$branch"; then
        echo local
    elif git -C "$main" show-ref --verify --quiet "refs/remotes/origin/$branch"; then
        echo remote
    elif [[ $branch == lane/* ]]; then
        echo new
    else
        die "branch $branch does not exist; only lane/<lane> branches are created"
    fi
}

cmd_add() {
    local slot=${1:-} lane=${2:-} configure=1 path branch origin
    [[ ${3:-} == --no-configure ]] && configure=0
    need_slot "$slot"
    branch=$(branch_of "$lane")
    path=$(slot_path "$slot")
    [[ ! -e $path ]] || die "slot $slot is taken: $path exists"
    (($(linked_count) < MAX_SLOTS)) || die "$(linked_count) linked worktrees exist; a fifth is refused"
    origin=$(branch_source "$branch")
    case $origin in
        local) git -C "$main" worktree add "$path" "$branch" ;;
        remote) git -C "$main" worktree add --track -b "$branch" "$path" "origin/$branch" ;;
        new) git -C "$main" worktree add -b "$branch" "$path" main ;;
    esac
    write_env "$slot" "$path"
    if ((configure)); then configure "$path"; fi
    echo "slot $slot: $path on $branch; settings in $path/$ENV_FILE"
}

cmd_env() {
    local slot=${1:-} path
    need_existing_slot "$slot"
    path=$(slot_path "$slot")
    if [[ ${2:-} == --write ]]; then
        write_env "$slot" "$path"
    fi
    if [[ -f $path/$ENV_FILE ]]; then
        cat "$path/$ENV_FILE"
    else
        env_text "$slot" "$path"
    fi
}

cmd_list() {
    local slot path branch head state first last hooks hook status
    printf '%-5s %-44s %-22s %-8s %-6s %s\n' slot path branch head state ports
    printf '%-5s %-44s %-22s %-8s %-6s %s\n' main "$main" \
        "$(git -C "$main" symbolic-ref --quiet --short HEAD 2>/dev/null || echo detached)" \
        "$(git -C "$main" rev-parse --short HEAD)" "$(is_clean "$main" && echo clean || echo dirty)" -
    for ((slot = 1; slot <= MAX_SLOTS; slot++)); do
        path=$(slot_path "$slot")
        first=$((PORT_ORIGIN + PORT_SPAN * slot))
        last=$((first + PORT_SPAN - 1))
        if is_worktree "$path"; then
            branch=$(git -C "$path" symbolic-ref --quiet --short HEAD 2>/dev/null || echo detached)
            head=$(git -C "$path" rev-parse --short HEAD)
            state=$(is_clean "$path" && echo clean || echo dirty)
            printf '%-5s %-44s %-22s %-8s %-6s %s\n' "$slot" "$path" "$branch" "$head" "$state" "$first-$last"
        else
            printf '%-5s %-44s %-22s %-8s %-6s %s\n' "$slot" "$path" "(free)" - - "$first-$last"
        fi
    done
    git -C "$main" worktree list --porcelain | sed -n 's/^worktree //p' | while read -r path; do
        [[ $path == "$main" || $path == "$(dirname "$main")/$(basename "$main")"-wt-[1-$MAX_SLOTS] ]] && continue
        printf '%-5s %s\n' other "$path"
    done
    hooks=$(git -C "$main" config --get core.hooksPath || true)
    if [[ $hooks != /* ]]; then
        echo "hooks: core.hooksPath is '${hooks:-unset}', not an absolute path: linked worktrees run no guard"
        return
    fi
    for hook in "${HOOKS[@]}"; do
        if [[ ! -x $hooks/$hook ]]; then
            status="missing"
        elif [[ ! -f $main/tools/hooks/$hook ]]; then
            status="present; the main checkout has no tools/hooks/$hook to compare it with"
        elif cmp -s "$hooks/$hook" "$main/tools/hooks/$hook"; then
            status="installed"
        else
            status="differs from $main/tools/hooks/$hook"
        fi
        echo "hooks: $hooks/$hook $status"
    done
}

cmd_retarget() {
    local slot=${1:-} lane=${2:-} path branch origin
    need_existing_slot "$slot"
    branch=$(branch_of "$lane")
    path=$(slot_path "$slot")
    is_clean "$path" || die "slot $slot is dirty; commit or remove the changes first"
    origin=$(branch_source "$branch")
    case $origin in
        local) git -C "$path" switch "$branch" ;;
        remote) git -C "$path" switch --track -c "$branch" "origin/$branch" ;;
        new) git -C "$path" switch -c "$branch" main ;;
    esac
    write_env "$slot" "$path"
    echo "slot $slot: $path on $branch"
}

cmd_remove() {
    local slot=${1:-} path unpushed
    need_existing_slot "$slot"
    path=$(slot_path "$slot")
    is_clean "$path" || die "slot $slot is dirty; nothing removed"
    unpushed=$(git -C "$path" rev-list --count HEAD --not --remotes refs/heads/main)
    ((unpushed == 0)) || die "slot $slot has $unpushed commits that are on no remote branch and not in main; push first"
    rm -f "$path/$ENV_FILE"
    git -C "$main" worktree remove "$path"
    echo "slot $slot removed; its branch is kept"
}

cmd_install_hooks() {
    local hooks hook temporary wanted=$main/.local/public-hooks
    [[ $here == "$main" ]] || die "install hooks from the main checkout ($main), whose tools/hooks/ is the reviewed copy"
    if [[ ${1:-} == --set-path ]]; then
        git -C "$main" config core.hooksPath "$wanted"
    fi
    hooks=$(git -C "$main" config --get core.hooksPath || true)
    [[ $hooks == /* ]] || die "core.hooksPath is '${hooks:-unset}'; it must be absolute so that linked worktrees use it (--set-path sets $wanted)"
    mkdir -p "$hooks"
    for hook in "${HOOKS[@]}"; do
        temporary=$(mktemp "$hooks/$hook.XXXXXX")
        cp "$main/tools/hooks/$hook" "$temporary"
        chmod 755 "$temporary"
        mv "$temporary" "$hooks/$hook"
        echo "installed $hooks/$hook"
    done
}

command=${1:-}
shift || true
case $command in
    add) cmd_add "$@" ;;
    env) cmd_env "$@" ;;
    list) cmd_list ;;
    retarget) cmd_retarget "$@" ;;
    remove) cmd_remove "$@" ;;
    install-hooks) cmd_install_hooks "$@" ;;
    -h | --help | help) usage ;;
    *) usage >&2; exit 2 ;;
esac
