// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "extensions.hpp"
#include <functional>
#include <memory>
#include <span>
#include <string>

// The server side of the Java bridge (protocol version 1, described in
// modules/java-bridge/README.md). The JVM runs as a child process and talks
// over a Unix socket in a private directory after presenting a random token.
// A reader thread queues its messages and a writer thread drains a bounded
// outbox; the game thread never waits for the JVM.
namespace circuit::server {

class JavaBridge {
public:
    using CommandRegistrar = std::function<plugin::Result<void>(std::string_view name, std::string_view usage)>;

    JavaBridge(GameServices& game, const ServerConfig& config, const contracts::VersionBundle& bundle,
               plugin::Permissions permissions, CommandRegistrar registrar);
    ~JavaBridge();
    JavaBridge(const JavaBridge&) = delete;
    JavaBridge& operator=(const JavaBridge&) = delete;

    // Starts the JVM and completes the handshake. Throws std::runtime_error.
    void start();
    // Game thread, before a tick runs: answers queued requests and checks
    // the JVM's health. After a failure, failed() is true.
    void serve();
    // Game thread, after a committed tick.
    void tick(const plugin::TickReport& report);
    // Forwards a registered command; the reply is sent to the player later.
    void command(std::string_view player, std::string_view name, std::span<const std::string> arguments);
    [[nodiscard]] bool subscribed() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::string failure() const;
    // Asks the JVM to stop, waits briefly, then kills it.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace circuit::server
