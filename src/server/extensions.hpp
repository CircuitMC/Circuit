// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/evidence/content.hpp>
#include <circuit/plugin/api.hpp>
#include <circuit/runtime/engine.hpp>
#include <circuit/server/server.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>

// Native modules (mods and plugins) and the Java bridge, hosted on the game
// thread. Modules see the world through the published engine view and change
// it only through admitted commands, like players.
namespace circuit::server {

// What the game provides to modules. Called on the game thread only.
class GameServices {
public:
    virtual ~GameServices() = default;
    [[nodiscard]] virtual const runtime::Engine& simulation() const = 0;
    [[nodiscard]] virtual const evidence::ContentMapping& content_mapping() const = 0;
    // Admits a command for the next tick; nullopt when the queue is full.
    virtual std::optional<std::uint64_t> submit(contracts::CommandBody body) = 0;
    // Withdraws an admitted command that has not been applied.
    virtual bool withdraw(std::uint64_t admission) = 0;
    // Sends a system message to an online player; false when there is none.
    virtual bool tell(std::string_view player, std::string_view text) = 0;
    virtual void freeze(bool frozen) = 0;
    virtual void step(std::uint32_t ticks) = 0;
    [[nodiscard]] virtual plugin::TickStatus tick_status() const = 0;
    [[nodiscard]] virtual plugin::TickMetrics tick_metrics() const = 0;
};

// Parses "world.read,commands" into permission bits; nullopt for an unknown name.
std::optional<plugin::Permissions> parse_permissions(std::string_view list);

class Extensions {
public:
    Extensions(GameServices& game, const ServerConfig& config, const contracts::VersionBundle& bundle);
    ~Extensions();
    Extensions(const Extensions&) = delete;
    Extensions& operator=(const Extensions&) = delete;

    // Loads the configured mods (registration phase), closes registration, then
    // loads the plugins and starts the Java bridge. Throws std::runtime_error.
    void load();
    // At the start of a tick that runs: requests from the Java bridge.
    void before_tick();
    // After a committed tick: tick reports, on_tick, the Java bridge.
    void after_tick();
    // Runs a module command for a player. nullopt when no module registered
    // `name`; otherwise the reply (empty when it is delivered later).
    std::optional<std::string> command(std::string_view player, std::string_view name, std::span<const std::string> arguments);
    // Registered command names with their usage, sorted.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> commands() const;
    // Stops the bridge and unloads modules in reverse load order.
    void unload();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace circuit::server
