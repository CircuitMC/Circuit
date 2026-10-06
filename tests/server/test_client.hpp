// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A minimal blocking protocol client for server tests, built on Circuit's own
// codec. It is not an independent implementation of the protocol: external
// clients (a protocol library and the real game client) cover that.
#include <asio.hpp>
#include <circuit/auth/crypto.hpp>
#include <chrono>
#include <circuit/protocol/framing.hpp>
#include <circuit/protocol/nbt.hpp>
#include <circuit/protocol/packets.hpp>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>

namespace circuit::test {
using protocol::Bound;
using protocol::Bytes;
using protocol::State;
using protocol::Writer;

class TestClient {
public:
    explicit TestClient(std::uint16_t port) : socket_(io_) {
        socket_.connect({asio::ip::make_address("127.0.0.1"), port});
        ids_ = protocol::PacketIds::for_protocol(772);
    }

    std::int32_t id(State s, Bound b, std::string_view name) const { return ids_->id(s, b, name); }

    void send_raw(Bytes bytes) {
        if (out_cipher_) out_cipher_->apply(bytes);
        asio::write(socket_, asio::buffer(bytes));
    }
    // Starts AES/CFB8 in both directions with `secret`.
    void enable_encryption(std::span<const std::byte> secret) {
        in_cipher_ = std::make_unique<auth::Cfb8>(secret, auth::Cfb8::Direction::decrypt);
        out_cipher_ = std::make_unique<auth::Cfb8>(secret, auth::Cfb8::Direction::encrypt);
        decoder_.transform_unread([this](std::span<std::byte> unread) { in_cipher_->apply(unread); });
    }
    void send(std::int32_t packet_id, const Writer& fields) {
        Writer w;
        w.varint(packet_id).raw(fields.bytes());
        send_raw(protocol::encode_frame(w.bytes(), threshold_));
    }
    void send(State s, std::string_view name, const Writer& fields = {}) { send(id(s, Bound::serverbound, name), fields); }

    struct Packet {
        std::int32_t id;
        Bytes fields;
        [[nodiscard]] protocol::Reader reader() const { return protocol::Reader(fields); }
    };

    // Next packet, or nullopt when the connection closed or the timeout passed.
    std::optional<Packet> next(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            auto ready = decoder_.next();
            if (!ready) throw std::runtime_error("test client: malformed frame from the server");
            if (*ready) {
                auto view = protocol::open_packet(**ready);
                if (!view) throw std::runtime_error("test client: malformed packet");
                const auto rest = view->fields.rest();
                return Packet{view->id, Bytes(rest.begin(), rest.end())};
            }
            if (closed_ || std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            if (!read_some(deadline)) return std::nullopt;
        }
    }

    // Skips packets until one with `packet_id` arrives.
    std::optional<Packet> until(State s, std::string_view name, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto want = id(s, Bound::clientbound, name);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto p = next(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            if (!p) return std::nullopt;
            if (p->id == want) return p;
            if (on_skip) on_skip(*p);
        }
        return std::nullopt;
    }

    void enable_compression(std::int32_t threshold) {
        threshold_ = threshold;
        decoder_.set_compression(threshold);
    }

    // Handshake with intent 2 (login) and Login Start.
    void start_login(std::string_view name, std::int32_t protocol_version = 772) {
        send(id(State::handshaking, Bound::serverbound, "intention"),
             Writer().varint(protocol_version).string("localhost").u16(25565).varint(2));
        send(State::login, "hello", Writer().string(name, 16).uuid({}));
    }

    // Full login through configuration into play. Returns the play login packet.
    Packet join(std::string_view name) {
        start_login(name);
        auto first = next();
        if (!first) throw std::runtime_error("test client: no login answer");
        if (first->id == id(State::login, Bound::clientbound, "login_compression")) {
            auto r = first->reader();
            enable_compression(r.varint().value());
            first = next();
        }
        if (!first || first->id != id(State::login, Bound::clientbound, "login_finished"))
            throw std::runtime_error("test client: login not accepted");
        send(State::login, "login_acknowledged");
        if (!until(State::configuration, "select_known_packs")) throw std::runtime_error("test client: no known packs");
        send(State::configuration, "select_known_packs", Writer().varint(1).string("minecraft").string("core").string("1.21.8"));
        send(State::configuration, "client_information",
             Writer().string("en_us", 16).i8(4).varint(0).boolean(true).u8(0x7F).varint(1).boolean(false).boolean(true).varint(0));
        if (!until(State::configuration, "finish_configuration")) throw std::runtime_error("test client: no finish");
        send(State::configuration, "finish_configuration");
        auto login = until(State::play, "login");
        if (!login) throw std::runtime_error("test client: no play login");
        return *login;
    }

    bool closed() const noexcept { return closed_; }
    std::function<void(const Packet&)> on_skip;

private:
    bool read_some(std::chrono::steady_clock::time_point deadline) {
        std::array<std::byte, 65536> buf{};
        std::size_t got = 0;
        asio::error_code result = asio::error::would_block;
        socket_.async_read_some(asio::buffer(buf), [&](const asio::error_code& ec, std::size_t n) {
            result = ec;
            got = n;
        });
        io_.restart();
        io_.run_until(deadline);
        if (result == asio::error::would_block) {
            socket_.cancel();
            io_.restart();
            io_.run();
            return false;
        }
        if (result) {
            closed_ = true;
            return false;
        }
        if (in_cipher_) in_cipher_->apply(std::span<std::byte>(buf.data(), got));
        if (!decoder_.feed(std::span<const std::byte>(buf.data(), got)))
            throw std::runtime_error("test client: too much data");
        return true;
    }

    asio::io_context io_;
    asio::ip::tcp::socket socket_;
    protocol::FrameDecoder decoder_{protocol::FrameLimits{.max_frame = 2097151, .max_uncompressed = 8388608, .max_buffered = 64 * 1024 * 1024}};
    std::int32_t threshold_{-1};
    bool closed_{false};
    std::unique_ptr<auth::Cfb8> in_cipher_, out_cipher_;
    const protocol::PacketIds* ids_;
};

} // namespace circuit::test
