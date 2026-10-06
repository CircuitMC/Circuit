// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

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
    std::int32_t floor_y{-61}; // flat world: one stone layer at this height
};

// Parses a `key=value` file (blank lines and `#` comments ignored). Unknown
// keys and invalid values are errors.
std::expected<ServerConfig, std::string> load_config(const std::filesystem::path& path);
std::expected<ServerConfig, std::string> parse_config(std::string_view text);

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts the network threads and the tick loop.
    void start();
    // Disconnects every player, drains their sessions and stops the threads.
    void stop();
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::size_t players() const;
    [[nodiscard]] std::uint64_t ticks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace circuit::server
