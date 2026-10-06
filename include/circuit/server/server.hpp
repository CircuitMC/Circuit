// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Network server: Java Edition connections on top of the headless engine.
namespace circuit::server {

struct ServerConfig {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{25565}; // 0 picks a free port (tests)
    std::string game_version{"java-1.21.8"};
    unsigned network_threads{2};
    unsigned workers{4}; // engine worker threads
    std::int32_t max_players{20};
    std::size_t max_connections{256}; // open sockets, including status pings and logins in progress
    std::int32_t view_distance{6};       // chunks sent around each player
    std::int32_t simulation_distance{4}; // ticking radius of each player's ticket
    std::int32_t compression_threshold{256}; // negative disables compression
    // When true, logins require session verification; without a working
    // verifier they are refused (default deny). When false, identities are
    // explicitly offline and unverified.
    bool online_mode{true};
    std::string motd{"A Circuit server"};
    // Session service for online mode: https, or plain http on loopback (tests).
    std::string session_server{"https://sessionserver.mojang.com"};
    std::chrono::milliseconds auth_timeout{5000};
    std::uint64_t seed{1};
    std::size_t max_queued_bytes{16 * 1024 * 1024}; // per connection, before it is closed as too slow
    std::chrono::seconds login_timeout{30};
    std::chrono::seconds keep_alive_timeout{30};
    std::int32_t floor_y{-61}; // flat world: one stone layer at this height (new worlds)
    // Directory of the saved world: checkpoints and the input log. Empty: the
    // world lives in memory only.
    std::filesystem::path world_directory;
    std::uint32_t autosave_ticks{6000}; // between checkpoints
    // Input log records are synced every this many ticks: a crash loses at
    // most the inputs of the ticks since the last sync. 1 syncs every tick.
    std::uint32_t log_sync_ticks{20};
    // Native modules (shared libraries), loaded at startup: mods during
    // registration, then plugins, each after its dependencies.
    std::vector<std::filesystem::path> plugins;
    // Permissions per module id, e.g. "world.read,world.write,commands".
    // Modules not listed get world.read and world.observe.
    std::map<std::string, std::string> plugin_permissions;
    // Java bridge: the class path of the bridge runtime; empty disables it.
    std::string java_bridge;
    std::string java{"java"};
    std::vector<std::string> java_plugin_classes;
    std::filesystem::path java_plugins; // directory of plugin jars
    std::string java_permissions{"world.read"};
    std::uint32_t java_max_lag_ticks{200}; // unacknowledged ticks before the bridge is stopped
};

// Parses a `key=value` file (blank lines and `#` comments ignored). Unknown
// keys and invalid values are errors.
std::expected<ServerConfig, std::string> load_config(const std::filesystem::path& path);
std::expected<ServerConfig, std::string> parse_config(std::string_view text);

class Server {
public:
    // Binds the port, then opens or creates the world, recovering it after a
    // crash. The world directory stays owned by this server until stop().
    // Throws std::invalid_argument for a bad configuration and
    // std::runtime_error for a port that cannot be bound or a world that
    // cannot be opened, including one another server owns.
    explicit Server(ServerConfig config);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Starts accepting connections, the network threads and the tick loop.
    void start();
    // Saves the world, releases its directory, disconnects every player,
    // drains their sessions and stops the threads.
    void stop();
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::size_t players() const;
    [[nodiscard]] std::uint64_t ticks() const;
    // False once the tick loop stopped by itself after an internal or
    // storage failure; the server then accepts no players and should be
    // stopped.
    [[nodiscard]] bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace circuit::server
