// SPDX-License-Identifier: AGPL-3.0-only
// In-process server tests: a real Server on an ephemeral port, driven by a
// small client built on Circuit's codec. They check protocol state handling,
// limits and the translation of player actions into simulation commands.
#include "../support/check.hpp"
#include "test_client.hpp"
#include <circuit/auth.hpp>
#include <circuit/auth/crypto.hpp>
#include <circuit/protocol/content.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/runtime/engine.hpp>
#include <circuit/server/anvil.hpp>
#include <circuit/server/server.hpp>
#include <circuit/storage/world_directory.hpp>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <regex>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

using namespace circuit;
using namespace circuit::test;
using protocol::Bound;
using protocol::State;
using protocol::Writer;
using server::ServerConfig;

namespace {
ServerConfig offline_config() {
    ServerConfig c;
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.online_mode = false;
    c.workers = 2;
    c.network_threads = 2;
    c.view_distance = 3;
    c.simulation_distance = 2;
    return c;
}

const protocol::ContentTables& content() { return *protocol::ContentTables::for_protocol(772); }

std::string text_of(const TestClient::Packet& p) {
    auto r = p.reader();
    auto tag = protocol::nbt::read_network(r);
    if (!tag) return {};
    if (tag->type() == protocol::nbt::TagType::string) return tag->text();
    if (const auto* t = tag->find("text")) return t->text();
    return {};
}

// Waits for a block update at `pos` and returns its state id.
std::optional<std::int32_t> block_update(TestClient& c, protocol::WirePosition pos, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto want = c.id(State::play, Bound::clientbound, "block_update");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto p = c.next(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
        if (!p) return std::nullopt;
        if (p->id != want) continue;
        auto r = p->reader();
        if (r.position().value() == pos) return r.varint().value();
    }
    return std::nullopt;
}

// Confirms the spawn teleport and returns its position.
std::array<double, 3> confirm_spawn(TestClient& c) {
    auto pos = c.until(State::play, "player_position");
    require(pos.has_value(), "spawn position");
    auto r = pos->reader();
    c.send(State::play, "accept_teleportation", Writer().varint(r.varint().value()));
    const auto x = r.f64().value(), y = r.f64().value(), z = r.f64().value();
    return {x, y, z};
}

void use_item_on(TestClient& c, std::string_view item, protocol::WirePosition on, std::int32_t seq) {
    c.send(State::play, "set_creative_mode_slot", Writer().i16(36).varint(1).varint(content().item_id(item).value()).varint(0).varint(0));
    c.send(State::play, "use_item_on",
           Writer().varint(0).position(on).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).boolean(false).boolean(false).varint(seq));
}

std::filesystem::path temporary_directory() {
    std::string templ = (std::filesystem::temp_directory_path() / "circuit-server-XXXXXX").string();
    require(::mkdtemp(templ.data()) != nullptr, "temporary directory");
    return templ;
}

// The engine state a world directory recovers to, read without a server.
gameplay::WorldDump recovered_state(const std::filesystem::path& dir) {
    const auto& bundle = **contracts::BundleRegistry::select_for_simulation("java-1.21.8");
    storage::PosixFilesystem fs;
    storage::WorldDirectory world(fs, dir, "java-1.21.8", std::string(bundle.rules->id));
    auto r = world.recover();
    require(r && *r, std::format("world recovers: {}", r ? "never saved" : r.error()));
    runtime::Engine engine(bundle, (*r)->checkpoint.seed, (*r)->checkpoint.state);
    for (const auto& input : (*r)->inputs) engine.advance(input.commands);
    return engine.dump();
}
std::uint32_t block_at(const gameplay::WorldDump& d, contracts::BlockPos p) {
    for (const auto& [pos, raw] : d.blocks)
        if (pos == p) return raw;
    return 0;
}
} // namespace

int main() {
    Suite suite("server_tests");

    suite.run("configuration files", [] {
        const auto c = server::parse_config("# comment\nport=25570\nonline-mode=false\nview-distance=8\nmotd=Hi there\n");
        require(c && c->port == 25570 && !c->online_mode && c->view_distance == 8 && c->motd == "Hi there", "parsed");
        require(!server::parse_config("port=70000\n"), "range");
        require(!server::parse_config("bogus=1\n"), "unknown key");
        require(!server::parse_config("online-mode=maybe\n"), "boolean");
        require(server::parse_config("").value().online_mode, "online mode by default");
        const auto w = server::parse_config("world-directory=/srv/world\nautosave-ticks=1200\nlog-sync-ticks=1\n");
        require(w && w->world_directory == "/srv/world" && w->autosave_ticks == 1200 && w->log_sync_ticks == 1, "world settings");
        require(!server::parse_config("log-sync-ticks=0\n") && !server::parse_config("autosave-ticks=5\n"), "world setting ranges");
    });

    suite.run("protocol encryption primitives", [] {
        const auto digest = [](std::string_view s) {
            return auth::minecraft_hex_digest(std::as_bytes(std::span(s.data(), s.size())));
        };
        // Documented examples (minecraft.wiki "Java Edition protocol/Encryption").
        require(digest("Notch") == "4ed1f46bbe04bc756bcb17c0c7ce3e4632f06a48", "positive digest");
        require(digest("jeb_") == "-7c9d5b0044c130109a5d7b5fb5c317c02b4e28c1", "negative digest");
        require(digest("simon") == "88e16a1019277b15d58faf0541e11910eb756f6", "leading zero dropped");
        const auto keys = auth::RsaKeyPair::generate();
        require(keys.public_der().size() > 100 && keys.public_der().size() < 200, "1024-bit SubjectPublicKeyInfo");
        const auto secret = auth::random_bytes(16);
        const auto sealed = auth::rsa_encrypt(keys.public_der(), secret);
        require(sealed && sealed->size() == 128 && keys.decrypt(*sealed).value() == secret, "RSA round trip");
        // OpenSSL rejects bad padding implicitly: garbage yields unrelated bytes,
        // never the secret, so the verify-token comparison refuses it.
        const auto garbage = keys.decrypt(auth::random_bytes(128));
        require(!garbage || *garbage != secret, "garbage does not yield the secret");
        // CFB8 is a stream cipher: chunking does not change the output.
        auto plain = auth::random_bytes(100);
        auto whole = plain, pieces = plain;
        auth::Cfb8 a(secret, auth::Cfb8::Direction::encrypt), b(secret, auth::Cfb8::Direction::encrypt);
        a.apply(whole);
        b.apply(std::span(pieces).first(7));
        b.apply(std::span(pieces).subspan(7));
        require(whole == pieces && whole != plain, "continuous stream");
        auth::Cfb8 d(secret, auth::Cfb8::Direction::decrypt);
        d.apply(whole);
        require(whole == plain, "decrypts");
    });

    suite.run("status ping", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.send(c.id(State::handshaking, Bound::serverbound, "intention"), Writer().varint(772).string("localhost").u16(25565).varint(1));
        c.send(State::status, "status_request");
        auto status = c.next();
        require(status && status->id == c.id(State::status, Bound::clientbound, "status_response"), "status response");
        auto r = status->reader();
        const auto json = r.string().value();
        require(json.find("\"protocol\":772") != std::string::npos && json.find("\"name\":\"1.21.8\"") != std::string::npos, json);
        c.send(State::status, "ping_request", Writer().i64(123456789));
        auto pong = c.next();
        require(pong && pong->reader().i64().value() == 123456789, "pong echoes the payload");
        s.stop();
    });

    suite.run("offline login reaches play", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.start_login("Steve");
        auto compression = c.next();
        require(compression && compression->id == c.id(State::login, Bound::clientbound, "login_compression"), "compression");
        c.enable_compression(compression->reader().varint().value());
        auto success = c.next();
        require(success && success->id == c.id(State::login, Bound::clientbound, "login_finished"), "login success");
        auto r = success->reader();
        require(r.uuid().value().text() == auth::offline_uuid("Steve") && r.string(16).value() == "Steve",
                "offline identity is the name-based UUID");
        c.send(State::login, "login_acknowledged");
        std::vector<std::string> registries;
        c.on_skip = [&](const TestClient::Packet& p) {
            if (p.id == c.id(State::configuration, Bound::clientbound, "registry_data")) {
                auto rr = p.reader();
                registries.push_back(rr.string().value());
            }
        };
        require(c.until(State::configuration, "select_known_packs").has_value(), "known packs offered");
        c.send(State::configuration, "select_known_packs", Writer().varint(1).string("minecraft").string("core").string("1.21.8"));
        require(c.until(State::configuration, "finish_configuration").has_value(), "configuration finished");
        require(registries.size() == 12 && registries[0] == "minecraft:dimension_type", "twelve registries sent");
        c.on_skip = nullptr;
        c.send(State::configuration, "finish_configuration");
        auto login = c.until(State::play, "login");
        require(login.has_value(), "play login");
        confirm_spawn(c);
        require(c.until(State::play, "level_chunk_with_light").has_value(), "chunks delivered");
        for (int i = 0; i < 100 && s.players() != 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(s.players() == 1, "one player");
        s.stop();
    });

    suite.run("online mode verifies sessions and denies otherwise", [] {
        // A loopback session service that answers every request with a fixed reply.
        struct MockSessions {
            asio::io_context io;
            asio::ip::tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
            std::string reply;
            std::string last_request;
            std::thread thread;
            void serve() {
                acceptor.async_accept([this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
                    if (ec) return;
                    asio::streambuf buf;
                    asio::error_code e;
                    asio::read_until(socket, buf, "\r\n\r\n", e);
                    last_request.assign(asio::buffers_begin(buf.data()), asio::buffers_end(buf.data()));
                    asio::write(socket, asio::buffer(reply), e);
                    socket.shutdown(asio::ip::tcp::socket::shutdown_both, e);
                    serve();
                });
            }
            explicit MockSessions(std::string r) : reply(std::move(r)) {
                serve();
                thread = std::thread([this] { io.run(); });
            }
            ~MockSessions() {
                io.stop();
                thread.join();
            }
            std::string url() const { return std::format("http://127.0.0.1:{}", acceptor.local_endpoint().port()); }
        };
        const auto body = std::string(R"({"id":"0123456789abcdef0123456789abcdef","name":"Verified",)"
                                      R"("properties":[{"name":"textures","value":"dGV4dHVyZXM=","signature":"c2ln"}]})");
        MockSessions ok(std::format("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}",
                                    body.size(), body));
        MockSessions denied("HTTP/1.1 204 No Content\r\n\r\n");

        // Runs the client side of the key exchange; returns the login hash it computed.
        const auto exchange = [](TestClient& c, bool forge_token) {
            auto hello = c.next();
            require(hello && hello->id == c.id(State::login, Bound::clientbound, "hello"), "encryption request");
            auto r = hello->reader();
            const auto server_id = r.string(20).value();
            const auto key = r.prefixed_bytes(1024).value();
            auto token = r.prefixed_bytes(64).value();
            require(r.boolean().value(), "the client is asked to authenticate");
            if (forge_token) token[0] ^= std::byte{1};
            const auto secret = auth::random_bytes(16);
            c.send(State::login, "key",
                   Writer().prefixed_bytes(auth::rsa_encrypt(key, secret).value()).prefixed_bytes(auth::rsa_encrypt(key, token).value()));
            // A server that refuses the token never starts encryption.
            if (!forge_token) c.enable_encryption(secret);
            return auth::login_hash(server_id, secret, key);
        };

        auto config = offline_config();
        config.online_mode = true;
        config.session_server = ok.url();
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.start_login("claimed");
            const auto hash = exchange(c, false);
            auto compression = c.next();
            require(compression && compression->id == c.id(State::login, Bound::clientbound, "login_compression"),
                    "encrypted compression packet");
            c.enable_compression(compression->reader().varint().value());
            auto success = c.next();
            require(success && success->id == c.id(State::login, Bound::clientbound, "login_finished"), "verified login");
            auto r = success->reader();
            require(r.uuid().value().text() == "01234567-89ab-cdef-0123-456789abcdef", "profile id from the service");
            require(r.string(16).value() == "Verified", "profile name from the service");
            require(r.varint().value() == 1 && r.string(64).value() == "textures", "profile properties");
            require(ok.last_request.find(std::format("username=claimed&serverId={}", hash)) != std::string::npos,
                    "server and client agree on the login hash");
            s.stop();
        }
        config.session_server = denied.url();
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.start_login("claimed");
            exchange(c, false);
            auto p = c.next();
            require(p && p->id == c.id(State::login, Bound::clientbound, "login_disconnect"), "unconfirmed login refused");
            require(p->reader().string().value().find("Failed to verify") != std::string::npos, "reason");
            TestClient forged(s.port());
            forged.start_login("claimed");
            exchange(forged, true);
            auto q = forged.next();
            require(q && q->id == c.id(State::login, Bound::clientbound, "login_disconnect")
                        && q->reader().string().value().find("Invalid encryption") != std::string::npos,
                    "forged token refused");
            s.stop();
        }
        config.session_server = "http://127.0.0.1:1"; // nothing listens there
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.start_login("claimed");
            exchange(c, false);
            auto p = c.next(std::chrono::milliseconds(8000));
            require(p && p->reader().string().value().find("unavailable") != std::string::npos, "service down: denied");
            s.stop();
        }
        bool refused = false;
        config.session_server = "http://example.com";
        try {
            server::Server s(config);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        require(refused, "plain http only on loopback");
    });

    suite.run("wrong versions, bad names and broken frames are refused", [] {
        server::Server s(offline_config());
        s.start();
        {
            TestClient c(s.port());
            c.start_login("Steve", 769);
            auto p = c.next();
            require(p && p->id == c.id(State::login, Bound::clientbound, "login_disconnect"), "other protocol refused");
        }
        {
            TestClient c(s.port());
            c.start_login("bad name!");
            auto p = c.next();
            require(p && p->id == c.id(State::login, Bound::clientbound, "login_disconnect"), "invalid name refused");
        }
        {
            TestClient c(s.port());
            c.send_raw(Writer().varint(2097152).take()); // beyond the frame limit
            require(!c.next(std::chrono::milliseconds(2000)) && c.closed(), "oversized frame closes the connection");
        }
        {
            TestClient c(s.port());
            c.send_raw(Bytes{std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x01}});
            require(!c.next(std::chrono::milliseconds(2000)) && c.closed(), "five-byte frame length closes the connection");
        }
        s.stop();
    });

    suite.run("connection limit", [] {
        auto config = offline_config();
        config.max_connections = 2;
        server::Server s(config);
        s.start();
        TestClient a(s.port()), b(s.port());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        TestClient c(s.port());
        require(!c.next(std::chrono::milliseconds(2000)) && c.closed(), "third connection refused");
        s.stop();
    });

    suite.run("login timeout", [] {
        auto config = offline_config();
        config.login_timeout = std::chrono::seconds(1);
        server::Server s(config);
        s.start();
        TestClient c(s.port());
        require(!c.next(std::chrono::milliseconds(4000)) && c.closed(), "idle connection closed");
        s.stop();
    });

    suite.run("movement validation", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.join("Mover");
        confirm_spawn(c);
        c.send(State::play, "move_player_pos", Writer().f64(0.5).f64(-59.0).f64(1.5).u8(1));
        c.send(State::play, "move_player_pos", Writer().f64(60.5).f64(-59.0).f64(1.5).u8(1)); // 60 blocks in one packet
        auto correction = c.until(State::play, "player_position");
        require(correction.has_value(), "too-fast move corrected");
        auto r = correction->reader();
        const auto teleport = r.varint().value();
        const auto x = r.f64().value();
        require(teleport == 2 && std::abs(x - 0.5) < 1e-9, "back to the last accepted position");
        c.send(State::play, "accept_teleportation", Writer().varint(teleport));
        c.send(State::play, "move_player_pos", Writer().f64(std::nan("")).f64(-59.0).f64(1.5).u8(1));
        auto kick = c.until(State::play, "disconnect");
        require(kick && text_of(*kick).find("Invalid movement") != std::string::npos, "non-finite position kicked");
        s.stop();
    });

    suite.run("creative placement and breaking", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.join("Builder");
        confirm_spawn(c);
        const auto stone_item = content().item_id("minecraft:stone").value();
        c.send(State::play, "set_creative_mode_slot", Writer().i16(36).varint(1).varint(stone_item).varint(0).varint(0));
        // Click the top face of the floor block at (0, -61, 2).
        c.send(State::play, "use_item_on",
               Writer().varint(0).position({0, -61, 2}).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).boolean(false).boolean(false).varint(7));
        require(block_update(c, {0, -60, 2}) == content().state_id("minecraft:stone"), "stone placed");
        auto ack = c.until(State::play, "block_changed_ack");
        require(ack && ack->reader().varint().value() == 7, "placement acknowledged");
        c.send(State::play, "player_action", Writer().varint(0).position({0, -60, 2}).i8(1).varint(8));
        require(block_update(c, {0, -60, 2}) == 0, "stone broken");
        // Out of reach: the server answers with the current state instead.
        c.send(State::play, "use_item_on",
               Writer().varint(0).position({0, -61, 30}).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).boolean(false).boolean(false).varint(9));
        require(block_update(c, {0, -60, 30}) == 0, "far placement refused");
        s.stop();
    });

    suite.run("a lever lights a lamp through the engine", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.join("Wirer");
        confirm_spawn(c);
        const auto place = [&](std::string_view item, protocol::WirePosition on, std::int32_t seq) {
            c.send(State::play, "set_creative_mode_slot",
                   Writer().i16(36).varint(1).varint(content().item_id(item).value()).varint(0).varint(0));
            c.send(State::play, "use_item_on",
                   Writer().varint(0).position(on).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).boolean(false).boolean(false).varint(seq));
        };
        place("minecraft:redstone_lamp", {2, -61, 1}, 1);
        require(block_update(c, {2, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=false]"), "lamp placed");
        place("minecraft:lever", {1, -61, 1}, 2);
        require(block_update(c, {1, -60, 1}).has_value(), "lever placed");
        c.send(State::play, "use_item_on",
               Writer().varint(0).position({1, -60, 1}).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).boolean(false).boolean(false).varint(3));
        require(block_update(c, {2, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=true]"), "lamp lit");
        s.stop();
    });

    suite.run("too many packets", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.join("Flooder");
        try {
            for (int i = 0; i < 1500; ++i) c.send(State::play, "client_tick_end");
        } catch (const std::exception&) {
            // The server may close the connection before the flood is written.
        }
        std::optional<TestClient::Packet> kick;
        try {
            kick = c.until(State::play, "disconnect");
        } catch (const std::exception&) {
        }
        require(!kick || text_of(*kick).find("Too many packets") != std::string::npos, "the only answer is the kick");
        for (int i = 0; i < 200 && s.players() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(s.players() == 0, "the flooding player was removed");
        s.stop();
    });

    suite.run("stopping kicks players with a reason", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.join("Leaver");
        confirm_spawn(c);
        std::thread stopper([&] { s.stop(); });
        auto kick = c.until(State::play, "disconnect", std::chrono::milliseconds(5000));
        stopper.join();
        require(kick && text_of(*kick).find("stopping") != std::string::npos, "stop reason delivered");
    });

    suite.run("a 1.21.4 server serves protocol 769", [] {
        auto config = offline_config();
        config.game_version = "java-1.21.4";
        server::Server s(config);
        s.start();
        const auto& tables = *protocol::ContentTables::for_protocol(769);
        {
            TestClient c(s.port(), 769);
            c.send(c.id(State::handshaking, Bound::serverbound, "intention"), Writer().varint(769).string("localhost").u16(25565).varint(1));
            c.send(State::status, "status_request");
            auto status = c.next();
            require(status && status->id == c.id(State::status, Bound::clientbound, "status_response"), "status response");
            const auto json = status->reader().string().value();
            require(json.find("\"protocol\":769") != std::string::npos && json.find("\"name\":\"1.21.4\"") != std::string::npos, json);
        }
        {
            TestClient c(s.port(), 772);
            c.start_login("Steve");
            auto p = c.next();
            require(p && p->id == c.id(State::login, Bound::clientbound, "login_disconnect"), "a 1.21.8 client is refused");
        }
        TestClient c(s.port(), 769);
        c.join("OldClient");
        auto pos = c.until(State::play, "player_position");
        require(pos.has_value(), "spawn");
        c.send(State::play, "accept_teleportation", Writer().varint(pos->reader().varint().value()));
        const auto place = [&](std::string_view item, protocol::WirePosition on, std::int32_t seq) {
            c.send(State::play, "set_creative_mode_slot", Writer().i16(36).varint(1).varint(tables.item_id(item).value()).varint(0).varint(0));
            c.send(State::play, "use_item_on",
                   Writer().varint(0).position(on).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).boolean(false).boolean(false).varint(seq));
        };
        const auto update = [&](protocol::WirePosition at) -> std::optional<std::int32_t> {
            const auto want = c.id(State::play, Bound::clientbound, "block_update");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                auto p = c.next(std::chrono::milliseconds(500));
                if (!p || p->id != want) continue;
                auto r = p->reader();
                if (r.position().value() == at) return r.varint().value();
            }
            return std::nullopt;
        };
        place("minecraft:redstone_lamp", {2, -61, 1}, 1);
        require(update({2, -60, 1}) == tables.state_id("minecraft:redstone_lamp[lit=false]"), "lamp placed with 1.21.4 ids");
        place("minecraft:lever", {1, -61, 1}, 2);
        require(update({1, -60, 1}).has_value(), "lever placed");
        c.send(State::play, "use_item_on",
               Writer().varint(0).position({1, -60, 1}).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).boolean(false).boolean(false).varint(3));
        require(update({2, -60, 1}) == tables.state_id("minecraft:redstone_lamp[lit=true]"), "the lamp lights on 1.21.4");
        s.stop();
    });

    suite.run("the world and players are saved and restored", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        const auto stone = gameplay::blocks::stone();
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Keeper");
            confirm_spawn(c);
            use_item_on(c, "minecraft:stone", {0, -61, 2}, 1);
            require(block_update(c, {0, -60, 2}) == content().state_id("minecraft:stone"), "stone placed");
            c.send(State::play, "move_player_pos", Writer().f64(3.5).f64(-60.0).f64(4.5).u8(1));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            s.stop();
        }
        require(block_at(recovered_state(config.world_directory), {0, -60, 2}) == stone.raw(), "stone on disk");
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Keeper");
            const auto at = confirm_spawn(c);
            require(at[0] == 3.5 && at[1] == -60.0 && at[2] == 4.5, std::format("saved position, not {} {} {}", at[0], at[1], at[2]));
            // The world continues: break the stone in the second session.
            c.send(State::play, "player_action", Writer().varint(0).position({0, -60, 2}).i8(1).varint(2));
            require(block_update(c, {0, -60, 2}) == 0, "stone broken");
            s.stop();
        }
        require(block_at(recovered_state(config.world_directory), {0, -60, 2}) == 0, "break saved");
        std::filesystem::remove_all(dir);
    });

    suite.run("a killed server process loses no synced input", [] {
        // The real server binary, killed with SIGKILL while players act; the
        // input log is synced every tick.
        const auto dir = temporary_directory();
        const auto world = dir / "world";
        std::ofstream(dir / "server.conf") << std::format(
            "bind-address=127.0.0.1\nport=0\nonline-mode=false\nworkers=2\nview-distance=3\nsimulation-distance=2\n"
            "log-sync-ticks=1\nworld-directory={}\n", world.string());
        int out[2];
        require(::pipe(out) == 0, "pipe");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], 2);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        const std::string binary = CIRCUIT_SERVER_BINARY, config_path = (dir / "server.conf").string();
        std::vector<char*> argv{const_cast<char*>(binary.c_str()), const_cast<char*>("--serve"), const_cast<char*>("--config"),
                                const_cast<char*>(config_path.c_str()), nullptr};
        pid_t pid = 0;
        require(posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), environ) == 0, "server process started");
        posix_spawn_file_actions_destroy(&actions);
        ::close(out[1]);
        // Read the log until the port appears, then keep draining it.
        std::string log;
        std::uint16_t port = 0;
        const std::regex listening(R"(listening on 127\.0\.0\.1:(\d+))");
        while (port == 0) {
            char buf[4096];
            const auto n = ::read(out[0], buf, sizeof buf);
            if (n <= 0) break;
            log.append(buf, static_cast<std::size_t>(n));
            if (std::smatch m; std::regex_search(log, m, listening)) port = static_cast<std::uint16_t>(std::stoi(m[1]));
        }
        std::thread drain([fd = out[0]] {
            char buf[4096];
            while (::read(fd, buf, sizeof buf) > 0) {
            }
        });
        const auto kill_server = [&] {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            drain.join();
            ::close(out[0]);
            return status;
        };
        try {
            require(port != 0, "server listening: " + log);
            TestClient c(port);
            c.join("Victim");
            confirm_spawn(c);
            use_item_on(c, "minecraft:redstone_lamp", {2, -61, 1}, 1);
            require(block_update(c, {2, -60, 1}).has_value(), "lamp placed");
            use_item_on(c, "minecraft:lever", {1, -61, 1}, 2);
            require(block_update(c, {1, -60, 1}).has_value(), "lever placed");
            c.send(State::play, "use_item_on",
                   Writer().varint(0).position({1, -60, 1}).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).boolean(false).boolean(false).varint(3));
            require(block_update(c, {2, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=true]"), "lamp lit");
        } catch (...) {
            kill_server();
            throw;
        }
        const auto status = kill_server();
        require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "the server was killed");
        // Every published change was logged before it was applied.
        const auto state = recovered_state(world);
        require(block_at(state, {2, -60, 1}) == gameplay::blocks::lamp(true).raw(), "lit lamp recovered");
        // A server opens the killed world and continues it.
        auto config = offline_config();
        config.world_directory = world;
        server::Server s(config);
        s.start();
        TestClient c(s.port());
        c.join("Victim");
        confirm_spawn(c);
        s.stop();
        require(block_at(recovered_state(world), {1, -60, 1}) == block_at(state, {1, -60, 1}), "lever kept after restart");
        std::filesystem::remove_all(dir);
    });

    suite.run("worlds move between bundles only through a migration", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Mover");
            confirm_spawn(c);
            use_item_on(c, "minecraft:stone", {0, -61, 3}, 1);
            require(block_update(c, {0, -60, 3}).has_value(), "stone placed");
            s.stop();
        }
        auto old_client = config;
        old_client.game_version = "java-1.21.4";
        bool refused = false;
        try {
            server::Server s(old_client);
        } catch (const std::runtime_error& e) {
            refused = std::string_view(e.what()).find("java-1.21.8") != std::string_view::npos;
        }
        require(refused, "a 1.21.8 world is not opened by a 1.21.4 server");
        require(!server::migrate_world(config.world_directory, dir / "unsupported", "java-1.20.4"), "no migration to a bundle without rules");
        const auto tick = server::migrate_world(config.world_directory, dir / "world-1.21.4", "java-1.21.4");
        require(tick.has_value(), std::format("migration: {}", tick ? "" : tick.error()));
        require(!server::migrate_world(config.world_directory, dir / "world-1.21.4", "java-1.21.4"), "an existing target is not overwritten");
        old_client.world_directory = dir / "world-1.21.4";
        {
            server::Server s(old_client);
            s.start();
            TestClient c(s.port(), 769);
            c.join("Mover");
            const auto at = confirm_spawn(c);
            require(at[0] == 0.5 && at[1] == -60.0 && at[2] == 0.5, "the player's saved position came along");
            s.stop();
        }
        // The migrated world keeps the stone.
        {
            const auto& bundle = **contracts::BundleRegistry::select_for_simulation("java-1.21.4");
            storage::PosixFilesystem fs;
            storage::WorldDirectory world(fs, dir / "world-1.21.4", "java-1.21.4", std::string(bundle.rules->id));
            auto r = world.recover();
            require(r && *r, "migrated world recovers");
            runtime::Engine engine(bundle, (*r)->checkpoint.seed, (*r)->checkpoint.state);
            for (const auto& input : (*r)->inputs) engine.advance(input.commands);
            require(block_at(engine.dump(), {0, -60, 3}) == gameplay::blocks::stone().raw(), "stone migrated");
        }
        // Anvil interchange is defined for 1.21.8 worlds only.
        auto anvil_config = old_client;
        anvil_config.world_directory = dir / "anvil-target";
        const auto imported = server::import_anvil_world(dir / "nowhere", anvil_config);
        require(!imported, "no Anvil import for 1.21.4");
        std::filesystem::remove_all(dir);
    });

    suite.run("a world from other rules is refused", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        { server::Server s(config); }
        std::string manifest;
        {
            std::ifstream in(config.world_directory / "manifest");
            manifest.assign(std::istreambuf_iterator<char>(in), {});
        }
        const auto at = manifest.find("rules ");
        manifest.replace(at, manifest.find('\n', at) - at, "rules some-other-rules");
        std::ofstream(config.world_directory / "manifest", std::ios::trunc) << manifest;
        bool refused = false;
        try {
            server::Server s(config);
        } catch (const std::runtime_error& e) {
            refused = std::string_view(e.what()).find("some-other-rules") != std::string_view::npos;
        }
        require(refused, "opening refused with the reason");
        std::filesystem::remove_all(dir);
    });

    return suite.finish();
}
