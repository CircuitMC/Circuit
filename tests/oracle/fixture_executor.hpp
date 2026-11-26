// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// circuit-trace fixtures run on the oracle, through the engine facade.
#include "oracle_engine.hpp"
#include <circuit/evidence/fixture_runner.hpp>

namespace circuit::oracle {

// `data`: the data packs' data the rules run with; none by default.
inline evidence::ExecutorFactory oracle_executor(evidence::DataSource data = {}) {
    return evidence::engine_executor(oracle_engine_factory(), std::move(data));
}

} // namespace circuit::oracle
