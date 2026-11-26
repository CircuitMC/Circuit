// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The engine the test programs run their worlds on, behind runtime::Engine.
// CIRCUIT_ENGINE names it; the oracle is the default and, until the strict
// runtime exists, the only one. Tests that compare an engine with the oracle
// (simulation_tests) take the engine from here and the oracle directly.
#include "../oracle/oracle_engine.hpp"
#include <circuit/runtime/engine.hpp>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace circuit::test {

inline std::string engine_under_test_name() {
    const char* name = std::getenv("CIRCUIT_ENGINE");
    return name && *name ? name : "oracle";
}

inline runtime::EngineFactory engine_under_test() {
    const auto name = engine_under_test_name();
    if (name == "oracle") return oracle::oracle_engine_factory();
    throw std::invalid_argument("CIRCUIT_ENGINE: unknown engine " + name + " (known: oracle)");
}

// A new world on the engine under test.
inline std::unique_ptr<runtime::Engine> make_engine(const contracts::VersionBundle& bundle, std::uint64_t seed,
                                                    const runtime::EngineOptions& options = {}) {
    return engine_under_test()(bundle, seed, options, nullptr);
}
// The engine under test continuing from a state; std::invalid_argument for a
// state the rules never produce.
inline std::unique_ptr<runtime::Engine> restore_engine(const contracts::VersionBundle& bundle, std::uint64_t seed,
                                                       const gameplay::WorldDump& state, const runtime::EngineOptions& options = {}) {
    return engine_under_test()(bundle, seed, options, &state);
}

} // namespace circuit::test
