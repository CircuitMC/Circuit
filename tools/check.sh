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
if command -v javac >/dev/null; then
    mkdir -p build/java-spi
    mapfile -t sources < <(rg --files modules/java-bridge -g '*.java')
    javac --release 17 -d build/java-spi "${sources[@]}"
else
    echo 'Java SPI compilation skipped: javac 17+ is not available.'
fi
