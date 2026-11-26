// SPDX-License-Identifier: AGPL-3.0-only
// The network layer's test program. Its first cases check the PACE package
// itself as Circuit consumes it (an exact release, TLS on, the system
// OpenSSL); the net lane adds its own cases with CIRCUIT_TEST in further files.
#include "../support/check.hpp"
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <pace/pace.hpp>
#include <pace/tls.hpp>
#include <array>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

using circuit::test::require;
using circuit::test::Suite;

namespace {
pace::task<void> echo(pace::tcp::stream socket) {
    try {
        std::array<char, 1024> buffer{};
        while (const auto count = co_await socket.read_some(buffer)) co_await socket.write_all(std::string_view(buffer.data(), count));
    } catch (const std::system_error&) {
        // The client closing ends the connection.
    }
}

pace::task<void> accept_one(pace::tcp::listener& listener) {
    try {
        listener.context().spawn(echo(co_await listener.accept()));
    } catch (const std::system_error&) {
    }
}

// An exception must not leave a root task (it would stop the reactor): it is
// kept and checked after run().
pace::task<void> client(pace::io_context& io, pace::tcp::endpoint endpoint, std::string message, std::string& reply,
                        std::exception_ptr& failure) {
    try {
        auto socket = co_await pace::tcp::connect(io, std::move(endpoint));
        co_await pace::write(socket, message);
        reply = co_await pace::read_exact(socket, message.size());
        co_await socket.close_gracefully();
    } catch (...) {
        failure = std::current_exception();
    }
    io.stop();
}
} // namespace

int main() {
    Suite suite("net_tests");

    suite.run("a TCP echo round trip on the loopback interface", [] {
        pace::io_context io;
        auto listener = pace::tcp::listen(io, {"127.0.0.1", std::uint16_t{0}});
        const std::string message = "circuit net smoke";
        std::string reply;
        std::exception_ptr failure;
        io.spawn(accept_one(listener));
        io.spawn(client(io, listener.local_endpoint(), message, reply, failure));
        io.run();
        if (failure) std::rethrow_exception(failure);
        require(reply == message, std::format("echoed `{}`", reply));
    });

    suite.run("a TLS client context loads the system trust store", [] {
        // Throws when the trust paths cannot be loaded; with the system
        // OpenSSL they are the distribution's.
        pace::tls::context context(pace::tls::role::client, pace::tls::trust_source::system);
    });

    suite.run("PACE and Circuit run on the OpenSSL whose headers they were compiled with", [] {
        // One OpenSSL in the process: the system library. If the package's
        // own OpenSSL were linked, Circuit's -lssl -lcrypto would resolve to
        // it and the running version would differ from the headers'.
        require(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER,
                std::format("compiled against {:#x}, running {:#x} ({})", static_cast<unsigned long>(OPENSSL_VERSION_NUMBER),
                            OpenSSL_version_num(), OpenSSL_version(OPENSSL_VERSION)));
    });

    return suite.finish();
}
