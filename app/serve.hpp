// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/server/server.hpp>

namespace circuit::app {

// Runs the network server until SIGINT or SIGTERM, then stops it gracefully.
// Returns a process exit code.
int serve(server::ServerConfig config);

} // namespace circuit::app
