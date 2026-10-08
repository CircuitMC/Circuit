#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export XMAKE_GLOBALDIR="${XMAKE_GLOBALDIR:-$PWD/.xmake-global}"
args=(-m debug -y)
if [[ -n "${CIRCUIT_ASIO_INCLUDE:-}" ]]; then
    args+=("--asio_include=$CIRCUIT_ASIO_INCLUDE")
fi
xmake f "${args[@]}"
xmake build -y --all
xmake run runtime_tests
xmake run interface_tests
xmake run validation_tests
xmake run plugin_tests
xmake run parallel_models
xmake run contract_tests
xmake run rules_tests
xmake run simulation_tests
xmake run control_tests
xmake run evidence_tests
xmake run protocol_tests
xmake run storage_tests
xmake run anvil_tests
xmake run server_tests
xmake run extension_tests
python3 tools/compat_ledger.py check
python3 tests/tools/compat_ledger_tests.py
if command -v javac >/dev/null; then
    mkdir -p build/java-spi
    mapfile -t sources < <(find modules/java-bridge -name '*.java' | sort)
    javac --release 17 -d build/java-spi "${sources[@]}"
else
    echo 'Java SPI compilation skipped: javac 17+ is not available.'
fi
