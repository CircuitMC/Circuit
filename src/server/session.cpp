// SPDX-License-Identifier: AGPL-3.0-only
#include "session.hpp"
#include "log.hpp"
#include <cstdlib>
#include <format>

namespace circuit::server {
using protocol::Bound;
using protocol::CodecError;
using protocol::State;

namespace {
constexpr auto keep_alive_interval = std::chrono::seconds(10);
constexpr std::uint32_t max_packets_per_second = 1000;
constexpr auto close_deadline = std::chrono::seconds(5); // for the last frames of a closing connection

std::optional<Uuid> parse_uuid(std::string_view text) {
    std::string hex;
    for (const char c : text)
        if (c != '-') hex += c;
    if (hex.size() != 32) return std::nullopt;
    try {
        return Uuid{std::stoull(hex.substr(0, 16), nullptr, 16), std::stoull(hex.substr(16), nullptr, 16)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
} // namespace

Session::Session(asio::ip::tcp::socket socket, Shared& shared)
    : socket_(std::move(socket)), strand_(asio::make_strand(socket_.get_executor())), timer_(strand_),
      shared_(shared), id_(shared.next_id++), started_(std::chrono::steady_clock::now()) {
    asio::error_code ec;
    const auto ep = socket_.remote_endpoint(ec);
    remote_ = ec ? "unknown" : std::format("{}:{}", ep.address().to_string(), ep.port());
}

Session::~Session() = default;

void Session::start() {
    {
        std::lock_guard lock(shared_.sessions_mutex);
        shared_.sessions[id_] = weak_from_this();
    }
    asio::dispatch(strand_, [self = shared_from_this()] {
        self->schedule_timer();
        self->read();
    });
}

void Session::send(Bytes packet) {
    asio::post(strand_, [self = shared_from_this(), packet = std::move(packet)]() mutable { self->queue(std::move(packet)); });
}

void Session::kick(std::string reason) {
    asio::post(strand_, [self = shared_from_this(), reason = std::move(reason)] { self->fail(reason); });
}

void Session::close() {
    asio::post(strand_, [self = shared_from_this()] { self->terminate(); });
}

void Session::read() {
    if (phase_ == Phase::closed) return;
    socket_.async_read_some(asio::buffer(buffer_), asio::bind_executor(strand_, [self = shared_from_this()](
                                                                                   const asio::error_code& ec, std::size_t n) {
        if (ec) {
            self->terminate();
            return;
        }
        self->received(n);
    }));
}

void Session::received(std::size_t n) {
    if (phase_ == Phase::closed) return;
    if (in_cipher_) in_cipher_->apply(std::span<std::byte>(buffer_.data(), n));
    if (!decoder_.feed(std::span<const std::byte>(buffer_.data(), n))) {
        fail("Too much unread data");
        return;
    }
    while (phase_ != Phase::closed && !closing_) {
        const auto next = decoder_.next();
        if (!next) {
            fail("Malformed frame");
            return;
        }
        if (!*next) break;
        if (!handle(**next)) return;
    }
    if (!closing_) read();
}

bool Session::handle(const Bytes& packet) {
    const auto now = std::chrono::steady_clock::now();
    if (now - rate_window_ > std::chrono::seconds(1)) {
        rate_window_ = now;
        rate_count_ = 0;
    }
    if (++rate_count_ > max_packets_per_second) {
        fail("Too many packets");
        return false;
    }
    auto view = protocol::open_packet(packet);
    if (!view) {
        fail("Malformed packet");
        return false;
    }
    switch (phase_) {
    case Phase::handshake: return on_handshake(view->id, view->fields);
    case Phase::status: return on_status(view->id, view->fields);
    case Phase::login: case Phase::login_encrypting: case Phase::login_verifying: case Phase::login_acknowledging:
        return on_login(view->id, view->fields);
    case Phase::configuration: return on_configuration(view->id, view->fields);
    case Phase::play: return on_play(view->id, view->fields);
    case Phase::closed: return false;
    }
    return false;
}

bool Session::on_handshake(std::int32_t id, protocol::Reader& r) {
    if (id != shared_.wire.serverbound(State::handshaking, "intention")) {
        terminate();
        return false;
    }
    const auto protocol = r.varint();
    const auto address = r.string(255);
    const auto port = r.u16();
    const auto intent = r.varint();
    if (!protocol || !address || !port || !intent) {
        terminate();
        return false;
    }
    client_protocol_ = *protocol;
    if (*intent == 1) {
        phase_ = Phase::status;
    } else if (*intent == 2 || *intent == 3) {
        phase_ = Phase::login;
    } else {
        terminate();
        return false;
    }
    return true;
}

bool Session::on_status(std::int32_t id, protocol::Reader& r) {
    if (id == shared_.wire.serverbound(State::status, "status_request")) {
        const auto json = std::format(
            "{{\"version\":{{\"name\":\"{}\",\"protocol\":{}}},\"players\":{{\"max\":{},\"online\":{},\"sample\":[]}},"
            "\"description\":{{\"text\":\"{}\"}},\"enforcesSecureChat\":false}}",
            shared_.game_version, shared_.wire.protocol(), shared_.config.max_players, shared_.game.players(),
            json_escape(shared_.config.motd));
        queue(shared_.wire.status_response(json));
        return true;
    }
    if (id == shared_.wire.serverbound(State::status, "ping_request")) {
        const auto payload = r.i64();
        if (!payload) {
            terminate();
            return false;
        }
        queue(shared_.wire.pong_response(*payload));
        close_after_writes();
        return false;
    }
    terminate();
    return false;
}

bool Session::on_login(std::int32_t id, protocol::Reader& r) {
    if (phase_ == Phase::login && id == shared_.wire.serverbound(State::login, "hello")) {
        const auto name = r.string(16);
        const auto claimed = r.uuid();
        if (!name || !claimed) {
            fail("Malformed login");
            return false;
        }
        if (client_protocol_ != shared_.wire.protocol()) {
            fail(std::format("This server runs Java Edition {} (protocol {})", shared_.game_version, shared_.wire.protocol()));
            return false;
        }
        if (shared_.stopping) {
            fail("The server is stopping");
            return false;
        }
        if (!auth::valid_player_name(*name)) {
            fail("Invalid player name");
            return false;
        }
        if (shared_.config.online_mode) {
            // Default deny: verification is required but no verifier is available.
            if (!shared_.keys || !shared_.verifier || !shared_.auth_pool) {
                fail("Online verification is not available on this server");
                return false;
            }
            name_ = *name;
            verify_token_ = auth::random_bytes(4);
            queue(shared_.wire.encryption_request(shared_.keys->public_der(), verify_token_, true));
            phase_ = Phase::login_encrypting;
            return true;
        }
        const auto identity = shared_.offline.authenticate({*name, {}, {}}, std::chrono::steady_clock::now());
        if (!identity) {
            fail("Invalid player name");
            return false;
        }
        name_ = identity->name;
        uuid_ = parse_uuid(identity->uuid).value();
        verified_ = false;
        finish_login();
        return true;
    }
    if (phase_ == Phase::login_encrypting && id == shared_.wire.serverbound(State::login, "key")) {
        const auto sealed_secret = r.prefixed_bytes(256);
        const auto sealed_token = r.prefixed_bytes(256);
        if (!sealed_secret || !sealed_token) {
            fail("Malformed encryption response");
            return false;
        }
        const auto secret = shared_.keys->decrypt(*sealed_secret);
        const auto token = shared_.keys->decrypt(*sealed_token);
        if (!secret || !token || *token != verify_token_ || secret->size() != 16) {
            fail("Invalid encryption response");
            return false;
        }
        // Everything after this packet is encrypted in both directions,
        // including input that already arrived with it.
        in_cipher_ = std::make_unique<auth::Cfb8>(*secret, auth::Cfb8::Direction::decrypt);
        decoder_.transform_unread([this](std::span<std::byte> unread) { in_cipher_->apply(unread); });
        out_cipher_ = std::make_unique<auth::Cfb8>(*secret, auth::Cfb8::Direction::encrypt);
        phase_ = Phase::login_verifying;
        const auto hash = auth::login_hash("", *secret, shared_.keys->public_der());
        asio::post(*shared_.auth_pool, [self = shared_from_this(), name = name_, hash] {
            auto result = self->shared_.verifier->has_joined(name, hash, self->shared_.config.auth_timeout);
            asio::post(self->strand_, [self, result = std::move(result)]() mutable { self->verified(std::move(result)); });
        });
        return true;
    }
    if (phase_ == Phase::login_verifying) return true; // the client waits for the answer
    if (phase_ == Phase::login_acknowledging && id == shared_.wire.serverbound(State::login, "login_acknowledged")) {
        phase_ = Phase::configuration;
        protocol::Writer brand;
        brand.string("circuit");
        queue(shared_.wire.config_plugin_message("minecraft:brand", brand.bytes()));
        queue(shared_.wire.feature_flags({"minecraft:vanilla"}));
        queue(shared_.wire.known_packs(shared_.game_version));
        return true;
    }
    if (id == shared_.wire.serverbound(State::login, "custom_query_answer") || id == shared_.wire.serverbound(State::login, "cookie_response"))
        return true;
    fail("Unexpected login packet");
    return false;
}

void Session::verified(std::expected<auth::VerifiedProfile, auth::SessionError> result) {
    if (phase_ != Phase::login_verifying) return;
    if (!result) {
        log_info(std::format("{} ({}) was not verified: {}", name_, remote_, auth::describe(result.error())));
        fail(result.error() == auth::SessionError::not_joined ? "Failed to verify username"
                                                               : "Authentication servers are unavailable");
        return;
    }
    const auto uuid = parse_uuid(result->uuid);
    if (!uuid || !auth::valid_player_name(result->name)) {
        fail("Authentication returned an invalid profile");
        return;
    }
    name_ = result->name;
    uuid_ = *uuid;
    properties_.clear();
    for (auto& p : result->properties) properties_.push_back({std::move(p.name), std::move(p.value), std::move(p.signature)});
    verified_ = true;
    finish_login();
}

void Session::finish_login() {
    if (shared_.config.compression_threshold >= 0) {
        queue(shared_.wire.set_compression(shared_.config.compression_threshold));
        out_threshold_ = shared_.config.compression_threshold;
        decoder_.set_compression(shared_.config.compression_threshold);
    }
    queue(shared_.wire.login_success(uuid_, name_, properties_));
    phase_ = Phase::login_acknowledging;
}

bool Session::on_configuration(std::int32_t id, protocol::Reader& r) {
    const auto& w = shared_.wire;
    if (id == w.serverbound(State::configuration, "select_known_packs")) {
        const auto count = r.length(64, 3);
        if (!count) {
            fail("Malformed known packs");
            return false;
        }
        bool core = false;
        for (std::size_t i = 0; i < *count; ++i) {
            const auto ns = r.string(), pack = r.string(), version = r.string();
            if (!ns || !pack || !version) {
                fail("Malformed known packs");
                return false;
            }
            if (*ns == "minecraft" && *pack == "core" && *version == shared_.game_version) core = true;
        }
        if (core) {
            for (const auto& registry : shared_.registries) queue(w.registry_data(registry.registry, registry.entries));
            if (!shared_.tags.empty()) queue(w.update_tags(shared_.tags));
        } else {
            // Clients without the vanilla pack (protocol libraries) receive the
            // documented minimal registries with their data.
            for (const auto& registry : shared_.registries_with_data) queue(w.registry_data(registry.registry, registry.entries));
        }
        queue(w.finish_configuration());
        return true;
    }
    if (id == w.serverbound(State::configuration, "client_information")) {
        const auto locale = r.string(16);
        const auto distance = r.i8();
        const auto chat = r.varint();
        const auto colors = r.boolean();
        const auto parts = r.u8();
        const auto hand = r.varint();
        if (!locale || !distance || !chat || !colors || !parts || !hand || *hand < 0 || *hand > 1) {
            fail("Malformed client information");
            return false;
        }
        view_distance_ = std::max<std::int32_t>(2, *distance);
        skin_parts_ = *parts;
        main_hand_ = static_cast<std::uint8_t>(*hand);
        return true;
    }
    if (id == w.serverbound(State::configuration, "finish_configuration")) {
        phase_ = Phase::play;
        joined_ = true;
        keep_alive_sent_ = std::chrono::steady_clock::now();
        shared_.game.join({shared_from_this(), uuid_, name_, properties_, view_distance_, verified_, skin_parts_, main_hand_});
        return true;
    }
    // Plugin messages, keep-alive answers, pongs, cookies and resource-pack
    // answers need no action.
    return true;
}

bool Session::on_play(std::int32_t id, protocol::Reader& r) {
    if (id == shared_.wire.serverbound(State::play, "keep_alive")) {
        const auto v = r.i64();
        if (v && keep_alive_pending_ && *v == *keep_alive_pending_) keep_alive_pending_.reset();
        return true;
    }
    // Diagnostic: CIRCUIT_TRACE_PACKETS=1 logs every serverbound play packet.
    if (static const bool trace = std::getenv("CIRCUIT_TRACE_PACKETS") != nullptr; trace) {
        const auto name = shared_.wire.ids().name(State::play, Bound::serverbound, id);
        log_info(std::format("packet {} 0x{:02x} {} ({} bytes)", name_, id, name ? *name : "?", r.remaining()));
    }
    auto intent = shared_.wire.parse_play(id, r);
    if (!intent) {
        fail("Malformed packet");
        return false;
    }
    if (*intent) shared_.game.submit(id_, std::move(**intent));
    return true;
}

void Session::queue(Bytes packet) {
    if (phase_ == Phase::closed || closing_) return;
    auto frame = protocol::encode_frame(packet, out_threshold_);
    if (out_cipher_) out_cipher_->apply(frame);
    out_.push(std::move(frame));
    if (out_.bytes() > shared_.config.max_queued_bytes) {
        log_warn(std::format("{} ({}) cannot keep up; disconnected", name_.empty() ? remote_ : name_, remote_));
        terminate();
        return;
    }
    write();
}

// Starts writing the next frame unless a write is active. The completion
// handler owns the frame it writes, so terminating the connection, which
// discards the queue, never frees a buffer an outstanding write still reads.
void Session::write() {
    if (phase_ == Phase::closed) return;
    auto frame = out_.begin();
    if (!frame) {
        if (closing_ && !out_.writing()) terminate();
        return;
    }
    asio::async_write(socket_, asio::buffer(*frame),
                      asio::bind_executor(strand_, [self = shared_from_this(), frame](const asio::error_code& ec, std::size_t) {
                          // After terminate the queue was discarded; nothing is left to account for.
                          if (self->phase_ == Phase::closed) return;
                          self->out_.finish(frame);
                          if (ec) {
                              self->terminate();
                              return;
                          }
                          self->write();
                      }));
}

void Session::fail(std::string reason) {
    if (phase_ == Phase::closed || closing_) return;
    switch (phase_) {
    case Phase::login: case Phase::login_encrypting: case Phase::login_verifying: case Phase::login_acknowledging:
        queue(shared_.wire.login_disconnect(reason));
        break;
    case Phase::configuration: queue(shared_.wire.config_disconnect(reason)); break;
    case Phase::play: queue(shared_.wire.play_disconnect(reason)); break;
    default: break;
    }
    if (!name_.empty()) log_info(std::format("{} disconnected: {}", name_, reason));
    close_after_writes(); // the reason is written before the socket closes
}

// Closes once the queued frames are written. A peer that stops reading cannot
// hold the connection open: the timer closes it after `close_deadline`.
void Session::close_after_writes() {
    if (phase_ == Phase::closed || closing_) return;
    closing_ = true;
    closing_since_ = std::chrono::steady_clock::now();
    if (!out_.writing()) terminate();
}

void Session::terminate() {
    if (phase_ == Phase::closed) return;
    phase_ = Phase::closed;
    asio::error_code ec;
    timer_.cancel();
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec); // an outstanding write completes with an error and keeps its frame until then
    out_.discard();
    if (joined_) {
        joined_ = false;
        shared_.game.leave(id_);
    }
    std::lock_guard lock(shared_.sessions_mutex);
    shared_.sessions.erase(id_);
}

void Session::schedule_timer() {
    timer_.expires_after(std::chrono::seconds(1));
    timer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec) {
        if (!ec) self->timer_fired();
    }));
}

void Session::timer_fired() {
    if (phase_ == Phase::closed) return;
    const auto now = std::chrono::steady_clock::now();
    if (closing_) {
        if (now - closing_since_ > close_deadline) {
            terminate();
            return;
        }
    } else if (phase_ != Phase::play) {
        if (now - started_ > shared_.config.login_timeout) {
            fail("Took too long to log in");
            return;
        }
    } else if (keep_alive_pending_) {
        if (now - keep_alive_sent_ > shared_.config.keep_alive_timeout) {
            fail("Timed out");
            return;
        }
    } else if (now - keep_alive_sent_ >= keep_alive_interval) {
        keep_alive_sent_ = now;
        keep_alive_pending_ = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        queue(shared_.wire.keep_alive(*keep_alive_pending_));
    }
    schedule_timer();
}

} // namespace circuit::server
