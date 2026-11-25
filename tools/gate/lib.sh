# Shared functions of tools/check.sh and tools/gate/*.sh. Source this file; do not run it.
#
# A gate is a list of steps. Every step ends as PASS, FAIL or NOT-RUN, and a gate passes only
# when every step it planned is PASS. A step that could not start is NOT-RUN, never a pass.
#
# Environment read here:
#   CIRCUIT_ASIO_INCLUDE          standalone Asio include directory (empty: the xmake package)
#   CIRCUIT_VANILLA_JAR_1_21_8    official server jars for the cases that need them
#   CIRCUIT_VANILLA_JAR_1_21_4
#   CIRCUIT_GATE_BUILD_TIMEOUT    seconds for configure plus build (default 3600)
#   CIRCUIT_GATE_TEST_TIMEOUT     seconds per test program (default 7200)
#   CIRCUIT_GATE_SHARDS           "program=n ..." runs a program as n shards (CIRCUIT_TEST_SHARD=i/n)
#                                 when the program supports sharding; otherwise it runs whole

# Longest first, so the side-by-side run is bounded by the longest program.
GATE_PROGRAMS=(evidence_tests storage_tests simulation_tests server_tests rules_tests anvil_tests
               protocol_tests data_tests extension_tests contract_tests control_tests runtime_tests
               interface_tests validation_tests plugin_tests parallel_models)

GATE_JAR_VARIABLES=(CIRCUIT_VANILLA_JAR_1_21_8 CIRCUIT_VANILLA_JAR_1_21_4)

# What a sanitizer prints when it has something to report.
GATE_SANITIZER_PATTERN='ERROR: AddressSanitizer|ERROR: LeakSanitizer|LeakSanitizer: detected|WARNING: ThreadSanitizer|ThreadSanitizer: |ERROR: UndefinedBehaviorSanitizer|runtime error: |DEADLYSIGNAL'

gate_steps=()

gate_now() { date +%s; }

# gate_record <PASS|FAIL|NOT-RUN> <step> <seconds> <detail>
gate_record() {
    gate_steps+=("$1|$2|$3|$4")
    printf '%-8s %-34s %6s s  %s\n' "$1" "$2" "$3" "$4"
}

# gate_summary <title>: one line per step; returns non-zero unless every step passed.
gate_summary() {
    local line status name seconds detail failed=0 passed=0
    echo
    echo "== $1"
    for line in "${gate_steps[@]}"; do
        IFS='|' read -r status name seconds detail <<<"$line"
        printf '%-8s %-34s %6s s  %s\n' "$status" "$name" "$seconds" "$detail"
        if [[ $status == PASS ]]; then passed=$((passed + 1)); else failed=$((failed + 1)); fi
    done
    if ((${#gate_steps[@]} == 0)); then
        echo "RESULT: FAIL (no step ran)"
        return 1
    fi
    if ((failed)); then
        echo "RESULT: FAIL ($failed of ${#gate_steps[@]} steps did not pass)"
        return 1
    fi
    echo "RESULT: PASS ($passed steps)"
}

# gate_require_jars <prefix>: the jar variables must name readable files.
gate_require_jars() {
    local variable value missing=()
    for variable in "${GATE_JAR_VARIABLES[@]}"; do
        value=${!variable:-}
        if [[ -z $value ]]; then
            missing+=("$variable is not set")
        elif [[ ! -r $value || ! -s $value ]]; then
            missing+=("$variable names no readable file")
        fi
    done
    if ((${#missing[@]})); then
        local joined
        joined=$(printf '%s; ' "${missing[@]}")
        gate_record FAIL "${1}jars" 0 "${joined%; }"
        return 1
    fi
    gate_record PASS "${1}jars" 0 "both official jars are readable"
}

# gate_build <prefix> <tree> <mode> <jobs> <log directory> <clean: 0|1> <zero warnings: 0|1>
# Configures and builds every target. A clean build removes the build directory and turns the
# compiler cache off, so every translation unit is compiled and can warn. An incremental build
# shows only the warnings of what it compiled.
gate_build() {
    local prefix=$1 tree=$2 mode=$3 jobs=$4 logs=$5 clean=$6 zero_warnings=$7
    local log=$logs/build-$mode.log start rc compiled warnings crashes=0 note=
    local configure=(-m "$mode" -y) build=(-y -j "$jobs" --all)
    if [[ -n ${CIRCUIT_ASIO_INCLUDE:-} ]]; then
        configure+=("--asio_include=$CIRCUIT_ASIO_INCLUDE")
    fi
    if ((clean)); then
        configure+=(--policies=build.ccache:n)
        build+=(-r)
    fi
    start=$(gate_now)
    (
        cd "$tree" || exit 1
        if ((clean)); then rm -rf build .xmake; fi
        timeout "${CIRCUIT_GATE_BUILD_TIMEOUT:-3600}" xmake f "${configure[@]}" &&
            timeout "${CIRCUIT_GATE_BUILD_TIMEOUT:-3600}" xmake build "${build[@]}"
    ) >"$log" 2>&1
    rc=$?
    # The compiler itself can crash at random on a loaded host. That says nothing about the
    # tree, so the build continues from where it stopped, at most twice; the log keeps every
    # attempt and the step says how often it happened. Any other failure is final.
    while ((rc != 0 && crashes < 2)) && [[ $(grep -c 'internal compiler error' "$log") -gt $crashes ]]; do
        crashes=$((crashes + 1))
        echo "== attempt $((crashes + 1)) after an internal compiler error" >>"$log"
        (cd "$tree" && timeout "${CIRCUIT_GATE_BUILD_TIMEOUT:-3600}" xmake build -y -j "$jobs" --all) >>"$log" 2>&1
        rc=$?
    done
    compiled=$(grep -c "compiling\.$mode" "$log")
    warnings=$(grep -c 'warning:' "$log")
    if ((crashes)); then note=", continued after $crashes compiler crashes"; fi
    if ((rc != 0)); then
        gate_record FAIL "${prefix}build" $(($(gate_now) - start)) "exit $rc$note, see $log"
        if ((zero_warnings)); then
            gate_record NOT-RUN "${prefix}warnings" 0 "the build failed"
        fi
        return 1
    fi
    gate_record PASS "${prefix}build" $(($(gate_now) - start)) "$compiled files compiled, $warnings warnings$note, $log"
    if ((zero_warnings)); then
        if ((warnings != 0)); then
            gate_record FAIL "${prefix}warnings" 0 "$warnings warnings in $log"
            return 1
        fi
        if ((clean && compiled == 0)); then
            gate_record FAIL "${prefix}warnings" 0 "a clean build compiled nothing, see $log"
            return 1
        fi
        gate_record PASS "${prefix}warnings" 0 "0 warnings in $compiled compiled files"
    fi
}

# gate_binary_directory <tree> <mode>: the directory of the built programs.
gate_binary_directory() {
    local candidate found=()
    for candidate in "$1"/build/*/*/"$2"; do
        [[ -d $candidate ]] && found+=("$candidate")
    done
    ((${#found[@]} == 1)) || return 1
    printf '%s\n' "${found[0]}"
}

# gate_shards_of <program>: the shard count asked for in CIRCUIT_GATE_SHARDS (default 1).
gate_shards_of() {
    local entry
    for entry in ${CIRCUIT_GATE_SHARDS:-}; do
        if [[ ${entry%%=*} == "$1" && ${entry#*=} =~ ^[1-9][0-9]*$ ]]; then
            printf '%s\n' "${entry#*=}"
            return
        fi
    done
    echo 1
}

# Runs one program with its log and writes "<exit status> <seconds>" next to the log.
# gate_launch <tree> <mode> <binary directory> <program> <log> <require jars: 0|1> <shard or empty>
gate_launch() {
    local tree=$1 mode=$2 bin=$3 program=$4 log=$5 jars=$6 shard=$7
    local command=() environment=() arguments=() start rc child
    case $program in
        plugin_tests) arguments=("$bin/libtechnical_tools.so" "$bin/libplugin_fixture.so") ;;
        evidence_tests) arguments=("$tree/tests/fixtures") ;;
    esac
    case $mode in
        tsan)
            # ThreadSanitizer cannot map its shadow with high mmap randomization.
            environment+=("TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1}")
            command=(setarch "$(uname -m)" -R)
            ;;
        asan) environment+=("ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1}") ;;
        ubsan) environment+=("UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}") ;;
    esac
    if ((jars)); then environment+=(CIRCUIT_REQUIRE_JARS=1); fi
    if [[ -n $shard ]]; then environment+=("CIRCUIT_TEST_SHARD=$shard"); fi
    rm -f "$log.rc"
    start=$(gate_now)
    cd "$tree" || return 1
    env "${environment[@]}" timeout "${CIRCUIT_GATE_TEST_TIMEOUT:-7200}" \
        "${command[@]}" "$bin/$program" "${arguments[@]}" >"$log" 2>&1 &
    child=$!
    trap 'kill "$child" 2>/dev/null' TERM INT
    wait "$child"
    rc=$?
    echo "$rc $(($(gate_now) - start))" >"$log.rc"
}

# Decides one finished program from its exit status and its log, and leaves the number of
# cases it counted as passed in gate_judged_cases.
# gate_judge <step> <log> <allow no cases: 0|1>
gate_judge() {
    local step=$1 log=$2 allow_empty=$3 rc seconds totals passed failed line
    gate_judged_cases=0
    if [[ ! -s $log.rc ]]; then
        gate_record NOT-RUN "$step" 0 "did not finish, see $log"
        return 1
    fi
    read -r rc seconds <"$log.rc"
    if ((rc == 124)); then
        gate_record FAIL "$step" "$seconds" "timed out, see $log"
        return 1
    fi
    if ((rc != 0)); then
        if grep -qE 'SUMMARY: [A-Za-z]+Sanitizer: SEGV [^ ]*libsanitizer/' "$log"; then
            # The fault is inside the sanitizer's runtime, not in the program. Still a failure:
            # the program did not finish its tests.
            gate_record FAIL "$step" "$seconds" "exit $rc: the sanitizer runtime crashed in its own code, see $log"
        elif grep -qE "$GATE_SANITIZER_PATTERN" "$log"; then
            gate_record FAIL "$step" "$seconds" "exit $rc with a sanitizer report, see $log"
        else
            gate_record FAIL "$step" "$seconds" "exit $rc, see $log"
        fi
        return 1
    fi
    if grep -qE "$GATE_SANITIZER_PATTERN" "$log"; then
        gate_record FAIL "$step" "$seconds" "exit 0 with a sanitizer report, see $log"
        return 1
    fi
    if grep -q 'SKIPPED' "$log"; then
        gate_record FAIL "$step" "$seconds" "a case was skipped: $(grep -m1 'SKIPPED' "$log" | cut -c1-60)"
        return 1
    fi
    if grep -qE '[1-9][0-9]* skipped for a missing jar' "$log"; then
        gate_record FAIL "$step" "$seconds" "cases left out the checks that need an official jar, see $log"
        return 1
    fi
    # A program that ran its cases says so: "<suite>: N passed, M failed" for the suites with
    # cases, a line with "passed" for the others. Exit 0 without it means no test ran.
    totals=$(grep -E '[0-9]+ passed, [0-9]+ failed' "$log")
    if [[ -n $totals ]]; then
        passed=0
        failed=0
        while read -r line; do
            [[ $line =~ ([0-9]+)\ passed,\ ([0-9]+)\ failed ]] || continue
            passed=$((passed + BASH_REMATCH[1]))
            failed=$((failed + BASH_REMATCH[2]))
        done <<<"$totals"
        if ((failed != 0)); then
            gate_record FAIL "$step" "$seconds" "$failed cases failed, see $log"
            return 1
        fi
        if ((passed == 0 && !allow_empty)); then
            gate_record FAIL "$step" "$seconds" "no case ran, see $log"
            return 1
        fi
        gate_judged_cases=$passed
        gate_record PASS "$step" "$seconds" "$passed passed, 0 failed"
        return 0
    fi
    line=$(grep -E 'passed' "$log" | tail -1)
    if [[ -z $line ]]; then
        gate_record FAIL "$step" "$seconds" "exit 0 without a completion line, see $log"
        return 1
    fi
    gate_record PASS "$step" "$seconds" "${line:0:70}"
}

# gate_run_programs <prefix> <tree> <mode> <test jobs> <log directory> <require jars: 0|1> [program...]
# Runs the programs side by side, at most <test jobs> at a time, and records one step each.
gate_run_programs() {
    local prefix=$1 tree=$2 mode=$3 jobs=$4 logs=$5 jars=$6
    shift 6
    local programs=("$@") program bin shards shard index running=0 start wall sum=0 failed=0
    local queue=() item step log rc seconds sharded=
    local -A shard_cases=()
    ((${#programs[@]})) || programs=("${GATE_PROGRAMS[@]}")
    if ! bin=$(gate_binary_directory "$tree" "$mode"); then
        for program in "${programs[@]}"; do
            gate_record NOT-RUN "${prefix}$program" 0 "no single build directory for mode $mode in $tree"
        done
        return 1
    fi
    for program in "${programs[@]}"; do
        if [[ ! -x $bin/$program ]]; then
            queue+=("$program||missing")
            continue
        fi
        shards=$(gate_shards_of "$program")
        if ((shards > 1)) && grep -q CIRCUIT_TEST_SHARD "$bin/$program"; then
            # Shard indices start at 0: CIRCUIT_TEST_SHARD=i/n with 0 <= i < n.
            for ((index = 0; index < shards; index++)); do
                queue+=("$program|$index/$shards|")
            done
        else
            queue+=("$program||")
        fi
    done
    start=$(gate_now)
    trap 'kill $(jobs -p) 2>/dev/null' TERM INT
    for item in "${queue[@]}"; do
        IFS='|' read -r program shard rc <<<"$item"
        [[ $rc == missing ]] && continue
        while ((running >= jobs)); do
            wait -n
            running=$((running - 1))
        done
        log=$logs/$mode-$program${shard:+-shard-${shard%%/*}}.log
        (gate_launch "$tree" "$mode" "$bin" "$program" "$log" "$jars" "$shard") &
        running=$((running + 1))
    done
    wait
    trap - TERM INT
    wall=$(($(gate_now) - start))
    for item in "${queue[@]}"; do
        IFS='|' read -r program shard rc <<<"$item"
        step=${prefix}$program${shard:+[$shard]}
        if [[ $rc == missing ]]; then
            gate_record NOT-RUN "$step" 0 "$bin/$program was not built"
            failed=1
            continue
        fi
        log=$logs/$mode-$program${shard:+-shard-${shard%%/*}}.log
        gate_judge "$step" "$log" "$([[ -n $shard ]] && echo 1 || echo 0)" || failed=1
        if [[ -n $shard ]]; then
            shard_cases[$program]=$((${shard_cases[$program]:-0} + gate_judged_cases))
        fi
        if [[ -s $log.rc ]]; then
            read -r rc seconds <"$log.rc"
            sum=$((sum + seconds))
        fi
    done
    # One shard may be empty, but a program whose shards ran no case at all did not run.
    for sharded in "${!shard_cases[@]}"; do
        if ((shard_cases[$sharded] == 0)); then
            gate_record FAIL "${prefix}$sharded" 0 "no case ran in any shard"
            failed=1
        fi
    done
    echo "${prefix}programs: $wall s side by side with $jobs at a time; $sum s one after another"
    return $failed
}

# gate_command <step> <log> <command...>: one step from a command's exit status.
gate_command() { gate_command_in . "$@"; }

# gate_command_in <directory> <step> <log> <command...>: the same, run in a directory.
gate_command_in() {
    local directory=$1 step=$2 log=$3 start rc
    shift 3
    start=$(gate_now)
    (cd "$directory" && "$@") >"$log" 2>&1
    rc=$?
    if ((rc != 0)); then
        gate_record FAIL "$step" $(($(gate_now) - start)) "exit $rc, see $log"
        return 1
    fi
    gate_record PASS "$step" $(($(gate_now) - start)) "$(tail -1 "$log" | cut -c1-70)"
}

# gate_tree_checks <prefix> <tree> <log directory>: the checks of a tree besides its programs:
# the compatibility ledger, the ledger tool's tests and the Java SPI compilation.
gate_tree_checks() {
    local prefix=$1 tree=$2 logs=$3 sources=()
    gate_command_in "$tree" "${prefix}ledger" "$logs/ledger.log" python3 tools/compat_ledger.py check
    gate_command_in "$tree" "${prefix}ledger-tools" "$logs/ledger-tools.log" python3 tests/tools/compat_ledger_tests.py
    if ! command -v javac >/dev/null; then
        gate_record NOT-RUN "${prefix}java-spi" 0 "javac 17+ is not available"
        return 1
    fi
    mapfile -t sources < <(cd "$tree" && find modules/java-bridge -name '*.java' | sort)
    if ((${#sources[@]} == 0)); then
        gate_record NOT-RUN "${prefix}java-spi" 0 "no Java source under modules/java-bridge"
        return 1
    fi
    mkdir -p "$tree/build/java-spi"
    gate_command_in "$tree" "${prefix}java-spi" "$logs/java-spi.log" javac --release 17 -d build/java-spi "${sources[@]}"
}
