// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "game.hpp"
#include <asio.hpp>
#include <atomic>
#include <circuit/auth.hpp>
#include <circuit/auth/crypto.hpp>
#include <circuit/auth/session.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/protocol/output_queue.hpp>
#include <mutex>
#include <unordered_map>

namespace circuit::server {

class Session;

// State shared by all connections of one server.
struct Shared {
    const ServerConfig& config;
    const Wire& wire;
    const std::vector<RegistryEntries>& registries;
    const std::vector<NumberedTags>& tags;
    const std::vector<RegistryData>& registries_with_data;
    std::string_view game_version; // e.g. "1.21.8"
    Game& game;
    auth::authenticator& offline;
    // Online mode: the server key and the session verifier; verifications run
    // on `auth_pool`, away from the network threads.
    const auth::RsaKeyPair* keys{};
    const auth::SessionVerifier* verifier{};
    asio::thread_pool* auth_pool{};
    std::atomic<std::uint64_t> next_id{1};
    std::atomic<bool> stopping{false};
    std::mutex sessions_mutex;
    std::unordered_map<std::uint64_t, std::weak_ptr<Session>> sessions;
};

// One client connection. Its protocol state machine runs on a strand; the
// game talks to it only through the thread-safe Peer interface.
class Session final : public Peer, public std::enable_shared_from_this<Session> {
public:
    Session(asio::ip::tcp::socket socket, Shared& shared);
    ~Session() override;

    void start();
    [[nodiscard]] std::uint64_t id() const noexcept override { return id_; }
    void send(Bytes packet) override;
    void kick(std::string reason) override;
    // Closes the connection without a reason (thread-safe).
    void close();

private:
    enum class Phase : std::uint8_t {
        handshake, status, login, login_encrypting, login_verifying, login_acknowledging, configuration, play, closed
    };

    void read();
    void received(std::size_t n);
    bool handle(const Bytes& packet);
    bool on_handshake(std::int32_t id, protocol::Reader& r);
    bool on_status(std::int32_t id, protocol::Reader& r);
    bool on_login(std::int32_t id, protocol::Reader& r);
    bool on_configuration(std::int32_t id, protocol::Reader& r);
    bool on_play(std::int32_t id, protocol::Reader& r);
    void queue(Bytes packet);
    void finish_login();
    void verified(std::expected<auth::VerifiedProfile, auth::SessionError> result);
    void write();
    void fail(std::string reason); // kick on the strand
    void close_after_writes();
    void terminate();
    void schedule_timer();
    void timer_fired();

    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    asio::steady_timer timer_;
    Shared& shared_;
    std::uint64_t id_;
    std::string remote_;
    Phase phase_{Phase::handshake};
    protocol::FrameDecoder decoder_;
    std::array<std::byte, 16384> buffer_{};
    protocol::OutputQueue out_;
    bool closing_{}; // the last frames are written, then the connection closes
    std::chrono::steady_clock::time_point closing_since_;
    std::int32_t out_threshold_{-1};
    std::int32_t client_protocol_{};
    std::string name_;
    Uuid uuid_;
    std::vector<ProfileProperty> properties_;
    auth::ByteVector verify_token_;
    std::unique_ptr<auth::Cfb8> in_cipher_, out_cipher_;
    bool verified_{};
    std::int32_t view_distance_{8};
    bool joined_{};
    std::chrono::steady_clock::time_point started_;
    std::optional<std::int64_t> keep_alive_pending_;
    std::chrono::steady_clock::time_point keep_alive_sent_;
    std::chrono::steady_clock::time_point rate_window_;
    std::uint32_t rate_count_{};
};

} // namespace circuit::server
