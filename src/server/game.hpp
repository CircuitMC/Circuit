// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "registries.hpp"
#include "wire.hpp"
#include "world_codec.hpp"
#include <circuit/runtime/engine.hpp>
#include <circuit/server/server.hpp>
#include <memory>
#include <string>

// The authoritative game: players, their views of the world, and the engine.
// Network threads only enqueue joins, intents and departures; the game thread
// applies them at the start of a tick in arrival order, advances the engine
// once, and publishes the resulting changes. Players never touch engine state
// directly.
namespace circuit::server {

// The network side of one player, as the game sees it. Both calls are
// thread-safe and never block on the network.
class Peer {
public:
    virtual ~Peer() = default;
    [[nodiscard]] virtual std::uint64_t id() const noexcept = 0;
    virtual void send(Bytes packet) = 0;
    virtual void kick(std::string reason) = 0;
};

struct JoinRequest {
    std::shared_ptr<Peer> peer;
    Uuid uuid;
    std::string name;
    std::vector<ProfileProperty> properties;
    std::int32_t view_distance{};
    bool verified{};
};

class Game {
public:
    Game(const ServerConfig& config, const contracts::VersionBundle& bundle, const Wire& wire,
         const protocol::ContentTables& content, const evidence::ContentMapping& mapping,
         const std::vector<RegistryEntries>& registries);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // Thread-safe inputs, applied at the start of the next tick in arrival order.
    void join(JoinRequest request);
    void submit(std::uint64_t peer, Intent intent);
    void leave(std::uint64_t peer);

    // Game thread only.
    void tick();
    void shutdown(std::string_view reason);

    [[nodiscard]] std::size_t players() const noexcept; // thread-safe
    [[nodiscard]] std::uint64_t ticks() const noexcept; // thread-safe
    [[nodiscard]] const runtime::Engine& engine() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace circuit::server
