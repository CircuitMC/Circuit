// SPDX-License-Identifier: AGPL-3.0-only
#include "serve.hpp"
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <csignal>
#include <iostream>

namespace circuit::app {

int serve(server::ServerConfig config) {
    server::Server server(std::move(config));
    server.start();
    asio::io_context signals_io;
    asio::signal_set signals(signals_io, SIGINT, SIGTERM);
    signals.async_wait([](const asio::error_code&, int) {});
    signals_io.run(); // returns after the first signal
    server.stop();
    return 0;
}

} // namespace circuit::app
