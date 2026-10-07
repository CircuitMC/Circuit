// SPDX-License-Identifier: AGPL-3.0-only
// In-process server tests: a real Server on an ephemeral port, driven by a
// small client built on Circuit's codec. They check protocol state handling,
// limits and the translation of player actions into simulation commands.
#include "../support/check.hpp"
#include "test_client.hpp"
#include "../../src/server/game.hpp"
#include "../../src/server/world_meta.hpp"
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
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <tuple>
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
    // The replay generates new columns as the server does: player commands
    // are resolved against the terrain.
    auto floor_y = server::fresh_meta(-61).floor_y;
    if (const auto it = (*r)->checkpoint.extras.find(std::string(server::meta_extra)); it != (*r)->checkpoint.extras.end())
        floor_y = server::decode_meta(it->second).value().floor_y;
    runtime::Engine engine(bundle, (*r)->checkpoint.seed, (*r)->checkpoint.state, server::world_engine_options(bundle, floor_y, 1));
    for (const auto& input : (*r)->inputs) engine.advance(input.commands);
    return engine.dump();
}
// Runs the server binary to its exit; returns the exit code (-1 when it was
// killed after `limit`) and what it printed.
std::pair<int, std::string> run_server_binary(const std::vector<std::string>& args, std::chrono::seconds limit = std::chrono::seconds(30)) {
    int out[2];
    require(::pipe(out) == 0, "pipe");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, out[1], 2);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    const std::string binary = CIRCUIT_SERVER_BINARY;
    std::vector<char*> argv{const_cast<char*>(binary.c_str())};
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    require(posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), environ) == 0, "server process started");
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    std::string output;
    std::thread reader([&] {
        char buf[4096];
        for (ssize_t n; (n = ::read(out[0], buf, sizeof buf)) > 0;) output.append(buf, static_cast<std::size_t>(n));
    });
    const auto deadline = std::chrono::steady_clock::now() + limit;
    int status = 0;
    while (::waitpid(pid, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    reader.join();
    ::close(out[0]);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

// Every file of a world directory and its content.
std::map<std::string, std::string> world_files(const std::filesystem::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::ifstream in(e.path(), std::ios::binary);
        out[e.path().filename().string()] = std::string(std::istreambuf_iterator<char>(in), {});
    }
    return out;
}

// A game driven directly: intents go into its inbox and each tick() takes
// the whole inbox, so a test decides which intents share a tick.
struct Recorder final : server::Peer {
    explicit Recorder(std::uint64_t id) : peer_id(id) {}
    std::uint64_t id() const noexcept override { return peer_id; }
    void send(protocol::Bytes p) override {
        std::lock_guard lock(mutex);
        packets.push_back(std::move(p));
    }
    void kick(std::string reason) override {
        std::lock_guard lock(mutex);
        kicks.push_back(std::move(reason));
    }
    std::uint64_t peer_id;
    std::mutex mutex;
    std::vector<protocol::Bytes> packets;
    std::vector<std::string> kicks;
};

struct GameFixture {
    explicit GameFixture(ServerConfig c = offline_config()) : config(std::move(c)) {
        game = std::make_unique<server::Game>(config, bundle, wire, content(), mapping, *server::synced_registries(772));
    }
    ~GameFixture() { game->shutdown("test finished"); }
    std::shared_ptr<Recorder> join(std::uint64_t id, std::string name, std::int32_t view = 0) {
        auto peer = std::make_shared<Recorder>(id);
        game->join({peer, {id, id}, std::move(name), {}, view == 0 ? config.view_distance : view, false});
        game->tick();
        return peer;
    }
    std::string at(contracts::BlockPos p) const {
        const auto s = game->engine().view().block(p);
        return s ? mapping.notation(*s) : "unloaded";
    }
    void hold(std::uint64_t peer, std::string_view item) {
        game->submit(peer, server::intent::CreativeSlot{36, 1, content().item_id(item).value()});
    }
    // Clicks the top face of `pos`.
    static server::Intent click(protocol::WirePosition pos, std::int32_t seq, std::int32_t face = 1) {
        return server::intent::UseOn{0, pos, face, 0.5f, 1.0f, 0.5f, false, seq};
    }

    const contracts::VersionBundle& bundle = **contracts::BundleRegistry::select_for_simulation("java-1.21.8");
    const evidence::ContentMapping& mapping = *evidence::ContentMapping::for_game_version("1.21.8");
    server::Wire wire{772};
    ServerConfig config;
    std::unique_ptr<server::Game> game;
};

gameplay::PlayerInventory empty_inventory(contracts::PlayerKey key) {
    gameplay::PlayerInventory inventory;
    inventory.player = key;
    return inventory;
}

// A click as the client sends it, predicting no changed slot and an empty
// carried stack.
server::Intent window_click(std::int32_t window, std::int16_t slot, std::int8_t button, std::int32_t mode, std::int32_t state = 0) {
    server::intent::ContainerClick click;
    click.carried = server::WireStack{};
    click.window = window;
    click.state = state;
    click.slot = slot;
    click.button = button;
    click.mode = mode;
    return click;
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

    suite.run("player actions mean the same however they are grouped into ticks", [] {
        // Each action depends on the one before it: stacking on a block placed
        // in the same tick, dust on it, using a lever placed just before, and
        // placing where a block was just broken.
        using I = server::Intent;
        const auto item = [](std::string_view name) { return I{server::intent::CreativeSlot{36, 1, content().item_id(name).value()}}; };
        const auto dig = [](protocol::WirePosition pos, std::int32_t seq) { return I{server::intent::Dig{0, pos, 1, seq}}; };
        const auto click = [](protocol::WirePosition pos, std::int32_t seq) { return GameFixture::click(pos, seq); };
        const std::vector<I> actions{
            item("minecraft:stone"), click({2, -61, 2}, 1), click({2, -60, 2}, 2), click({2, -59, 2}, 3),
            click({3, -61, 2}, 4), item("minecraft:redstone"), click({3, -60, 2}, 5),
            item("minecraft:redstone_lamp"), click({5, -61, 2}, 6), item("minecraft:lever"), click({4, -61, 2}, 7),
            click({4, -60, 2}, 8),
            item("minecraft:stone"), click({6, -61, 2}, 9), dig({6, -60, 2}, 10), item("minecraft:glass"), click({6, -61, 2}, 11),
        };
        const auto run = [&](const std::vector<std::size_t>& group_sizes) {
            GameFixture f;
            f.join(1, "Builder");
            std::size_t next = 0;
            for (const auto n : group_sizes) {
                for (std::size_t i = 0; i < n; ++i) f.game->submit(1, actions[next++]);
                f.game->tick();
            }
            for (int i = 0; i < 10; ++i) f.game->tick();
            std::string out;
            for (std::int32_t x = 1; x <= 7; ++x)
                for (std::int32_t y = -60; y <= -57; ++y) out += f.at({x, y, 2}) + ";";
            return out;
        };
        const auto one_tick = run({actions.size()});
        std::vector<std::size_t> each(actions.size(), 1);
        require(run(each) == one_tick, "one action per tick equals all actions in one tick");
        std::mt19937 rng(7);
        for (int trial = 0; trial < 4; ++trial) {
            std::vector<std::size_t> groups;
            for (std::size_t left = actions.size(); left > 0;) {
                const auto n = std::min<std::size_t>(left, std::uniform_int_distribution<std::size_t>(1, 5)(rng));
                groups.push_back(n);
                left -= n;
            }
            require(run(groups) == one_tick, std::format("grouping {} equals one tick", trial));
        }
        GameFixture f;
        f.join(1, "Builder");
        for (const auto& a : actions) f.game->submit(1, a);
        f.game->tick();
        for (int i = 0; i < 10; ++i) f.game->tick();
        require(f.at({2, -60, 2}) == "minecraft:stone" && f.at({2, -59, 2}) == "minecraft:stone" && f.at({2, -58, 2}) == "minecraft:stone",
                "stacked on blocks placed in the same tick");
        require(f.at({3, -59, 2}).starts_with("minecraft:redstone_wire"), "dust on a block placed in the same tick: " + f.at({3, -59, 2}));
        require(f.at({4, -60, 2}).find("powered=true") != std::string::npos, "a lever placed in the same tick was used: " + f.at({4, -60, 2}));
        require(f.at({5, -60, 2}) == "minecraft:redstone_lamp[lit=true]", "and lit the lamp: " + f.at({5, -60, 2}));
        require(f.at({6, -60, 2}) == "minecraft:glass", "placed where a block was broken in the same tick");
    });

    suite.run("players pair chests as they place them", [] {
        GameFixture f;
        f.join(1, "Chests");
        const auto place = [&](protocol::WirePosition on, std::int32_t face, bool sneak = false) {
            f.game->submit(1, server::intent::Sneak{sneak});
            f.hold(1, "minecraft:chest");
            f.game->submit(1, GameFixture::click(on, 1, face));
            f.game->tick();
            f.game->submit(1, server::intent::Sneak{false});
        };
        // The player looks south (yaw 0): chests face north.
        const auto chest = [](std::string_view facing, std::string_view type) {
            return std::format("minecraft:chest[facing={},type={}]", facing, type);
        };
        place({2, -61, 4}, 1);
        place({3, -61, 4}, 1);
        require(f.at({2, -60, 4}) == chest("north", "left") && f.at({3, -60, 4}) == chest("north", "right"),
                "two adjacent chests pair: " + f.at({2, -60, 4}) + " " + f.at({3, -60, 4}));
        place({4, -61, 4}, 1);
        require(f.at({4, -60, 4}) == chest("north", "single"), "a third chest stays single next to a double chest");
        // Between two single chests, the clockwise one (east, for north) is chosen: the new chest is the left half.
        place({-4, -61, 4}, 1);
        place({-2, -61, 4}, 1);
        place({-3, -61, 4}, 1);
        require(f.at({-3, -60, 4}) == chest("north", "left") && f.at({-2, -60, 4}) == chest("north", "right")
                    && f.at({-4, -60, 4}) == chest("north", "single"),
                "between two singles the clockwise one pairs: " + f.at({-4, -60, 4}) + " " + f.at({-3, -60, 4}) + " " + f.at({-2, -60, 4}));
        // Sneaking: no automatic pairing.
        place({-2, -61, -3}, 1);
        place({-3, -61, -3}, 1, true);
        require(f.at({-3, -60, -3}) == chest("north", "single") && f.at({-2, -60, -3}) == chest("north", "single"),
                "sneaking does not pair with a neighbor: " + f.at({-3, -60, -3}) + " " + f.at({-2, -60, -3}));
        // Sneaking against the side of a single chest joins it and takes its facing.
        place({1, -61, -4}, 1);
        place({1, -60, -4}, 5, true); // the chest's east face: the new chest goes to (2,-60,-4)
        require(f.at({2, -60, -4}) == chest("north", "right") && f.at({1, -60, -4}) == chest("north", "left"),
                "sneak-clicking a chest's side joins it: " + f.at({1, -60, -4}) + " " + f.at({2, -60, -4}));
        // Sneak-clicking a chest's front cannot pair: the chests would be front to back.
        place({0, -61, -5}, 1);
        place({0, -60, -5}, 2, true); // the north face: the new chest goes to (0,-60,-6)
        require(f.at({0, -60, -6}) == chest("north", "single") && f.at({0, -60, -5}) == chest("north", "single"),
                "front-to-back chests do not pair: " + f.at({0, -60, -6}));
        // Facing differs: no automatic pairing.
        f.game->submit(1, server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}); // look west: the chest faces east
        place({3, -61, -2}, 1);
        f.game->submit(1, server::intent::Move{std::nullopt, std::pair{0.0f, 0.0f}, true});
        place({4, -61, -2}, 1);
        require(f.at({3, -60, -2}) == chest("east", "single") && f.at({4, -60, -2}) == chest("north", "single"),
                "chests of different facing stay single: " + f.at({3, -60, -2}) + " " + f.at({4, -60, -2}));
    });

    suite.run("players see their whole view without widening ticking", [] {
        const auto sent_columns = [](Recorder& r, const server::Wire& wire) {
            const auto chunk = wire.ids().id(State::play, Bound::clientbound, "level_chunk_with_light");
            const auto forget = wire.ids().id(State::play, Bound::clientbound, "forget_level_chunk");
            std::set<contracts::ChunkPos> shown;
            std::lock_guard lock(r.mutex);
            for (const auto& bytes : r.packets) {
                auto p = protocol::open_packet(bytes);
                if (!p) continue;
                if (p->id == chunk) {
                    const auto x = p->fields.i32().value(), z = p->fields.i32().value();
                    shown.insert({x, z});
                } else if (p->id == forget) {
                    // Forget Level Chunk: the position as one long, z in the high half.
                    const auto packed = p->fields.i64().value();
                    shown.erase({static_cast<std::int32_t>(packed & 0xFFFFFFFF), static_cast<std::int32_t>(packed >> 32)});
                }
            }
            return shown;
        };
        const auto levels = [](const server::Game& game) {
            std::map<contracts::ChunkPos, gameplay::ChunkLevel> out;
            for (const auto& [c, l] : game.engine().loaded_columns()) out[c] = l;
            return out;
        };
        const auto square = [](contracts::ChunkPos center, std::int32_t r) {
            std::set<contracts::ChunkPos> out;
            for (std::int32_t dx = -r; dx <= r; ++dx)
                for (std::int32_t dz = -r; dz <= r; ++dz) out.insert({center.x + dx, center.z + dz});
            return out;
        };
        const auto ticking_of = [](const std::map<contracts::ChunkPos, gameplay::ChunkLevel>& l) {
            std::set<contracts::ChunkPos> out;
            for (const auto& [c, level] : l)
                if (level == gameplay::ChunkLevel::ticking) out.insert(c);
            return out;
        };
        auto config = offline_config();
        config.view_distance = 6;
        config.simulation_distance = 4;
        {
            // View above the simulation distance: 169 columns shown, 81 ticking.
            GameFixture f(config);
            auto peer = f.join(1, "Viewer");
            for (int i = 0; i < 10; ++i) f.game->tick();
            require(sent_columns(*peer, f.wire) == square({0, 0}, 6), std::format("all 169 columns of view 6 shown, not {}", sent_columns(*peer, f.wire).size()));
            const auto l = levels(*f.game);
            require(ticking_of(l) == square({0, 0}, 4), "ticking stays within the simulation distance");
            require(l.size() == 169, std::format("the view is loaded and nothing beyond: {}", l.size()));
            // Movement: three chunks east, in steps the movement rules accept.
            f.game->submit(1, server::intent::ConfirmTeleport{1});
            for (int step = 1; step <= 6; ++step)
                f.game->submit(1, server::intent::Move{server::Vec3{0.5 + 8.0 * step, -60.0, 0.5}, std::nullopt, true});
            for (int i = 0; i < 12; ++i) f.game->tick();
            require(sent_columns(*peer, f.wire) == square({3, 0}, 6), "the view follows the player");
            auto ticking = square({3, 0}, 4);
            for (const auto c : square({0, 0}, 2)) ticking.insert(c); // the spawn area
            require(ticking_of(levels(*f.game)) == ticking, "ticking follows the simulation distance");
            // A second player at spawn: the views overlap; each sees its own.
            auto other = f.join(2, "Other");
            for (int i = 0; i < 10; ++i) f.game->tick();
            require(sent_columns(*other, f.wire) == square({0, 0}, 6), "the second view is complete");
            f.game->leave(1);
            for (int i = 0; i < 4; ++i) f.game->tick();
            auto after = levels(*f.game);
            for (const auto c : square({0, 0}, 6)) require(after.contains(c), "the remaining view stays loaded");
            require(ticking_of(after) == square({0, 0}, 4), "only the remaining player's area ticks");
            require(!after.contains({9, 0}), "the departed player's view is unloaded");
        }
        {
            // View below the simulation distance: the client sees 25 columns; 81 tick.
            GameFixture f(config);
            auto peer = f.join(1, "Near", 2);
            for (int i = 0; i < 10; ++i) f.game->tick();
            require(sent_columns(*peer, f.wire) == square({0, 0}, 2), "view 2 shows 25 columns");
            const auto l = levels(*f.game);
            require(ticking_of(l) == square({0, 0}, 4) && l.size() == 121, "simulation distance 4 ticks, its border ring is loaded");
        }
    });

    suite.run("survival players use up items, dig in time and pick up drops", [] {
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Miner");
        const contracts::PlayerKey key{1, 1};
        const auto inventory = [&] {
            const auto inv = f.game->engine().view().player(key).value();
            return inv ? *inv : empty_inventory(key);
        };
        const auto item = [](std::string_view name) { return content().item_id(name).value(); };
        // Creative stocking, then survival.
        f.game->submit(1, I{server::intent::CreativeSlot{36, 2, item("minecraft:stone")}});
        f.game->submit(1, I{server::intent::CreativeSlot{37, 1, item("minecraft:wooden_pickaxe")}});
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->submit(1, I{server::intent::Move{std::nullopt, std::nullopt, true}}); // standing: off the ground digs 5 times slower
        f.game->tick();
        require(inventory().slots[36] == contracts::ItemStack{gameplay::items::stone, 2, 0}, "stocked");
        // Placing uses one stone.
        f.game->submit(1, GameFixture::click({2, -61, 1}, 1));
        f.game->tick();
        require(f.at({2, -60, 1}) == "minecraft:stone" && inventory().slots[36].count == 1, "placement used one stone");
        // By hand, stone takes 150 ticks (1 / 1.5 / 100 per tick) and drops nothing; finishing
        // early delays the break until the progress is whole.
        f.game->submit(1, I{server::intent::Dig{0, {2, -60, 1}, 1, 2}});
        f.game->tick();
        f.game->submit(1, I{server::intent::Dig{2, {2, -60, 1}, 1, 3}});
        for (int i = 0; i < 100; ++i) f.game->tick();
        require(f.at({2, -60, 1}) == "minecraft:stone", "not broken before the progress is whole");
        for (int i = 0; i < 60; ++i) f.game->tick();
        require(f.at({2, -60, 1}) == "minecraft:air", "broken once it is");
        require(f.game->engine().dump().entities.empty(), "a hand does not harvest stone");
        // With a wooden pickaxe (2 / 1.5 / 30 per tick) the client may finish at 70 %: 16 ticks.
        f.game->submit(1, GameFixture::click({3, -61, 1}, 4));
        f.game->submit(1, I{server::intent::HeldSlot{1}});
        f.game->tick();
        f.game->submit(1, I{server::intent::Dig{0, {3, -60, 1}, 1, 5}});
        f.game->tick();
        for (int i = 0; i < 14; ++i) f.game->tick();
        f.game->submit(1, I{server::intent::Dig{2, {3, -60, 1}, 1, 6}});
        f.game->tick();
        f.game->tick();
        require(f.at({3, -60, 1}) == "minecraft:air", "broken with a pickaxe after 16 ticks");
        const auto inv = inventory();
        require(inv.slots[37].item == gameplay::items::wooden_pickaxe && inv.slots[37].damage == 1, "the pickaxe wore by one");
        auto drops = f.game->engine().dump().entities;
        require(drops.size() == 1 && drops[0].stack.item == gameplay::items::cobblestone, "stone drops cobblestone");
        // The drop waits 10 ticks, then the player, whose pickup box reaches 1.3 blocks
        // sideways, takes it.
        f.game->submit(1, I{server::intent::Command{"tp 2.5 -60 1.5"}});
        for (int i = 0; i < 14; ++i) f.game->tick();
        require(f.game->engine().dump().entities.empty(), "picked up");
        bool cobblestone = false;
        for (const auto& s : inventory().slots) cobblestone |= s.item == gameplay::items::cobblestone && s.count == 1;
        require(cobblestone, "the cobblestone is in the inventory");
        const auto take = f.wire.ids().id(State::play, Bound::clientbound, "take_item_entity");
        bool animated = false;
        {
            std::lock_guard lock(peer->mutex);
            for (const auto& bytes : peer->packets)
                if (auto pk = protocol::open_packet(bytes); pk && pk->id == take) animated = true;
        }
        require(animated, "the pickup is shown");
        // Dropping one from the main hand: a thrown item waits 40 ticks.
        f.game->submit(1, I{server::intent::HeldSlot{0}});
        f.game->submit(1, I{server::intent::Dig{4, {0, 0, 0}, 0, 0}});
        f.game->tick();
        require(inventory().slots[36].empty() && f.game->engine().dump().entities.size() == 1, "the last stone was thrown");
        require(f.game->engine().dump().entities[0].pickup_delay > 30, "with a long pickup delay");
    });

    suite.run("dropped and picked-up items are conserved", [] {
        using I = server::Intent;
        GameFixture f;
        f.join(1, "Keeper");
        const auto item = [](std::string_view name) { return content().item_id(name).value(); };
        // Conservation: random drops and pickups between two players keep the item count.
        f.join(2, "Other");
        f.game->submit(1, I{server::intent::CreativeSlot{36, 64, item("minecraft:redstone")}});
        f.game->submit(2, I{server::intent::CreativeSlot{36, 32, item("minecraft:redstone")}});
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->submit(2, I{server::intent::Command{"gamemode survival"}});
        f.game->tick();
        const auto total = [&] {
            std::size_t n = 0;
            const auto d = f.game->engine().dump();
            for (const auto& p : d.players)
                for (const auto& s : p.slots)
                    if (s.item == gameplay::items::redstone) n += s.count;
            for (const auto& e : d.entities)
                if (e.stack.item == gameplay::items::redstone) n += e.stack.count;
            return n;
        };
        require(total() == 96, "96 redstone to start");
        std::mt19937 rng(11);
        for (int step = 0; step < 300; ++step) {
            const auto who = std::uniform_int_distribution<int>(1, 2)(rng);
            const auto what = std::uniform_int_distribution<int>(0, 5)(rng);
            if (what == 0) f.game->submit(static_cast<std::uint64_t>(who), I{server::intent::Dig{4, {0, 0, 0}, 0, 0}});
            if (what == 1) f.game->submit(static_cast<std::uint64_t>(who), I{server::intent::Dig{3, {0, 0, 0}, 0, 0}});
            if (what == 2) f.game->submit(static_cast<std::uint64_t>(who), I{server::intent::HeldSlot{static_cast<std::int16_t>(std::uniform_int_distribution<int>(0, 8)(rng))}});
            f.game->tick();
            require(total() == 96, std::format("conserved at step {}", step));
        }
    });

    suite.run("players open, click and close container windows", [] {
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Clerk");
        const contracts::PlayerKey key{1, 1};
        const auto inventory = [&] {
            const auto inv = f.game->engine().view().player(key).value();
            return inv ? *inv : empty_inventory(key);
        };
        const auto chest_slots = [&] {
            for (const auto& [pos, be] : f.game->engine().dump().block_entities)
                if (pos == contracts::BlockPos{2, -60, 1}) return be.slots;
            return std::vector<contracts::ItemStack>{};
        };
        // Window packets since the last call: (packet name, window id, menu
        // type, slot count or slot index), and the last state id sent.
        std::size_t seen = 0;
        std::int32_t state = 0;
        const auto window_packets = [&] {
            const auto open = f.wire.ids().id(State::play, Bound::clientbound, "open_screen");
            const auto content_id = f.wire.ids().id(State::play, Bound::clientbound, "container_set_content");
            const auto slot_id = f.wire.ids().id(State::play, Bound::clientbound, "container_set_slot");
            const auto cursor_id = f.wire.ids().id(State::play, Bound::clientbound, "set_cursor_item");
            const auto close = f.wire.ids().id(State::play, Bound::clientbound, "container_close");
            std::vector<std::tuple<std::string, std::int32_t, std::int32_t>> out;
            std::lock_guard lock(peer->mutex);
            for (; seen < peer->packets.size(); ++seen) {
                auto pk = protocol::open_packet(peer->packets[seen]);
                if (!pk) continue;
                if (pk->id == open) {
                    const auto window = pk->fields.varint().value();
                    out.emplace_back("open", window, pk->fields.varint().value());
                } else if (pk->id == content_id) {
                    const auto window = pk->fields.varint().value();
                    state = pk->fields.varint().value();
                    out.emplace_back("content", window, pk->fields.varint().value());
                } else if (pk->id == slot_id) {
                    const auto window = pk->fields.varint().value();
                    state = pk->fields.varint().value();
                    out.emplace_back("slot", window, pk->fields.i16().value());
                } else if (pk->id == cursor_id) {
                    out.emplace_back("cursor", 0, 0);
                } else if (pk->id == close) {
                    out.emplace_back("close", pk->fields.varint().value(), 0);
                }
            }
            return out;
        };
        const auto has = [](const auto& packets, std::string_view name, std::int32_t window, std::int32_t value) {
            return std::ranges::any_of(packets, [&](const auto& t) {
                return std::get<0>(t) == name && std::get<1>(t) == window && std::get<2>(t) == value;
            });
        };
        f.hold(1, "minecraft:chest");
        f.game->submit(1, GameFixture::click({2, -61, 1}, 1));
        f.game->submit(1, I{server::intent::CreativeSlot{37, 64, content().item_id("minecraft:redstone").value()}});
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->tick();
        require(f.at({2, -60, 1}).starts_with("minecraft:chest"), "chest placed");
        (void)window_packets();
        // Using the chest opens a 9x3 window: 27 chest slots, then the 36 inventory slots.
        f.game->submit(1, GameFixture::click({2, -60, 1}, 2));
        f.game->tick();
        auto packets = window_packets();
        require(has(packets, "open", 1, 2) && has(packets, "content", 1, 63), "a generic_9x3 window opened");
        // Shift-clicking the redstone in hotbar slot 1 (window slot 27 + 27 + 1) moves it into
        // the chest; the client is sent the two changed slots only.
        f.game->submit(1, window_click(1, 55, 0, 1, state));
        f.game->tick();
        require(chest_slots().size() == 27 && chest_slots()[0] == contracts::ItemStack{gameplay::items::redstone, 64, 0}, "moved into the chest");
        require(inventory().slots[37].empty(), "and out of the hotbar");
        packets = window_packets();
        require(packets.size() == 2 && has(packets, "slot", 1, 0) && has(packets, "slot", 1, 55), "only the changed slots are sent");
        // A click on a stale state id is applied, and the whole window is sent again.
        f.game->submit(1, window_click(1, 0, 0, 0, state - 1));
        f.game->tick();
        packets = window_packets();
        require(inventory().cursor.count == 64 && has(packets, "content", 1, 63), "a stale click resends the window");
        f.game->submit(1, window_click(1, 0, 0, 0, state));
        f.game->tick();
        packets = window_packets();
        // The client predicted the empty carried stack: only the slot is sent.
        require(chest_slots()[0].count == 64 && packets.size() == 1 && has(packets, "slot", 1, 0), "put back");
        // Clicks naming another window are ignored.
        f.game->submit(1, window_click(0, 0, 0, 0));
        f.game->tick();
        require(inventory().cursor.empty() && chest_slots()[0].count == 64, "a stale window id does nothing");
        // Picking up half with the right button, then closing returns the cursor to the inventory.
        f.game->submit(1, window_click(1, 0, 1, 0));
        f.game->tick();
        require(inventory().cursor == contracts::ItemStack{gameplay::items::redstone, 32, 0} && chest_slots()[0].count == 32, "half picked up");
        f.game->submit(1, I{server::intent::ContainerClose{1}});
        f.game->tick();
        require(inventory().cursor.empty(), "closing returns the carried stack");
        bool returned = false;
        for (const auto& st : inventory().slots) returned |= st == contracts::ItemStack{gameplay::items::redstone, 32, 0};
        require(returned, "into the inventory");
        // A window whose container goes out of reach closes, and the client is told.
        f.game->submit(1, GameFixture::click({2, -60, 1}, 3));
        f.game->tick();
        packets = window_packets();
        require(has(packets, "open", 2, 2), "the next window has the next id");
        f.game->submit(1, I{server::intent::Command{"tp 20.5 -60 20.5"}});
        f.game->tick();
        f.game->tick();
        require(has(window_packets(), "close", 2, 0), "closed out of reach");
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

    suite.run("a client that stops reading is closed while a write is outstanding", [] {
        // Uncompressed chunks fill a small receive window quickly. The session
        // is closed while the server's write waits on the socket: the write's
        // buffer must outlive the close, and the session must go away.
        const auto wait_gone = [](server::Server& s, std::chrono::seconds limit) {
            const auto deadline = std::chrono::steady_clock::now() + limit;
            while (s.players() != 0 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return s.players() == 0;
        };
        const auto still_serves = [](server::Server& s) {
            TestClient ping(s.port());
            ping.send(State::handshaking, "intention", Writer().varint(772).string("localhost").u16(25565).varint(1));
            ping.send(State::status, "status_request");
            return ping.until(State::status, "status_response").has_value();
        };
        auto config = offline_config();
        config.compression_threshold = -1;
        config.view_distance = 8;
        config.simulation_distance = 8; // about 14 MiB of chunks: more than the socket buffers hold
        {
            // The queue limit closes it.
            config.max_queued_bytes = 256 * 1024;
            server::Server s(config);
            s.start();
            TestClient c(s.port(), 772, 4096);
            c.join("Stalled");
            require(wait_gone(s, std::chrono::seconds(20)), "a client over the queue limit is closed");
            require(still_serves(s), "the server keeps serving");
            s.stop();
        }
        {
            // A kick waits for its frame; a peer that never reads is closed after the deadline.
            config.max_queued_bytes = 64 * 1024 * 1024;
            server::Server s(config);
            s.start();
            TestClient c(s.port(), 772, 4096);
            c.join("Stalled");
            for (int i = 0; i < 200 && s.players() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            require(s.players() == 1, "joined");
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // chunks fill the window
            try {
                for (int i = 0; i < 1500; ++i) c.send(State::play, "client_tick_end");
            } catch (const std::exception&) {
            }
            require(wait_gone(s, std::chrono::seconds(20)), "a closing client that does not read is closed");
            require(still_serves(s), "the server keeps serving");
            s.stop();
        }
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
            // The inventory is simulation state: the stone the player held is recovered with the world.
            const auto players = recovered_state(config.world_directory).players;
            require(players.size() == 1 && players[0].slots[36].item == gameplay::items::stone, "inventory on disk");
        }
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
        require(block_at(state, {2, -60, 1}) == gameplay::blocks::lamp(true).raw(),
                std::format("lit lamp recovered: lamp {:x} lever {:x}", block_at(state, {2, -60, 1}), block_at(state, {1, -60, 1})));
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

    suite.run("a world has one owning server", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        {
            server::Server s(config);
            s.start();
            s.stop();
        }
        std::ofstream(dir / "server.conf") << std::format(
            "bind-address=127.0.0.1\nport=0\nonline-mode=false\nworkers=2\nview-distance=3\nsimulation-distance=2\n"
            "world-directory={}\n", config.world_directory.string());
        const std::vector<std::string> serve{"--serve", "--config", (dir / "server.conf").string()};
        {
            // An owner that only holds the world: nothing else changes the files.
            const auto& bundle = **contracts::BundleRegistry::select_for_simulation("java-1.21.8");
            storage::PosixFilesystem fs;
            storage::WorldDirectory holder(fs, config.world_directory, "java-1.21.8", std::string(bundle.rules->id));
            require(holder.recover().has_value(), "the test owns the world");
            const auto before = world_files(config.world_directory);
            const auto [code, output] = run_server_binary(serve);
            require(code == 1 && output.find("is in use") != std::string::npos, "a second process is refused: " + output);
            bool threw = false;
            try {
                server::Server second(config);
            } catch (const std::runtime_error& e) {
                threw = std::string(e.what()).find("is in use") != std::string::npos;
            }
            require(threw, "a second server in this process is refused");
            require(world_files(config.world_directory) == before, "the refused servers changed no file");
            const auto exported = run_server_binary({"--export-anvil", (dir / "export").string(), "--world", config.world_directory.string()});
            require(exported.first == 1 && exported.second.find("is in use") != std::string::npos, "export refuses an owned world");
            const auto migrated = run_server_binary(
                {"--migrate-world", (dir / "copy").string(), "--world", config.world_directory.string(), "--game-version", "java-1.21.4"});
            require(migrated.first == 1 && migrated.second.find("is in use") != std::string::npos, "migration refuses an owned world");
            require(world_files(config.world_directory) == before, "the refused tools changed no file");
        }
        {
            // A running server owns its world until it stops.
            server::Server s(config);
            s.start();
            const auto [code, output] = run_server_binary(serve);
            require(code == 1 && output.find("is in use") != std::string::npos, "refused while a server runs: " + output);
            require(s.running(), "the owner keeps running");
            TestClient c(s.port());
            c.join("Owner");
            confirm_spawn(c);
            use_item_on(c, "minecraft:stone", {0, -61, 2}, 1);
            require(block_update(c, {0, -60, 2}) == content().state_id("minecraft:stone"), "the owner's world still changes");
            s.stop();
        }
        require(block_at(recovered_state(config.world_directory), {0, -60, 2}) == gameplay::blocks::stone().raw(), "the owner's history is intact");
        {
            // A server that cannot bind its port does not open its world.
            server::Server s(offline_config());
            s.start();
            auto other = offline_config();
            other.port = s.port();
            other.world_directory = dir / "never";
            bool threw = false;
            try {
                server::Server second(other);
            } catch (const std::exception&) {
                threw = true;
            }
            require(threw && !std::filesystem::exists(other.world_directory), "the port conflict fails before the world is created");
            s.stop();
        }
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
