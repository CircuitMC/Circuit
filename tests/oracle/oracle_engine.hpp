// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The oracle behind the engine facade. It is the specification of strict
// mode: tests run an implementation and this adapter on the same inputs and
// compare traces, dumps and change feeds tick by tick.
#include "oracle.hpp"
#include <circuit/runtime/engine.hpp>
#include <memory>

namespace circuit::oracle {

// Makes oracles behind runtime::Engine. Queries, light and the change feed
// are computed from the dumps of the last two ticks, which the adapter keeps;
// it is meant for tests, not for speed.
runtime::EngineFactory oracle_engine_factory();

} // namespace circuit::oracle
