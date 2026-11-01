// SPDX-License-Identifier: AGPL-3.0-only
// In-process server tests: a real Server on an ephemeral port, driven by a
// small client built on Circuit's codec. They check protocol state handling,
// limits and the translation of player actions into simulation commands.
#include "../support/check.hpp"
#include "test_client.hpp"
#include "../../src/server/command_reader.hpp"
#include "../../src/server/game.hpp"
#include "../../src/server/server_data.hpp"
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/loot.hpp>
#include <circuit/gameplay/physics.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <circuit/gameplay/survival.hpp>
#include "../../src/server/world_meta.hpp"
#include <circuit/auth.hpp>
#include <circuit/auth/crypto.hpp>
#include <circuit/protocol/content.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/runtime/engine.hpp>
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/evidence/trace_format.hpp>
#include <circuit/server/anvil.hpp>
#include <circuit/server/server.hpp>
#include <circuit/storage/world_directory.hpp>
#include <cmath>
#include <numbers>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <spawn.h>
#include <sstream>
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
        game = std::make_unique<server::Game>(config, bundle, wire, content(), mapping, data);
    }
    // On another target version ("1.21.4").
    GameFixture(std::string_view v, ServerConfig c) : version(v), config(std::move(c)) {
        game = std::make_unique<server::Game>(config, bundle, wire, *protocol::ContentTables::for_protocol(wire.protocol()), mapping, data);
    }
    // With the data a server loaded from data packs.
    GameFixture(ServerConfig c, server::ServerData d) : data(std::move(d)), wire(772, data.components), config(std::move(c)) {
        game = std::make_unique<server::Game>(config, bundle, wire, content(), mapping, data);
    }
    ~GameFixture() { game->shutdown("test finished"); }
    std::shared_ptr<Recorder> join(std::uint64_t id, std::string name, std::int32_t view = 0) {
        auto peer = std::make_shared<Recorder>(id);
        game->join({peer, {id, id}, std::move(name), {}, view == 0 ? config.view_distance : view, false});
        game->tick();
        return peer;
    }
    std::string at(contracts::BlockPos p) const {
        const auto s = game->engine().view().block(contracts::DimensionId::overworld, p);
        return s ? mapping.notation(*s) : "unloaded";
    }
    void hold(std::uint64_t peer, std::string_view item) {
        game->submit(peer, server::intent::CreativeSlot{36, 1, content().item_id(item).value()});
    }
    // Clicks the top face of `pos`.
    static server::Intent click(protocol::WirePosition pos, std::int32_t seq, std::int32_t face = 1) {
        return server::intent::UseOn{0, pos, face, 0.5f, 1.0f, 0.5f, false, seq};
    }

    std::string version{"1.21.8"};
    const contracts::VersionBundle& bundle = **contracts::BundleRegistry::select_for_simulation(std::format("java-{}", version));
    const evidence::ContentMapping& mapping = *evidence::ContentMapping::for_game_version(version);
    server::ServerData data = server::load_server_data(bundle, {}).value();
    server::Wire wire{version == "1.21.4" ? 769 : 772, data.components};
    ServerConfig config;
    std::unique_ptr<server::Game> game;
};

// The system chat messages a player got, as tags.
std::vector<protocol::nbt::Tag> chat_messages(Recorder& r, const server::Wire& wire) {
    const auto chat = wire.ids().id(State::play, Bound::clientbound, "system_chat");
    std::vector<protocol::nbt::Tag> out;
    std::lock_guard lock(r.mutex);
    for (const auto& bytes : r.packets) {
        auto p = protocol::open_packet(bytes);
        if (!p || p->id != chat) continue;
        if (auto tag = protocol::nbt::read_network(p->fields)) out.push_back(std::move(*tag));
    }
    return out;
}

// The translated feedback a player got: the key and its arguments (strings,
// numbers or text components). A command error is the first of an empty
// text's parts.
std::vector<std::string> feedback(Recorder& r, const server::Wire& wire) {
    std::vector<std::string> out;
    for (const auto& message : chat_messages(r, wire)) {
        const auto* tag = &message;
        if (!tag->find("translate")) {
            const auto* extra = tag->find("extra");
            if (!extra || extra->items().empty() || !extra->items()[0].find("translate")) continue;
            tag = &extra->items()[0];
        }
        std::string text = tag->find("translate")->text();
        if (const auto* with = tag->find("with"))
            for (const auto& a : with->items())
                text += " " + (a.type() == protocol::nbt::TagType::string ? a.text()
                               : a.type() == protocol::nbt::TagType::int_ ? std::to_string(a.integer())
                               : a.find("text")                           ? a.find("text")->text()
                                                                         : a.find("translate")->text());
        out.push_back(std::move(text));
    }
    return out;
}

// The context lines of command errors a player got: the command clicking
// suggests, then the text with the underlined part in brackets.
std::vector<std::string> error_contexts(Recorder& r, const server::Wire& wire) {
    std::vector<std::string> out;
    for (const auto& message : chat_messages(r, wire)) {
        const auto* extra = message.find("extra");
        if (!extra || extra->items().empty()) continue;
        const auto& context = extra->items()[0];
        const auto* click = context.find(wire.protocol() >= 770 ? "click_event" : "clickEvent");
        if (!click) continue;
        std::string text = click->find(wire.protocol() >= 770 ? "command" : "value")->text() + " :: ";
        for (const auto& part : context.find("extra")->items()) {
            if (const auto* key = part.find("translate")) text += "<" + key->text() + ">";
            else if (part.find("underlined")) text += "[" + part.find("text")->text() + "]";
            else text += part.find("text")->text();
        }
        out.push_back(std::move(text));
    }
    return out;
}

// The weather game events a player got (1 rain starts, 2 rain ends, 7 rain
// level, 8 thunder level) with their values.
std::vector<std::pair<int, float>> weather_events(Recorder& r, const server::Wire& wire) {
    const auto id = wire.ids().id(State::play, Bound::clientbound, "game_event");
    std::vector<std::pair<int, float>> out;
    std::lock_guard lock(r.mutex);
    for (const auto& bytes : r.packets) {
        auto p = protocol::open_packet(bytes);
        if (!p || p->id != id) continue;
        protocol::Reader in(p->fields);
        const auto event = in.u8().value();
        const auto value = in.f32().value();
        if (event == 1 || event == 2 || event == 7 || event == 8) out.emplace_back(event, value);
    }
    return out;
}

// The set_time packets a player got: world age, day time, advancing.
std::vector<std::tuple<std::int64_t, std::int64_t, bool>> time_packets(Recorder& r, const server::Wire& wire) {
    const auto id = wire.ids().id(State::play, Bound::clientbound, "set_time");
    std::vector<std::tuple<std::int64_t, std::int64_t, bool>> out;
    std::lock_guard lock(r.mutex);
    for (const auto& bytes : r.packets) {
        auto p = protocol::open_packet(bytes);
        if (!p || p->id != id) continue;
        protocol::Reader in(p->fields);
        const auto age = in.i64().value();
        const auto day = in.i64().value();
        out.emplace_back(age, day, in.boolean().value());
    }
    return out;
}

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
    for (const auto& [pos, raw] : d.overworld().blocks)
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
        require(registries.size() == 17 && registries[0] == "minecraft:dimension_type", "seventeen registries sent");
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

    suite.run("players place building blocks by the clicked face and half, with their neighbors' shapes", [] {
        // As both official servers do (circuit-clienttest/place-families.js).
        using I = server::Intent;
        const auto use = [](protocol::WirePosition pos, std::int32_t face, float y, std::int32_t seq) {
            return I{server::intent::UseOn{0, pos, face, 0.5f, y, 0.5f, false, seq}};
        };
        // Away from the player, who stands at the spawn (0, -60, 0).
        GameFixture f;
        f.join(1, "Builder");
        const auto place = [&](std::string_view item, I click) {
            f.hold(1, item);
            f.game->submit(1, std::move(click));
            f.game->tick();
        };
        place("minecraft:oak_log", use({0, -61, 3}, 1, 1.0f, 1));
        place("minecraft:stone", use({2, -61, 3}, 1, 1.0f, 2));
        place("minecraft:oak_log", use({2, -60, 3}, 2, 0.5f, 3));        // the stone's north face
        place("minecraft:oak_stairs", use({2, -60, 3}, 5, 0.75f, 4));    // its east face, upper half
        place("minecraft:oak_stairs", use({2, -60, 3}, 3, 0.25f, 5));    // its south face, lower half
        place("minecraft:cobblestone_wall", use({1, -61, 3}, 1, 1.0f, 6)); // between the log and the stone
        place("minecraft:stone_slab", use({-2, -61, 3}, 1, 1.0f, 7));
        place("minecraft:stone_slab", use({-2, -60, 3}, 1, 0.5f, 8));     // the slab's top: doubled
        place("minecraft:stone_slab", use({0, -61, -3}, 1, 1.0f, 9));
        place("minecraft:stone_slab", use({0, -60, -3}, 2, 0.25f, 10));   // its north face, lower half: beside it
        for (int i = 0; i < 4; ++i) f.game->tick();
        require(f.at({0, -60, 3}) == "minecraft:oak_log[axis=y]" && f.at({2, -60, 2}) == "minecraft:oak_log[axis=z]",
                "logs along the face: " + f.at({0, -60, 3}) + " " + f.at({2, -60, 2}) + " " + f.at({2, -60, 3}));
        require(f.at({3, -60, 3}).contains("half=top") && f.at({2, -60, 4}).contains("half=bottom"), f.at({3, -60, 3}) + " " + f.at({2, -60, 4}));
        require(f.at({1, -60, 3}) == "minecraft:cobblestone_wall[east=low,north=none,south=none,up=false,waterlogged=false,west=low]",
                "the wall joins the log and the stone: " + f.at({1, -60, 3}));
        require(f.at({-2, -60, 3}) == "minecraft:stone_slab[type=double,waterlogged=false]", "doubled: " + f.at({-2, -60, 3}));
        require(f.at({0, -60, -3}) == "minecraft:stone_slab[type=bottom,waterlogged=false]"
                    && f.at({0, -60, -4}) == "minecraft:stone_slab[type=bottom,waterlogged=false]",
                "a lower-half side click places beside: " + f.at({0, -60, -4}));
    });

    suite.run("players pour and fill buckets along the view of the click before the use", [] {
        // As a real client sends a bucket's use: a click on the block its view
        // meets, then the use of the item with its view, in the same tick
        // (circuit-clienttest/buckets.js). The player stands at the spawn
        // (0, -60, 0) and looks at the floor's top at (2.5, -60, 2.5).
        GameFixture f;
        f.join(1, "Builder");
        const double dx = 2.5, dy = -1.62, dz = 2.5;
        const auto yaw = static_cast<float>(std::atan2(-dx, dz) * 180.0 / std::numbers::pi);
        const auto pitch = static_cast<float>(-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0 / std::numbers::pi);
        const auto use = [&](std::string_view item, bool click, std::int32_t seq) {
            f.hold(1, item);
            f.game->tick(); // the server looks at the held item as the last tick left it
            if (click) f.game->submit(1, server::intent::UseOn{0, {2, -61, 2}, 1, 0.5f, 1.0f, 0.5f, false, seq});
            f.game->submit(1, server::intent::Use{0, seq + 1, yaw, pitch});
            f.game->tick();
        };
        use("minecraft:water_bucket", true, 1);
        require(f.at({2, -60, 2}) == "minecraft:water[level=0]", "the water bucket pours on the clicked floor: " + f.at({2, -60, 2}));
        use("minecraft:bucket", true, 3);
        require(f.at({2, -60, 2}) == "minecraft:air", "the bucket takes the source the view enters: " + f.at({2, -60, 2}));
        use("minecraft:lava_bucket", false, 5);
        require(f.at({2, -60, 2}) == "minecraft:air", "without a click the view met no block: " + f.at({2, -60, 2}));
        use("minecraft:lava_bucket", true, 7);
        require(f.at({2, -60, 2}) == "minecraft:lava[level=0]", "the lava bucket pours: " + f.at({2, -60, 2}));
    });

    suite.run("players turn signs and heads by their yaw and place beds with both halves", [] {
        // As both official servers do (circuit-clienttest/place-families.js).
        GameFixture f;
        f.join(1, "Builder");
        const auto place = [&](std::string_view item, float yaw, protocol::WirePosition on, std::int32_t face) {
            f.game->submit(1, server::intent::Move{std::nullopt, std::pair{yaw, 0.0f}, true});
            f.hold(1, item);
            f.game->submit(1, GameFixture::click(on, 1, face));
            f.game->tick();
        };
        place("minecraft:oak_sign", 30.0f, {2, -61, 2}, 1);
        place("minecraft:skeleton_skull", 30.0f, {3, -61, 2}, 1);
        place("minecraft:red_bed", -90.0f, {-3, -61, 2}, 1);
        place("minecraft:stone", 0.0f, {-2, -61, -2}, 1);
        place("minecraft:blue_bed", -90.0f, {-3, -61, -2}, 1);
        for (int i = 0; i < 4; ++i) f.game->tick();
        require(f.at({2, -60, 2}) == "minecraft:oak_sign[rotation=9,waterlogged=false]", "the sign faces the player: " + f.at({2, -60, 2}));
        require(f.at({3, -60, 2}) == "minecraft:skeleton_skull[powered=false,rotation=1]", "the skull faces away: " + f.at({3, -60, 2}));
        require(f.at({-3, -60, 2}) == "minecraft:red_bed[facing=east,occupied=false,part=foot]"
                    && f.at({-2, -60, 2}) == "minecraft:red_bed[facing=east,occupied=false,part=head]",
                "the bed's head goes ahead of its foot: " + f.at({-3, -60, 2}) + " " + f.at({-2, -60, 2}));
        require(f.at({-3, -60, -2}) == "minecraft:air", "a bed without room for its head is not placed: " + f.at({-3, -60, -2}));
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
            return std::format("minecraft:chest[facing={},type={},waterlogged=false]", facing, type);
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
        // Loaded chunk -> {load level, simulation level}.
        const auto levels = [](const server::Game& game) {
            std::map<contracts::ChunkPos, std::pair<int, int>> out;
            const auto dump = game.engine().dump();
            for (const auto& c : dump.overworld().chunks)
                if (c.level != gameplay::ChunkLevel::unloaded) out[c.pos] = {c.load, c.simulation};
            return out;
        };
        const auto square = [](contracts::ChunkPos center, std::int32_t r) {
            std::set<contracts::ChunkPos> out;
            for (std::int32_t dx = -r; dx <= r; ++dx)
                for (std::int32_t dz = -r; dz <= r; ++dz) out.insert({center.x + dx, center.z + dz});
            return out;
        };
        // The chunks whose simulation level is `level` or lower.
        const auto simulated = [](const std::map<contracts::ChunkPos, std::pair<int, int>>& l, int level) {
            std::set<contracts::ChunkPos> out;
            for (const auto& [c, v] : l)
                if (v.second <= level) out.insert(c);
            return out;
        };
        const auto keys = [](const std::map<contracts::ChunkPos, std::pair<int, int>>& l) {
            std::set<contracts::ChunkPos> out;
            for (const auto& [c, v] : l) out.insert(c);
            return out;
        };
        auto config = offline_config();
        config.view_distance = 6;
        config.simulation_distance = 4;
        {
            // Without players, the start ticket (level 32 - spawnChunkRadius 2
            // at the spawn chunk) loads and simulates the spawn chunks.
            GameFixture f(config);
            for (int i = 0; i < 3; ++i) f.game->tick();
            const auto l = levels(*f.game);
            require(keys(l) == square({0, 0}, 3) && simulated(l, 31) == square({0, 0}, 1) && simulated(l, 32) == square({0, 0}, 2)
                        && l.at({0, 0}) == std::pair{30, 30} && l.at({3, 3}) == std::pair{33, 33},
                    "the spawn chunks");
        }
        {
            // View above the simulation distance: 169 columns shown, loaded
            // two chunks further (levels 32 and 33); entities tick in 81,
            // blocks in 121 (simulation level 31 - 4 at the player's chunk).
            GameFixture f(config);
            auto peer = f.join(1, "Viewer");
            for (int i = 0; i < 10; ++i) f.game->tick();
            require(sent_columns(*peer, f.wire) == square({0, 0}, 6), std::format("all 169 columns of view 6 shown, not {}", sent_columns(*peer, f.wire).size()));
            const auto l = levels(*f.game);
            require(simulated(l, 31) == square({0, 0}, 4) && simulated(l, 32) == square({0, 0}, 5),
                    "simulation stays within the simulation distance");
            require(keys(l) == square({0, 0}, 8), std::format("the view and two rings are loaded, nothing beyond: {}", l.size()));
            require(l.at({6, 0}) == std::pair{31, 33} && l.at({7, 0}) == std::pair{32, 34} && l.at({8, 8}) == std::pair{33, 34},
                    "the loading ticket loads without simulating");
            // Movement: three chunks east, in steps the movement rules accept.
            f.game->submit(1, server::intent::ConfirmTeleport{1});
            for (int step = 1; step <= 6; ++step)
                f.game->submit(1, server::intent::Move{server::Vec3{0.5 + 8.0 * step, -60.0, 0.5}, std::nullopt, true});
            for (int i = 0; i < 12; ++i) f.game->tick();
            // Columns one chunk beyond the view stay shown while they are loaded.
            const auto sent = sent_columns(*peer, f.wire);
            require(std::ranges::includes(sent, square({3, 0}, 6)) && std::ranges::includes(square({3, 0}, 7), sent),
                    "the view follows the player");
            const auto moved = levels(*f.game);
            require(simulated(moved, 31) == square({3, 0}, 4) && simulated(moved, 32) == square({3, 0}, 5),
                    "simulation follows the simulation distance");
            // A second player at spawn: the views overlap; each sees its own.
            auto other = f.join(2, "Other");
            for (int i = 0; i < 10; ++i) f.game->tick();
            require(sent_columns(*other, f.wire) == square({0, 0}, 6), "the second view is complete");
            f.game->leave(1);
            for (int i = 0; i < 4; ++i) f.game->tick();
            const auto after = levels(*f.game);
            require(keys(after) == square({0, 0}, 8), "the remaining view stays loaded, the departed player's unloads");
            require(simulated(after, 31) == square({0, 0}, 4), "only the remaining player's area ticks");
        }
        {
            // View below the simulation distance: the client sees 25 columns.
            // The simulation ticket simulates only what the view loads.
            GameFixture f(config);
            auto peer = f.join(1, "Near", 2);
            for (int i = 0; i < 10; ++i) f.game->tick();
            require(sent_columns(*peer, f.wire) == square({0, 0}, 2), "view 2 shows 25 columns");
            const auto l = levels(*f.game);
            require(keys(l) == square({0, 0}, 4) && simulated(l, 31) == square({0, 0}, 2) && simulated(l, 32) == square({0, 0}, 3),
                    "simulation distance 4 runs where view 2 loads");
        }
    });

    suite.run("players keep the overworld awake", [] {
        GameFixture f;
        for (int i = 0; i < 305; ++i) f.game->tick();
        // Without players or forced chunks the overworld idles 300 ticks
        // after the server started (tick 1).
        auto d = f.game->engine().dump();
        require(d.started == 0 && d.overworld().last_active == 0 && d.overworld().presences.empty(), "idle without players");
        auto peer = f.join(1, "Present");
        f.game->tick();
        d = f.game->engine().dump();
        require(d.overworld().presences.size() == 1 && !d.overworld().presences[0].spectator && d.overworld().last_active == d.time,
                "a player makes it active");
        // The rules follow the position (experience orbs are drawn to it).
        f.game->submit(1, server::intent::ConfirmTeleport{1});
        f.game->submit(1, server::intent::Move{server::Vec3{4.5, -60.0, 0.5}, std::nullopt, true});
        f.game->tick();
        f.game->tick();
        require(f.game->engine().dump().overworld().presences[0].position == contracts::Vec3{4.5, -60.0, 0.5}, "the new position");
        f.game->leave(1);
        f.game->tick();
        d = f.game->engine().dump();
        require(d.overworld().presences.empty() && d.overworld().last_active == d.time - 1, "leaving ends it");
    });

    suite.run("forceload marks chunks that stay loaded and persist", [] {
        const auto forced = [](const server::Game& game) {
            std::set<contracts::ChunkPos> out;
            for (const auto& t : game.engine().dump().tickets)
                if (t.type == contracts::TicketType::forced) out.insert(t.center);
            return out;
        };
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        {
            GameFixture f(config);
            auto peer = f.join(1, "Loader");
            using I = server::Intent;
            for (const auto* command : {"forceload add 320 0", "forceload add 320 0", "forceload add -20 -20 20 20",
                                        "forceload query 330 5", "forceload query 400 0", "forceload remove 0 0",
                                        "forceload add 0 0 300 300", "forceload query"})
                f.game->submit(1, I{server::intent::Command{command}});
            f.game->tick();
            f.game->tick();
            const std::vector<std::string> expected{
                "commands.forceload.added.single [20, 0] minecraft:overworld",
                "commands.forceload.added.failure",
                "commands.forceload.added.multiple 16 minecraft:overworld [-2, -2] [1, 1]",
                "commands.forceload.query.success [20, 0] minecraft:overworld",
                "commands.forceload.query.failure [25, 0] minecraft:overworld",
                "commands.forceload.removed.single [0, 0] minecraft:overworld",
                "commands.forceload.toobig 256 361",
                "commands.forceload.list.multiple 16 minecraft:overworld [-2, -2], [-2, -1], [-2, 0], [-2, 1], [-1, -2], [-1, -1], "
                "[-1, 0], [-1, 1], [0, -2], [0, -1], [0, 1], [1, -2], [1, -1], [1, 0], [1, 1], [20, 0]"};
            const auto got = feedback(*peer, f.wire);
            require(got.size() == expected.size(), std::format("{} messages", got.size()));
            for (std::size_t i = 0; i < expected.size(); ++i) require(got[i] == expected[i], got[i]);
            require(forced(*f.game).size() == 16 && forced(*f.game).contains({20, 0}), "the forced tickets");
            const auto d = f.game->engine().dump();
            const auto at = [&](contracts::ChunkPos c) {
                for (const auto& col : d.overworld().chunks)
                    if (col.pos == c) return std::pair<int, int>{col.load, col.simulation};
                return std::pair<int, int>{34, 34};
            };
            require(at({20, 0}) == std::pair{31, 31} && at({21, 0}) == std::pair{32, 32} && at({22, 0}) == std::pair{33, 33},
                    "a forced chunk and its rings");
        }
        {
            // After a restart the forced chunks are still loaded, and can be removed.
            GameFixture f(config);
            f.game->tick();
            require(forced(*f.game).size() == 16, "forced chunks persist");
            auto peer = f.join(1, "Loader");
            f.game->submit(1, server::Intent{server::intent::Command{"forceload remove all"}});
            f.game->tick();
            f.game->tick();
            require(forced(*f.game).empty() && feedback(*peer, f.wire).back() == "commands.forceload.removed.all minecraft:overworld",
                    "remove all");
        }
        std::filesystem::remove_all(dir);
    });

    suite.run("game rules are set with /gamerule, move the spawn chunks and persist", [] {
        const auto start = [](const server::Game& game) {
            std::optional<std::uint8_t> level;
            for (const auto& t : game.engine().dump().tickets)
                if (t.type == contracts::TicketType::start) {
                    require(!level, "one start ticket");
                    level = t.level;
                }
            return level;
        };
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        {
            GameFixture f(config);
            auto peer = f.join(1, "Ruler");
            require(start(*f.game) == std::optional<std::uint8_t>{30}, "spawnChunkRadius 2: a start ticket of level 30");
            using I = server::Intent;
            // What both official servers answered to the same commands.
            for (const auto* command : {"gamerule randomTickSpeed", "gamerule randomTickSpeed 7", "gamerule randomTickSpeed x",
                                        "gamerule randomTickSpeed 1.5", "gamerule randomTickSpeed 5x", "gamerule keepInventory maybe",
                                        "gamerule keepInventory 'true'", "gamerule keepInventory", "gamerule doDaylightCycle \"tru",
                                        "gamerule spawnChunkRadius 33", "gamerule spawnChunkRadius -1", "gamerule noSuchRule",
                                        "gamerule minecartMaxSpeed 4", "gamerule spawnChunkRadius 3 4", "gamerule",
                                        "gamerule spawnChunkRadius 0"})
                f.game->submit(1, I{server::intent::Command{command}});
            f.game->tick();
            f.game->tick();
            const std::vector<std::string> expected{"commands.gamerule.query randomTickSpeed 3",
                                                    "commands.gamerule.set randomTickSpeed 7",
                                                    "parsing.int.expected",
                                                    "parsing.int.invalid 1.5",
                                                    "command.expected.separator",
                                                    "parsing.bool.invalid maybe",
                                                    "commands.gamerule.set keepInventory true",
                                                    "commands.gamerule.query keepInventory true",
                                                    "parsing.quote.expected.end",
                                                    "argument.integer.big 32 33",
                                                    "argument.integer.low 0 -1",
                                                    "command.unknown.argument",
                                                    "command.unknown.argument",
                                                    "command.unknown.argument",
                                                    "command.unknown.command",
                                                    "commands.gamerule.set spawnChunkRadius 0"};
            const auto got = feedback(*peer, f.wire);
            require(got.size() == expected.size(), std::format("{} messages", got.size()));
            for (std::size_t i = 0; i < expected.size(); ++i) require(got[i] == expected[i], got[i]);
            const auto contexts = error_contexts(*peer, f.wire);
            require(contexts.size() == 11, std::format("{} context lines", contexts.size()));
            const std::vector<std::pair<std::size_t, std::string>> lines{
                {0, "/gamerule randomTickSpeed x :: ...TickSpeed [x]<command.context.here>"},
                {2, "/gamerule randomTickSpeed 5x :: ...ickSpeed 5[x]<command.context.here>"},
                {4, "/gamerule doDaylightCycle \"tru :: ...Cycle \"tru<command.context.here>"},
                {7, "/gamerule noSuchRule :: gamerule [noSuchRule]<command.context.here>"},
                {9, "/gamerule spawnChunkRadius 3 4 :: ...kRadius 3 [4]<command.context.here>"},
                {10, "/gamerule :: gamerule<command.context.here>"}};
            for (const auto& [i, line] : lines) require(contexts[i] == line, contexts[i]);
            const auto d = f.game->engine().dump();
            require(d.game_rules == std::map<std::string, std::int32_t>{{"keepInventory", 1}, {"randomTickSpeed", 7}, {"spawnChunkRadius", 0}},
                    "the rules are world state");
            require(!start(*f.game), "spawnChunkRadius 0: no spawn chunks");
            f.game->submit(1, I{server::intent::Command{"gamerule spawnChunkRadius 3"}});
            f.game->tick();
            require(start(*f.game) == std::optional<std::uint8_t>{29}, "spawnChunkRadius 3: level 29");
        }
        {
            // After a restart the rules are the world's, and the spawn
            // chunks follow theirs.
            GameFixture f(config);
            f.game->tick();
            require(f.game->engine().dump().game_rules.at("randomTickSpeed") == 7 && start(*f.game) == std::optional<std::uint8_t>{29},
                    "the rules persist");
            auto peer = f.join(1, "Ruler");
            f.game->submit(1, server::Intent{server::intent::Command{"gamerule spawnChunkRadius"}});
            f.game->tick();
            require(feedback(*peer, f.wire).back() == "commands.gamerule.query spawnChunkRadius 3", "queried after the restart");
        }
        std::filesystem::remove_all(dir);
    });

    suite.run("the command reader reads arguments as the official parser", [] {
        using server::CommandReader;
        const auto ticks = [](std::string_view text) {
            CommandReader r(text, 0);
            const auto t = r.time();
            return t ? std::format("{} at {}", *t, r.cursor()) : std::format("{} at {}", t.error().key, t.error().cursor);
        };
        // What both official servers made of /time set <text>.
        for (const auto& [text, expected] : std::vector<std::pair<std::string_view, std::string>>{
                 {"1.5", "2 at 3"}, {"2.5", "3 at 3"}, {"0.4t", "0 at 4"}, {"-0.4", "0 at 4"}, {".5d", "12000 at 3"},
                 {"0.5s", "10 at 4"}, {"0.0001d", "2 at 7"}, {"16777217", "16777216 at 8"}, {"99999999999", "2147483647 at 11"},
                 {"100000d", "2147483647 at 7"}, {"-99999999999", "argument.time.tick_count_too_low at 12"},
                 {"-5", "argument.time.tick_count_too_low at 2"}, {"1e3", "argument.time.invalid_unit at 3"},
                 {"5dd", "argument.time.invalid_unit at 3"}, {"abc", "parsing.float.expected at 0"},
                 {"1.2.3", "parsing.float.invalid at 0"}, {"\"5\"", "parsing.float.expected at 0"}, {"5\"", "5 at 1"}})
            require(ticks(text) == expected, std::format("{}: {}", text, ticks(text)));
        CommandReader quoted("'a\\qb'", 0);
        const auto escape = quoted.boolean();
        require(!escape && escape.error().key == "parsing.quote.escape" && escape.error().cursor == 3, "an invalid escape");
        CommandReader spaced("x ", 1);
        require(spaced.end()->key == "command.unknown.argument" && spaced.end()->cursor == 1, "a trailing space");
    });

    suite.run("the day time advances, is set with /time, sent to players and persists", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        {
            GameFixture f(config);
            auto peer = f.join(1, "Clock");
            for (int i = 0; i < 45; ++i) f.game->tick();
            // On joining, then before every tick whose number is a multiple of 20.
            auto packets = time_packets(*peer, f.wire);
            require(packets.size() == 3, std::format("{} set_time packets", packets.size()));
            for (const auto& [age, day, advancing] : packets)
                require(day == age && advancing, std::format("a new world's day time is its age: {} {}", age, day));
            require(std::get<0>(packets[1]) == 19 && std::get<0>(packets[2]) == 39, "periodic at ages 19 and 39");
            using I = server::Intent;
            const auto age = static_cast<std::int64_t>(f.game->engine().time());
            for (const auto* command : {"time set 1000", "time add 1d", "time query daytime", "time query day", "time set 2x"})
                f.game->submit(1, I{server::intent::Command{command}});
            f.game->tick();
            packets = time_packets(*peer, f.wire);
            require(packets.size() == 5 && packets[3] == std::tuple{age, std::int64_t{1000}, true}
                        && packets[4] == std::tuple{age, std::int64_t{25000}, true},
                    "each change is sent at once");
            const std::vector<std::string> expected{"commands.time.set 1000", "commands.time.set 1000", "commands.time.query 1000",
                                                    "commands.time.query 1", "argument.time.invalid_unit"};
            const auto got = feedback(*peer, f.wire);
            require(got == expected, std::format("{} messages: {}", got.size(), got.empty() ? "" : got.back()));
            require(f.game->engine().day_time() == 25001, "set, then advanced in the tick");
            f.game->submit(1, I{server::intent::Command{"gamerule doDaylightCycle false"}});
            for (int i = 0; i < 20; ++i) f.game->tick();
            require(f.game->engine().day_time() == 25001, "frozen");
            require(!std::get<2>(time_packets(*peer, f.wire).back()), "sent as not advancing");
        }
        {
            GameFixture f(config);
            f.game->tick();
            require(f.game->engine().day_time() == 25001, "the day time persists");
            const auto age = static_cast<std::int64_t>(f.game->engine().time());
            auto peer = f.join(1, "Clock");
            require(time_packets(*peer, f.wire).front() == std::tuple{age, std::int64_t{25001}, false}, "sent on joining");
        }
        std::filesystem::remove_all(dir);
    });

    suite.run("the weather is set with /weather, sent as it changes and on joining, and persists", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        {
            GameFixture f(config);
            auto peer = f.join(1, "Rain");
            using I = server::Intent;
            for (const auto* command : {"weather rain 100", "weather rain 0", "weather snow", "weather thunder 1x"})
                f.game->submit(1, I{server::intent::Command{command}});
            f.game->tick();
            const std::vector<std::string> expected{"commands.weather.set.rain", "argument.time.tick_count_too_low 1 0",
                                                    "command.unknown.argument", "argument.time.invalid_unit"};
            require(feedback(*peer, f.wire) == expected, "replies");
            for (int i = 0; i < 19; ++i) f.game->tick();
            // As both official servers sent them: a level each tick, and the
            // start of rain with both levels once it passes 0.2.
            const auto events = weather_events(*peer, f.wire);
            require(events.size() == 23 && events[0] == std::pair{7, 0.01f} && events[19] == std::pair{7, 0.20000001788139343f}
                        && events[20] == std::pair{1, 0.0f} && events[21] == std::pair{7, 0.20000001788139343f}
                        && events[22] == std::pair{8, 0.0f},
                    std::format("{} weather events", events.size()));
            f.game->submit(1, I{server::intent::Command{"gamerule doWeatherCycle false"}});
            f.game->submit(1, I{server::intent::Command{"weather thunder"}});
            for (int i = 0; i < 40; ++i) f.game->tick();
            // A newcomer sees the rain at once.
            auto other = f.join(2, "Late");
            const auto joined = weather_events(*other, f.wire);
            require(joined.size() >= 3 && joined[0] == std::pair{1, 0.0f} && joined[1].first == 7 && joined[2].first == 8
                        && joined[2].second > 0.3f && joined[2].second < 0.5f,
                    "rain and its levels on joining");
        }
        {
            // A start sets the levels to their flags, as when the official servers load a world.
            GameFixture f(config);
            f.game->tick();
            const auto& w = f.game->engine().weather();
            require(w.raining && w.thundering && w.rain_level == 1.0f && w.thunder_level == 1.0f && w.rain_time > 0,
                    "the weather persists; its levels are its flags'");
        }
        std::filesystem::remove_all(dir);
    });

    suite.run("an empty server pauses", [] {
        auto config = offline_config();
        config.pause_when_empty_seconds = 2;
        GameFixture f(config);
        for (int i = 0; i < 60; ++i) f.game->tick();
        require(f.game->engine().time() == 40, std::format("paused after 40 ticks without players, at {}", f.game->engine().time()));
        f.join(1, "Waker");
        f.game->tick();
        require(f.game->engine().time() == 42, "a player resumes it");
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
        require(f.game->engine().dump().overworld().entities.empty(), "a hand does not harvest stone");
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
        auto drops = f.game->engine().dump().overworld().entities;
        require(drops.size() == 1 && drops[0].stack.item == gameplay::items::cobblestone, "stone drops cobblestone");
        // The drop waits 10 ticks, then the player, whose pickup box reaches 1.3 blocks
        // sideways, takes it (it lands within half a block of its cell's center).
        f.game->submit(1, I{server::intent::Command{"tp 3.5 -60 1.5"}});
        for (int i = 0; i < 14; ++i) f.game->tick();
        require(f.game->engine().dump().overworld().entities.empty(), "picked up");
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
        require(inventory().slots[36].empty() && f.game->engine().dump().overworld().entities.size() == 1, "the last stone was thrown");
        require(f.game->engine().dump().overworld().entities[0].pickup_delay > 30, "with a long pickup delay");
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
            for (const auto& e : d.overworld().entities)
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
            const auto d = f.game->engine().dump();
            for (const auto& [pos, be] : d.overworld().block_entities)
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
            const auto put = f.wire.ids().id(State::play, Bound::clientbound, "set_player_inventory");
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
                } else if (pk->id == put) {
                    out.emplace_back("inventory", pk->fields.varint().value(), 0);
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
        f.hold(1, "minecraft:dropper");
        f.game->submit(1, GameFixture::click({2, -61, 3}, 1));
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
        (void)window_packets();
        f.game->submit(1, I{server::intent::ContainerClose{1}});
        f.game->tick();
        require(inventory().cursor.empty(), "closing returns the carried stack");
        std::int32_t returned = -1;
        for (std::size_t n = 0; n < inventory().slots.size(); ++n)
            if (inventory().slots[n] == contracts::ItemStack{gameplay::items::redstone, 32, 0}) returned = static_cast<std::int32_t>(n);
        require(returned >= 9 && returned < 45, "into the inventory");
        // As both official servers send it: the stack put back by its
        // inventory index, then the own window's slot that differs; the
        // client keeps the rest from the chest window.
        packets = window_packets();
        require(packets.size() == 2 && std::get<0>(packets[0]) == "inventory" && std::get<1>(packets[0]) == (returned >= 36 ? returned - 36 : returned)
                    && has(packets, "slot", 0, returned),
                "the put-back stack, then its slot");
        // In the own window, each change of the crafting grid by a click
        // sends the result again, though nothing changed it, and a slot
        // emptied by a pickup changes twice (client/own_grid_counts): the
        // clicks predict every slot right, so nothing else is sent.
        {
            const auto redstone = content().item_id("minecraft:redstone").value();
            const auto predicted = [&](std::int16_t slot, std::int8_t button, std::vector<std::pair<std::int16_t, std::optional<server::WireStack>>> changed,
                                       std::int32_t carried) {
                auto click = window_click(0, slot, button, 0, state);
                auto& c = std::get<server::intent::ContainerClick>(click);
                c.changed = std::move(changed);
                c.carried = carried ? server::WireStack{redstone, carried, 0, {}} : server::WireStack{};
                return click;
            };
            f.game->submit(1, predicted(static_cast<std::int16_t>(returned), 0, {{static_cast<std::int16_t>(returned), server::WireStack{}}}, 32));
            f.game->tick();
            require(inventory().cursor.count == 32 && window_packets().empty(), "picked up: nothing sent");
            f.game->submit(1, predicted(1, 1, {{1, server::WireStack{redstone, 1, 0, {}}}}, 31));
            f.game->tick();
            packets = window_packets();
            require(inventory().slots[1].count == 1 && packets.size() == 1 && has(packets, "slot", 0, 0), "one into the grid: the result sent");
            f.game->submit(1, predicted(1, 0, {{1, server::WireStack{redstone, 32, 0, {}}}}, 0));
            f.game->tick();
            packets = window_packets();
            require(inventory().slots[1].count == 32 && packets.size() == 1 && has(packets, "slot", 0, 0), "the rest: the result sent again");
            f.game->submit(1, predicted(1, 0, {{1, server::WireStack{}}}, 32));
            f.game->tick();
            packets = window_packets();
            require(inventory().slots[1].empty() && packets.size() == 2 && has(packets, "slot", 0, 0), "picked up, the slot emptied: the result sent twice");
            f.game->submit(1, predicted(2, 1, {{2, server::WireStack{redstone, 1, 0, {}}}}, 31));
            f.game->tick();
            packets = window_packets();
            require(packets.size() == 1 && has(packets, "slot", 0, 0), "one into slot 2: once");
            f.game->submit(1, predicted(static_cast<std::int16_t>(returned), 0, {{static_cast<std::int16_t>(returned), server::WireStack{redstone, 31, 0, {}}}}, 0));
            f.game->tick();
            require(window_packets().empty(), "the rest put back, out of the grid: nothing sent");
            // Closing gives the grid back without sending the result again
            // (it did not change: no recipes here).
            f.game->submit(1, I{server::intent::ContainerClose{0}});
            f.game->tick();
            packets = window_packets();
            require(inventory().slots[2].empty() && !has(packets, "slot", 0, 0), "closed: the grid given back, the result not sent");
            returned = -1;
            for (std::size_t n = 0; n < inventory().slots.size(); ++n)
                if (inventory().slots[n].item == gameplay::items::redstone) returned = static_cast<std::int32_t>(n);
            require(returned >= 9 && inventory().slots[static_cast<std::size_t>(returned)].count == 32, "all 32 together again");
        }
        // A dropper opens a generic_3x3 window: 9 slots, then the 36 inventory slots.
        require(f.at({2, -60, 3}).starts_with("minecraft:dropper"), "dropper placed");
        f.game->submit(1, GameFixture::click({2, -60, 3}, 3));
        f.game->tick();
        packets = window_packets();
        require(has(packets, "open", 2, 6) && has(packets, "content", 2, 45), "a generic_3x3 window opened");
        f.game->submit(1, I{server::intent::ContainerClose{2}});
        f.game->tick();
        require(window_packets().empty(), "closing a window changed nothing: nothing is sent");
        // A window whose container goes out of reach closes, and the client is told.
        f.game->submit(1, GameFixture::click({2, -60, 1}, 4));
        f.game->tick();
        packets = window_packets();
        require(has(packets, "open", 3, 2), "the next window has the next id");
        f.game->submit(1, I{server::intent::Command{"tp 20.5 -60 20.5"}});
        f.game->tick();
        f.game->tick();
        require(has(window_packets(), "close", 3, 0), "closed out of reach");
        // A window closed in the tick it opens again: the stack the close puts
        // back is sent before the new window, as a tick apart.
        f.game->submit(1, I{server::intent::Command{"tp 2.5 -60 -0.5"}});
        f.game->tick();
        f.game->submit(1, GameFixture::click({2, -60, 1}, 5));
        f.game->tick();
        require(has(window_packets(), "open", 4, 2), "the chest opened again");
        f.game->submit(1, window_click(4, 0, 0, 0, state));
        f.game->tick();
        (void)window_packets();
        require(inventory().cursor.item == gameplay::items::redstone, "a stack carried");
        f.game->submit(1, I{server::intent::ContainerClose{4}});
        f.game->submit(1, GameFixture::click({2, -60, 1}, 6));
        f.game->tick();
        packets = window_packets();
        const auto first = [&](std::string_view name) {
            return std::ranges::find_if(packets, [&](const auto& t) { return std::get<0>(t) == name; }) - packets.begin();
        };
        require(inventory().cursor.empty() && has(packets, "open", 5, 2) && first("inventory") < first("open"), "put back, then the new window");
    });

    suite.run("a use that changes its stack sends the whole own window, a drop nothing, and each window counts its state ids", [] {
        // As both official servers do (circuit-clienttest/window-sync.js,
        // both versions alike in these packets and state ids). The player
        // stands at the spawn (0, -60, 0) and looks at the floor's top at
        // (2.5, -60, 2.5), as in the bucket suite.
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Clerk");
        const contracts::PlayerKey key{1, 1};
        const auto inventory = [&] {
            const auto inv = f.game->engine().view().player(key).value();
            return inv ? *inv : empty_inventory(key);
        };
        // Window packets since the last call: (name, window, slot count,
        // slot index or menu type, state id).
        std::size_t seen = 0;
        std::map<std::int32_t, std::int32_t> state; // the last state id of each window
        const auto window_packets = [&] {
            const auto open = f.wire.ids().id(State::play, Bound::clientbound, "open_screen");
            const auto content_id = f.wire.ids().id(State::play, Bound::clientbound, "container_set_content");
            const auto slot_id = f.wire.ids().id(State::play, Bound::clientbound, "container_set_slot");
            std::vector<std::tuple<std::string, std::int32_t, std::int32_t, std::int32_t>> out;
            std::lock_guard lock(peer->mutex);
            for (; seen < peer->packets.size(); ++seen) {
                auto pk = protocol::open_packet(peer->packets[seen]);
                if (!pk) continue;
                if (pk->id == open) {
                    const auto window = pk->fields.varint().value();
                    out.emplace_back("open", window, pk->fields.varint().value(), 0);
                } else if (pk->id == content_id || pk->id == slot_id) {
                    const auto window = pk->fields.varint().value();
                    const auto id = pk->fields.varint().value();
                    state[window] = id;
                    out.emplace_back(pk->id == content_id ? "content" : "slot", window,
                                     pk->id == content_id ? pk->fields.varint().value() : pk->fields.i16().value(), id);
                }
            }
            return out;
        };
        using Packets = std::vector<std::tuple<std::string, std::int32_t, std::int32_t, std::int32_t>>;
        const double dx = 2.5, dy = -1.62, dz = 2.5;
        const auto yaw = static_cast<float>(std::atan2(-dx, dz) * 180.0 / std::numbers::pi);
        const auto pitch = static_cast<float>(-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0 / std::numbers::pi);
        std::int32_t seq = 0;
        // The held bucket used on the water at (2, -60, 2), as a client sends it.
        const auto fill = [&] {
            f.game->submit(1, I{server::intent::Command{"setblock 2 -60 2 minecraft:water[level=0]"}});
            f.game->tick();
            (void)window_packets();
            f.game->submit(1, GameFixture::click({2, -61, 2}, ++seq));
            f.game->submit(1, I{server::intent::Use{0, ++seq, yaw, pitch}});
            f.game->tick();
        };
        const auto bucket = content().item_id("minecraft:bucket").value();
        const auto dirt = content().item_id("minecraft:dirt").value();
        f.game->submit(1, I{server::intent::CreativeSlot{36, 2, bucket}});
        f.game->submit(1, I{server::intent::CreativeSlot{37, 5, dirt}});
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->tick();
        (void)window_packets();
        const auto own = state[0];
        fill();
        require(inventory().slots[36] == contracts::ItemStack{gameplay::items::bucket, 1, 0}
                    && std::ranges::count(inventory().slots, contracts::ItemStack{gameplay::items::water_bucket, 1, 0}) == 1,
                "a bucket of two filled");
        require(window_packets() == Packets{{"content", 0, 46, own + 1}}, "the whole own window, in place of the two slots");
        // The bucket left, used at the sky: it changes nothing, and nothing is sent.
        f.game->submit(1, I{server::intent::Use{0, ++seq, 0.0f, -90.0f}});
        f.game->tick();
        require(inventory().slots[36].count == 1 && window_packets().empty(), "a use that changes nothing sends nothing");
        // Drops: the client empties its slot itself.
        f.game->submit(1, I{server::intent::HeldSlot{1}});
        f.game->submit(1, I{server::intent::Dig{4, {0, 0, 0}, 0, 0}});
        f.game->tick();
        require(inventory().slots[37].count == 4 && window_packets().empty(), "one dropped: nothing sent");
        f.game->submit(1, I{server::intent::Dig{3, {0, 0, 0}, 0, 0}});
        f.game->tick();
        require(inventory().slots[37].empty() && window_packets().empty(), "the stack dropped: nothing sent");
        // One dropped, then in the same tick a shift-click fills the stack
        // again: the client shows the stack the drop left, so it is sent.
        f.game->submit(1, I{server::intent::Command{"gamemode creative"}});
        f.game->tick();
        f.game->submit(1, I{server::intent::CreativeSlot{37, 5, dirt}});
        f.game->submit(1, I{server::intent::CreativeSlot{9, 1, dirt}});
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->tick();
        (void)window_packets();
        f.game->submit(1, I{server::intent::Dig{4, {0, 0, 0}, 0, 0}});
        f.game->submit(1, window_click(0, 9, 0, 1, state[0]));
        f.game->tick();
        require(inventory().slots[37].count == 5 && inventory().slots[9].empty(), "one dropped, one shift-clicked onto the stack");
        const auto refilled = window_packets();
        require(refilled.size() == 2 && std::get<2>(refilled[0]) == 9 && std::get<2>(refilled[1]) == 37, "both slots sent");
        // In creative mode the bucket stays and a water bucket is held already.
        f.game->submit(1, I{server::intent::HeldSlot{0}});
        f.game->submit(1, I{server::intent::Command{"gamemode creative"}});
        f.game->tick();
        fill();
        require(f.at({2, -60, 2}) == "minecraft:air" && inventory().slots[36].count == 1 && window_packets().empty(),
                "creative: the water taken, nothing sent");
        // A chest's window counts from 1; a use with it open sends the whole
        // own window, which goes on counting its own ids, then the chest
        // window's hotbar slot.
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->submit(1, I{server::intent::Command{"setblock 2 -60 -2 minecraft:chest[facing=north,type=single,waterlogged=false]"}});
        f.game->tick();
        (void)window_packets();
        f.game->submit(1, GameFixture::click({2, -60, -2}, ++seq));
        f.game->tick();
        require(window_packets() == Packets{{"open", 1, 2, 0}, {"content", 1, 63, 1}}, "the chest window's first state id is 1");
        const auto own_last = state[0];
        fill();
        require(inventory().slots[36].item == gameplay::items::water_bucket, "the last bucket filled with the chest open");
        require(window_packets() == Packets{{"content", 0, 46, own_last + 1}, {"slot", 1, 54, 2}}, "the own window, then the chest window's slot");
        // A click on the chest window's state id is not stale.
        f.game->submit(1, window_click(1, 0, 0, 0, 2));
        f.game->tick();
        require(std::ranges::none_of(window_packets(), [](const auto& t) { return std::get<0>(t) == "content"; }), "the chest window's id is current");
        f.game->submit(1, I{server::intent::ContainerClose{1}});
        f.game->submit(1, I{server::intent::Command{"gamemode creative"}});
        f.game->tick();
        f.game->submit(1, I{server::intent::CreativeSlot{37, 5, dirt}});
        f.game->tick();
        (void)window_packets();
        // Back in the own window, a click on the own window's id is current too.
        f.game->submit(1, window_click(0, 37, 0, 0, state[0]));
        f.game->tick();
        require(inventory().cursor.count == 5 && std::ranges::none_of(window_packets(), [](const auto& t) { return std::get<0>(t) == "content"; }),
                "the own window's id goes on");
    });

    suite.run("players get status effects by /effect with both official servers' replies, packets and entity data", [] {
        // circuit-clienttest/effects2.js, effects3.js and effect-colors.js:
        // both versions alike, but for orders that vary from run to run there
        // (here effects come in id order).
        using I = server::Intent;
        GameFixture f;
        auto alice = f.join(1, "Alice");
        auto bob = f.join(2, "Bob");
        for (int i = 0; i < 8; ++i) f.game->tick();
        // Effect packets and entity data since the last call, as text: an
        // effect "effect <entity> <id> <amplifier> <duration> <flags>",
        // "removed <entity> <id>", data "data <entity> <index>=<value> ...",
        // health "health".
        std::map<std::uint64_t, std::size_t> seen;
        const auto packets = [&](Recorder& r) {
            const auto update = f.wire.ids().id(State::play, Bound::clientbound, "update_mob_effect");
            const auto remove = f.wire.ids().id(State::play, Bound::clientbound, "remove_mob_effect");
            const auto data = f.wire.ids().id(State::play, Bound::clientbound, "set_entity_data");
            const auto health = f.wire.ids().id(State::play, Bound::clientbound, "set_health");
            std::vector<std::string> out;
            std::lock_guard lock(r.mutex);
            for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                auto pk = protocol::open_packet(r.packets[at]);
                if (!pk) continue;
                auto& in = pk->fields;
                if (pk->id == update) {
                    const auto entity = in.varint().value(), id = in.varint().value(), amplifier = in.varint().value(), duration = in.varint().value();
                    out.push_back(std::format("effect {} {} {} {} {}", entity, id, amplifier, duration, in.u8().value()));
                } else if (pk->id == remove) {
                    const auto entity = in.varint().value();
                    out.push_back(std::format("removed {} {}", entity, in.varint().value()));
                } else if (pk->id == data) {
                    std::string text = std::format("data {}", in.varint().value());
                    for (auto index = in.u8().value(); index != 0xFF; index = in.u8().value()) {
                        const auto type = in.varint().value();
                        text += std::format(" {}=", index);
                        if (type == 0) text += std::to_string(in.u8().value());
                        else if (type == 8) text += in.boolean().value() ? "true" : "false";
                        else if (type == 18) {
                            const auto n = in.varint().value();
                            text += "[";
                            for (std::int32_t k = 0; k < n; ++k) {
                                const auto particle = in.varint().value();
                                text += (k ? "," : "") + std::to_string(particle);
                                if (particle == 20) text += std::format(":{:08X}", static_cast<std::uint32_t>(in.i32().value()));
                            }
                            text += "]";
                        } else {
                            text += "?";
                            break;
                        }
                    }
                    out.push_back(std::move(text));
                } else if (pk->id == health) {
                    out.push_back("health");
                }
            }
            return out;
        };
        // A command in one tick: its effect packets at once, the entity data
        // they decide at the start of the next tick (client/effect_edges).
        const auto command = [&](std::string text) {
            f.game->submit(1, I{server::intent::Command{std::move(text)}});
            f.game->tick();
        };
        const auto then = [&] { f.game->tick(); };
        (void)packets(*alice);
        (void)packets(*bob);
        // Alice's entity id, from the first effect packet.
        command("effect give Alice minecraft:speed 100 1");
        auto a = packets(*alice);
        require(a.size() == 1 && a[0].starts_with("effect ") && a[0].ends_with(" 0 1 2000 14"), "a new speed: " + (a.empty() ? "" : a[0]));
        const auto entity = a[0].substr(7, a[0].find(' ', 7) - 7);
        require(packets(*bob).empty(), "nothing for bob yet");
        then();
        require(packets(*alice) == std::vector<std::string>{std::format("data {} 10=[20:FF33EBFF]", entity)}, "its particle, a tick later");
        require(packets(*bob) == std::vector<std::string>{std::format("data {} 10=[20:FF33EBFF]", entity)}, "bob sees the particle");
        require(feedback(*alice, f.wire).back() == "commands.effect.give.success.single effect.minecraft.speed Alice", "applied");
        command("effect give Alice speed 50 1");
        then();
        require(packets(*alice).empty() && feedback(*alice, f.wire).back() == "commands.effect.give.failed", "the same amplifier, shorter: refused");
        // Glowing for both, its particles hidden: the shared flag, Alice's
        // particles sent again, Bob's ambience (his only effect shows none).
        command("effect give @a minecraft:glowing 30 0 true");
        then();
        a = packets(*alice);
        require(a.size() == 3 && a[0] == std::format("effect {} 23 0 600 8", entity) && a[1] == std::format("data {} 0=64 10=[20:FF33EBFF]", entity),
                "glowing: " + (a.size() > 1 ? a[1] : ""));
        const auto b = packets(*bob);
        const auto bob_data = std::ranges::find_if(b, [](const auto& t) { return t.ends_with(" 0=64 11=true"); });
        require(bob_data != b.end() && std::ranges::count(b, *bob_data) == 1, "bob glows, his effects ambient");
        const auto bob_seen = *bob_data;
        require(feedback(*alice, f.wire).back() == "commands.effect.give.success.multiple effect.minecraft.glowing 2", "two targets");
        command("effect clear Alice minecraft:speed");
        then();
        a = packets(*alice);
        require(a == std::vector<std::string>{std::format("removed {} 0", entity), std::format("data {} 10=[] 11=true", entity)}, "speed cleared");
        require(feedback(*alice, f.wire).back() == "commands.effect.clear.specific.success.single effect.minecraft.speed Alice", "removed");
        command("effect clear Alice minecraft:speed");
        require(feedback(*alice, f.wire).back() == "commands.effect.clear.specific.failed", "nothing to remove");
        // Errors.
        command("effect give Alice minecraft:no_such_effect 5 0");
        require(feedback(*alice, f.wire).back() == "argument.resource.not_found minecraft:no_such_effect minecraft:mob_effect", "an unknown effect");
        command("effect give Alice speed 0");
        require(feedback(*alice, f.wire).back() == "argument.integer.low 1 0", "seconds below 1");
        command("effect give Alice speed 5 256");
        require(feedback(*alice, f.wire).back() == "argument.integer.big 255 256", "an amplifier above 255");
        command("effect give Nobody speed 5 0");
        require(feedback(*alice, f.wire).back() == "argument.entity.notfound.entity", "no such player");
        // A stronger effect for 3 seconds over a weaker one: the weaker comes
        // back in the 60th tick, counting the one that gave it.
        command("effect give Alice haste infinite 0");
        then();
        (void)packets(*alice);
        command("effect give Alice haste 3 1");
        a = packets(*alice);
        require(a == std::vector<std::string>{std::format("effect {} 2 1 60 6", entity)}, "the stronger haste");
        for (int i = 0; i < 58; ++i) f.game->tick();
        a = packets(*alice);
        require(std::ranges::none_of(a, [](const auto& t) { return t.starts_with("effect") || t.starts_with("removed"); }), "the stronger still shown");
        f.game->tick();
        require(packets(*alice) == std::vector<std::string>{std::format("effect {} 2 0 -1 6", entity)}, "the infinite haste back");
        // An effect ends after its duration; its data follows a tick later.
        then();
        (void)packets(*alice);
        command("effect give Alice minecraft:invisibility 2");
        then();
        a = packets(*alice);
        require(a.size() == 2 && a[0] == std::format("effect {} 13 0 40 14", entity) && a[1] == std::format("data {} 0=96 10=[20:FFD9C043,20:FFF6F6F6]", entity),
                "invisible: " + (a.size() > 1 ? a[1] : ""));
        for (int i = 0; i < 37; ++i) f.game->tick();
        require(packets(*alice).empty(), "still invisible");
        f.game->tick();
        require(packets(*alice) == std::vector<std::string>{std::format("removed {} 13", entity)}, "invisibility ended");
        then();
        require(packets(*alice) == std::vector<std::string>{std::format("data {} 0=64 10=[20:FFD9C043]", entity)}, "its data a tick later");
        // A rejoin: the effects again (no longer new), before the health,
        // then the entity data a tick later.
        command("effect give Alice minecraft:water_breathing 100 0 true");
        f.game->leave(1);
        f.game->tick();
        alice = f.join(1, "Alice");
        seen[1] = 0;
        a = packets(*alice);
        // Bob appears with his data as last told (glowing, his effects ambient).
        require(std::erase(a, bob_seen) == 1, "bob appears glowing");
        const auto health = std::ranges::find(a, std::string("health"));
        require(health != a.end() && health - a.begin() == 3 && health + 1 == a.end(), "three effects, then the health");
        // Water breathing went down in the tick that gave it (none while
        // away, none in the joining tick); glowing in each tick since its own.
        require(a[0].ends_with(" 2 0 -1 6") && a[1].ends_with(" 12 0 1999 0") && a[2].ends_with(" 23 0 486 0"),
                "haste, water breathing, glowing: " + a[0] + " | " + a[1] + " | " + a[2]);
        then();
        require(packets(*alice) == std::vector<std::string>{std::format("data {} 0=64 10=[20:FFD9C043]", a[0].substr(7, a[0].find(' ', 7) - 7))},
                "then the data");
        // Death takes them.
        command("kill");
        f.game->submit(1, I{server::intent::ClientCommand{0}});
        f.game->tick();
        command("effect clear Alice");
        require(feedback(*alice, f.wire).back() == "commands.effect.clear.everything.failed", "a respawned player has no effects");
    });

    suite.run("chests send their lids' block events and trapped chests power by their viewers", [] {
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Clerk");
        f.game->submit(1, I{server::intent::Command{"setblock 2 -60 1 minecraft:trapped_chest[facing=north,type=single,waterlogged=false]"}});
        f.game->submit(1, I{server::intent::Command{"setblock 3 -60 1 minecraft:redstone_wire[east=none,north=none,power=0,south=none,west=none]"}});
        f.game->tick();
        require(f.at({2, -60, 1}).starts_with("minecraft:trapped_chest[") && f.at({3, -60, 1}).starts_with("minecraft:redstone_wire["), "set");
        // Block events since the last call: (x, y, z, event, parameter, block id).
        std::size_t seen = 0;
        const auto events = [&] {
            const auto id = f.wire.ids().id(State::play, Bound::clientbound, "block_event");
            std::vector<std::tuple<std::int32_t, std::int32_t, std::int32_t, std::uint8_t, std::uint8_t, std::int32_t>> out;
            std::lock_guard lock(peer->mutex);
            for (; seen < peer->packets.size(); ++seen) {
                auto pk = protocol::open_packet(peer->packets[seen]);
                if (!pk || pk->id != id) continue;
                const auto pos = pk->fields.position().value();
                const auto event = pk->fields.u8().value();
                const auto param = pk->fields.u8().value();
                out.emplace_back(pos.x, pos.y, pos.z, event, param, pk->fields.varint().value());
            }
            return out;
        };
        const auto trapped = content().block_id("minecraft:trapped_chest").value();
        using Event = std::tuple<std::int32_t, std::int32_t, std::int32_t, std::uint8_t, std::uint8_t, std::int32_t>;
        (void)events();
        f.game->submit(1, GameFixture::click({2, -60, 1}, 1));
        f.game->tick();
        require(events() == std::vector<Event>{{2, -60, 1, 1, 1, trapped}} && f.at({3, -60, 1}).contains("power=1"), "opened: the lid and power 1");
        for (int i = 0; i < 5; ++i) f.game->tick();
        require(events() == std::vector<Event>{{2, -60, 1, 1, 1, trapped}}, "the recheck 5 ticks later sends the lid again");
        f.game->submit(1, I{server::intent::ContainerClose{1}});
        f.game->tick();
        require(events() == std::vector<Event>{{2, -60, 1, 1, 0, trapped}} && f.at({3, -60, 1}).contains("power=0"), "closed: the lid and no power");
    });

    suite.run("barrels open under a solid block and ender chests open, with their translated titles", [] {
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Clerk");
        f.game->submit(1, I{server::intent::Command{"setblock 2 -60 1 minecraft:barrel[facing=up,open=false]"}});
        f.game->submit(1, I{server::intent::Command{"setblock 2 -59 1 minecraft:stone"}});
        f.game->tick();
        std::size_t seen = 0;
        // Windows opened since the last call: (window id, menu type, translation key).
        const auto opened = [&] {
            const auto open = f.wire.ids().id(State::play, Bound::clientbound, "open_screen");
            std::vector<std::tuple<std::int32_t, std::int32_t, std::string>> out;
            std::lock_guard lock(peer->mutex);
            for (; seen < peer->packets.size(); ++seen) {
                auto pk = protocol::open_packet(peer->packets[seen]);
                if (!pk || pk->id != open) continue;
                const auto window = pk->fields.varint().value();
                const auto type = pk->fields.varint().value();
                const auto title = protocol::nbt::read_network(pk->fields).value();
                const auto* key = title.find("translate");
                out.emplace_back(window, type, key ? key->text() : std::string{});
            }
            return out;
        };
        (void)opened();
        f.game->submit(1, GameFixture::click({2, -60, 1}, 1, 3));
        f.game->tick();
        require((opened() == std::vector<std::tuple<std::int32_t, std::int32_t, std::string>>{{1, 2, "container.barrel"}})
                    && f.at({2, -60, 1}) == "minecraft:barrel[facing=up,open=true]",
                "a generic_9x3 window titled container.barrel; the barrel opens");
        f.game->submit(1, I{server::intent::ContainerClose{1}});
        f.game->tick();
        require(f.at({2, -60, 1}) == "minecraft:barrel[facing=up,open=false]", "and closes");
        // An ender chest's window has its own title and 27 slots.
        f.game->submit(1, I{server::intent::Command{"setblock 4 -60 1 minecraft:ender_chest[facing=south,waterlogged=false]"}});
        f.game->tick();
        (void)opened();
        f.game->submit(1, GameFixture::click({4, -60, 1}, 2, 3));
        f.game->tick();
        require((opened() == std::vector<std::tuple<std::int32_t, std::int32_t, std::string>>{{2, 2, "container.enderchest"}}),
                "an ender chest opens a generic_9x3 window titled container.enderchest");
        // A shulker box's window is its own.
        f.game->submit(1, I{server::intent::Command{"setblock 6 -60 1 minecraft:purple_shulker_box[facing=up]"}});
        f.game->tick();
        (void)opened();
        f.game->submit(1, GameFixture::click({6, -60, 1}, 3, 3));
        f.game->tick();
        require((opened() == std::vector<std::tuple<std::int32_t, std::int32_t, std::string>>{{3, 20, "container.shulkerBox"}}),
                "a shulker box opens a shulker_box window titled container.shulkerBox");
    });

    suite.run("empty maps become filled maps whose data the player gets, and cartography tables open", [] {
        // Item components come from the official data packs (CIRCUIT_VANILLA_JAR_1_21_8).
        const char* jar = std::getenv("CIRCUIT_VANILLA_JAR_1_21_8");
        if (!jar || !*jar) {
            std::puts("  (no official 1.21.8 server jar given: the map checks did not run)");
            return;
        }
        using I = server::Intent;
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        auto d = server::load_server_data(bundle, {jar, {}});
        require(d.has_value(), d ? "" : d.error());
        GameFixture f(offline_config(), std::move(*d));
        auto peer = f.join(1, "Mapper");
        f.hold(1, "minecraft:map");
        f.game->tick();
        f.game->submit(1, server::intent::Use{0, 1, 0.0f, 0.0f});
        f.game->tick();
        f.game->tick();
        // Each map data packet: the map's id, scale, lock, markers' types (-1: none sent), and its colors' size.
        using MapPackets = std::vector<std::tuple<std::int32_t, std::uint8_t, bool, std::vector<std::int32_t>, std::size_t>>;
        const auto map_packets = [&](std::size_t from) {
            MapPackets maps;
            const auto id = f.wire.ids().id(State::play, Bound::clientbound, "map_item_data");
            std::lock_guard lock(peer->mutex);
            for (std::size_t i = from; i < peer->packets.size(); ++i) {
                const auto& bytes = peer->packets[i];
                auto pk = protocol::open_packet(bytes);
                if (!pk || pk->id != id) continue;
                auto& r = pk->fields;
                const auto map = r.varint().value();
                const auto scale = r.u8().value();
                const auto locked = r.boolean().value();
                std::vector<std::int32_t> types;
                if (!r.boolean().value()) {
                    types.push_back(-1);
                } else {
                    const auto n = r.varint().value();
                    for (std::int32_t k = 0; k < n; ++k) {
                        types.push_back(r.varint().value());
                        (void)r.u8(); (void)r.u8(); (void)r.u8();
                        require(!r.boolean().value(), "markers without names");
                    }
                }
                std::size_t colors = 0;
                if (const auto columns = r.u8().value(); columns > 0) {
                    (void)r.u8(); (void)r.u8(); (void)r.u8();
                    colors = r.prefixed_bytes(1 << 16).value().size();
                }
                maps.emplace_back(map, scale, locked, types, colors);
            }
            return maps;
        };
        require((map_packets(0) == MapPackets{{0, 0, false, {0}, 128 * 128}}), "map 0 of scale 0 with the player's marker and its colors, once");
        // Held (the creative player's filled map went to hotbar 1), it explores in the tick: every column
        // but the first (its step is 1), the stone floor; the patch comes without markers.
        const auto before = [&] { std::lock_guard lock(peer->mutex); return peer->packets.size(); }();
        f.game->submit(1, server::intent::HeldSlot{1});
        f.game->tick();
        f.game->tick();
        require((map_packets(before) == MapPackets{{0, 0, false, {-1}, 127 * 128}}), "a patch of 127 columns, once");
        {
            const auto map = f.game->engine().view().map(0).value();
            require(map && map->holders.size() == 1 && map->holders.front().step == 2 && map->colors[64 + 64 * 128] == 11 * 4 + 1
                        && map->colors[0] == 0,
                    "map 0 explored twice; the floor stone level, column 0 not yet");
        }
        f.game->submit(1, I{server::intent::Command{"setblock 2 -60 1 minecraft:cartography_table"}});
        f.game->tick();
        f.game->submit(1, GameFixture::click({2, -60, 1}, 1, 3));
        f.game->tick();
        bool opened = false;
        {
            const auto open = f.wire.ids().id(State::play, Bound::clientbound, "open_screen");
            std::lock_guard lock(peer->mutex);
            for (const auto& bytes : peer->packets) {
                auto pk = protocol::open_packet(bytes);
                if (!pk || pk->id != open) continue;
                (void)pk->fields.varint();
                const auto type = pk->fields.varint().value();
                const auto title = protocol::nbt::read_network(pk->fields).value();
                const auto* key = title.find("translate");
                opened = type == 23 && key && key->text() == "container.cartography_table";
            }
        }
        require(opened, "a cartography table opens a cartography_table window titled container.cartography_table");
    });

    suite.run("the recipe book: displays, unlocking and the books of players who come back", [] {
        // Recipes and advancements come from the official data packs (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        const char* jar = std::getenv("CIRCUIT_VANILLA_JAR_1_21_8");
        if (!jar || !*jar) {
            std::puts("  (no official 1.21.8 server jar given: the recipe book checks did not run)");
            return;
        }
        if (const char* old = std::getenv("CIRCUIT_VANILLA_JAR_1_21_4"); old && *old) {
            const auto d = server::load_server_data(**contracts::BundleRegistry::find("java-1.21.4"), {old, {}});
            require(d.has_value() && d->recipe_book, "1.21.4 data");
            const auto& b = *d->recipe_book;
            require(b.by_id.at("minecraft:crafting_table") == 259 && b.by_id.at("minecraft:oak_planks") == 777
                        && b.by_id.at("minecraft:black_bundle") == 66 && b.recipes[66].display.result.kind == server::SlotDisplay::Kind::item,
                    "1.21.4's display ids, and a transmute's result as an item");
        }
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        auto d = server::load_server_data(bundle, {jar, {}});
        require(d.has_value() && d->recipe_book, d ? "" : d.error());
        {
            const auto& b = *d->recipe_book;
            const auto& planks = b.recipes[b.by_id.at("minecraft:oak_planks")];
            require(b.recipes.size() == 1396 && b.all == 1407 && b.by_id.at("minecraft:crafting_table") == 262 && b.by_id.at("minecraft:oak_planks") == 805
                        && planks.group == 8 && planks.notification && !b.recipes[262].notification
                        && !b.recipes[b.by_id.at("minecraft:map_extending")].in_book,
                    "display ids by sorted recipe ids, groups, notifications, map extending numbered but not in books");
            require(b.advancements.size() == 1395 && b.unsupported == 0 && b.on_tick.size() == 1 && b.in_water.size() == 10 && b.occupied.size() == 1,
                    std::format("{} advancements, {} criteria not triggered", b.advancements.size(), b.unsupported));
        }
        GameFixture f(offline_config(), std::move(*d));
        const auto book_packets = [&](const std::shared_ptr<Recorder>& peer, std::size_t from) {
            // Per packet: "settings", "add <replace> <count> <first id> <flags of the last>", "remove <ids>".
            std::vector<std::string> out;
            const auto add = f.wire.ids().id(State::play, Bound::clientbound, "recipe_book_add");
            const auto remove = f.wire.ids().id(State::play, Bound::clientbound, "recipe_book_remove");
            const auto settings = f.wire.ids().id(State::play, Bound::clientbound, "recipe_book_settings");
            std::lock_guard lock(peer->mutex);
            for (std::size_t i = from; i < peer->packets.size(); ++i) {
                const auto& bytes = peer->packets[i];
                auto pk = protocol::open_packet(bytes);
                if (!pk) continue;
                if (pk->id == settings) out.push_back("settings");
                if (pk->id == remove) {
                    const auto n = pk->fields.varint().value();
                    std::string ids = "remove";
                    for (std::int32_t k = 0; k < n; ++k) ids += " " + std::to_string(pk->fields.varint().value());
                    out.push_back(ids);
                }
                if (pk->id == add) {
                    const auto n = pk->fields.varint().value();
                    const auto first = n > 0 ? pk->fields.varint().value() : -1;
                    out.push_back(std::format("add {} {} {} {}", static_cast<int>(bytes.back()), n, first,
                                              n > 0 ? static_cast<int>(bytes[bytes.size() - 2]) : -1));
                }
            }
            return out;
        };
        auto peer = f.join(1, "Chef");
        require((book_packets(peer, 0) == std::vector<std::string>{"settings", "add 1 0 -1 -1", "add 0 1 262 2"}),
                "settings, the empty book, then the crafting table (no notification)");
        auto mark = [&] { std::lock_guard lock(peer->mutex); return peer->packets.size(); }();
        f.hold(1, "minecraft:oak_log");
        f.game->tick();
        const auto unlocked = book_packets(peer, mark);
        require((unlocked == std::vector<std::string>{"add 0 1 805 3", "add 0 1 811 3", "add 0 1 148 3"}),
                std::format("an oak log unlocks oak planks, oak wood and charcoal, each by its advancement, in their ids' order: {}",
                            unlocked.empty() ? "none" : unlocked.front()));
        mark = [&] { std::lock_guard lock(peer->mutex); return peer->packets.size(); }();
        f.game->submit(1, server::Intent{server::intent::Command{"recipe take Chef minecraft:oak_planks"}});
        f.game->submit(1, server::Intent{server::intent::RecipeSeen{811}});
        f.game->submit(1, server::Intent{server::intent::RecipeBookSettings{2, true, false}});
        f.game->tick();
        require((book_packets(peer, mark) == std::vector<std::string>{"remove 805"}), "/recipe take removes it");
        f.game->leave(1);
        f.game->tick();
        peer = f.join(1, "Chef");
        const auto back = book_packets(peer, 0);
        std::string seen;
        for (const auto& b : back) seen += b + "; ";
        require((back == std::vector<std::string>{"settings", "add 1 3 148 0"}), "back: the known recipes, oak wood seen: " + seen);
    });

    suite.run("recipes of the book are placed into windows, with ghosts for those missing items", [] {
        const char* jar = std::getenv("CIRCUIT_VANILLA_JAR_1_21_8");
        if (!jar || !*jar) {
            std::puts("  (no official 1.21.8 server jar given: the recipe placement checks did not run)");
            return;
        }
        using I = server::Intent;
        auto d = server::load_server_data(**contracts::BundleRegistry::find("java-1.21.8"), {jar, {}});
        require(d.has_value() && d->recipe_book, d ? "" : d.error());
        GameFixture f(offline_config(), std::move(*d));
        auto peer = f.join(1, "Chef");
        const contracts::PlayerKey key{1, 1};
        const auto slot = [&](std::size_t i) {
            const auto inv = f.game->engine().view().player(key).value();
            return inv ? inv->slots[i] : contracts::ItemStack{};
        };
        // Ghosts since the last call: (window, display kind).
        std::size_t seen = 0;
        const auto ghosts = [&] {
            const auto id = f.wire.ids().id(State::play, Bound::clientbound, "place_ghost_recipe");
            std::vector<std::pair<std::int32_t, std::int32_t>> out;
            std::lock_guard lock(peer->mutex);
            for (; seen < peer->packets.size(); ++seen) {
                auto pk = protocol::open_packet(peer->packets[seen]);
                if (!pk || pk->id != id) continue;
                const auto window = pk->fields.varint().value();
                out.emplace_back(window, pk->fields.varint().value());
            }
            return out;
        };
        f.hold(1, "minecraft:oak_log");
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->tick();
        (void)ghosts();
        // Oak planks (805, known from the log) into the own window.
        f.game->submit(1, I{server::intent::PlaceRecipe{0, 805, false}});
        f.game->tick();
        require(slot(1).count == 1 && slot(36).empty() && slot(0).count == 4 && ghosts().empty(), "the log in the grid");
        // A crafting table (262) without planks: the log goes back, the ghost (shaped: 1) is shown.
        f.game->submit(1, I{server::intent::PlaceRecipe{0, 262, false}});
        f.game->tick();
        require(slot(1).empty() && slot(36).count == 1 && (ghosts() == std::vector<std::pair<std::int32_t, std::int32_t>>{{0, 1}}),
                "the table's ghost in window 0");
        // Another window's id, and a recipe the player does not know, do nothing.
        f.game->submit(1, I{server::intent::PlaceRecipe{3, 805, false}});
        f.game->submit(1, I{server::intent::Command{"recipe take Chef minecraft:crafting_table"}});
        f.game->tick();
        f.game->submit(1, I{server::intent::PlaceRecipe{0, 262, false}});
        f.game->tick();
        require(slot(1).empty() && slot(36).count == 1 && ghosts().empty(), "ignored");
    });

    suite.run("the rules learn the crafting recipes of each player's book, for doLimitedCrafting", [] {
        const char* jar = std::getenv("CIRCUIT_VANILLA_JAR_1_21_8");
        if (!jar || !*jar) {
            std::puts("  (no official 1.21.8 server jar given: the known recipe checks did not run)");
            return;
        }
        using I = server::Intent;
        auto d = server::load_server_data(**contracts::BundleRegistry::find("java-1.21.8"), {jar, {}});
        require(d.has_value() && d->recipe_book && d->game, d ? "" : d.error());
        GameFixture f(offline_config(), std::move(*d));
        const auto index = [&](std::string_view id) {
            const auto& c = f.data.game->crafting;
            return static_cast<std::uint32_t>(std::ranges::find(c, id, &gameplay::recipes::Recipe::id) - c.begin());
        };
        const auto table = index("minecraft:crafting_table"), planks = index("minecraft:oak_planks");
        const contracts::PlayerKey key{1, 1};
        const auto known = [&] {
            const auto inv = f.game->engine().view().player(key).value();
            return inv ? inv->known_crafting : std::vector<std::uint32_t>{};
        };
        auto peer = f.join(1, "Chef");
        f.game->tick();
        require(known() == std::vector<std::uint32_t>{table}, "a new player: the crafting table");
        f.hold(1, "minecraft:oak_log");
        f.game->tick();
        f.game->tick();
        const auto now = known();
        require(std::ranges::binary_search(now, planks) && std::ranges::binary_search(now, table), "oak planks unlocked by the log");
        f.game->submit(1, I{server::intent::Command{"recipe take Chef minecraft:oak_planks"}});
        f.game->tick();
        f.game->tick();
        require(!std::ranges::binary_search(known(), planks), "taken");
        // Joining again replaces what the rules know with the book.
        f.game->submit(1, I{server::intent::Command{"recipe give Chef minecraft:oak_planks"}});
        f.game->tick();
        f.game->leave(1);
        f.game->tick();
        peer = f.join(1, "Chef");
        f.game->tick();
        require(known() == now, "back: the same recipes");
    });

    suite.run("books and quills are edited and signed, within the edit packet's limits", [] {
        using I = server::Intent;
        // The packet's limits: 100 pages of 1024 code units, a title of 32.
        {
            GameFixture f;
            const auto id = f.wire.ids().id(State::play, Bound::serverbound, "edit_book");
            const auto parse = [&](std::int32_t pages, std::size_t page_units, std::optional<std::size_t> title_units) {
                Writer w;
                w.varint(0).varint(pages);
                for (std::int32_t k = 0; k < pages; ++k) w.string(std::string(page_units, 'x'));
                w.boolean(title_units.has_value());
                if (title_units) w.string(std::string(*title_units, 't'));
                protocol::Reader r(w.bytes());
                return f.wire.parse_play(id, r).has_value();
            };
            require(parse(100, 1024, 32) && !parse(101, 1, std::nullopt) && !parse(1, 1025, std::nullopt) && !parse(1, 1, 33), "the limits");
        }
        const char* jar = std::getenv("CIRCUIT_VANILLA_JAR_1_21_8");
        if (!jar || !*jar) {
            std::puts("  (no official 1.21.8 server jar given: the book editing checks did not run)");
            return;
        }
        auto d = server::load_server_data(**contracts::BundleRegistry::find("java-1.21.8"), {jar, {}});
        require(d.has_value() && d->game, d ? "" : d.error());
        GameFixture f(offline_config(), std::move(*d));
        auto peer = f.join(1, "Writer");
        const contracts::PlayerKey key{1, 1};
        const auto slot = [&](std::size_t i) {
            const auto inv = f.game->engine().view().player(key).value();
            return inv ? inv->slots[i] : contracts::ItemStack{};
        };
        const auto* codec = protocol::ComponentCodec::for_protocol(772);
        const auto writable = *gameplay::items::by_name("minecraft:writable_book"), written = *gameplay::items::by_name("minecraft:written_book");
        f.hold(1, "minecraft:writable_book");
        f.game->tick();
        f.game->submit(1, I{server::intent::EditBook{0, {"hello", "", "world"}, std::nullopt}});
        f.game->tick();
        require(slot(36).item == writable && codec->book_pages_of(slot(36).components) == 3, "three pages written");
        f.game->submit(1, I{server::intent::EditBook{9, {"no"}, std::nullopt}});
        f.game->submit(1, I{server::intent::EditBook{40, {"no"}, std::string("No")}});
        f.game->tick();
        require(slot(36).item == writable && codec->book_pages_of(slot(36).components) == 3 && slot(45).empty(), "another hand: nothing");
        // Swapped into the off hand, it is signed there.
        f.game->submit(1, I{server::intent::Dig{6, {0, 0, 0}, 0, 0}});
        f.game->tick();
        require(slot(36).empty() && slot(45).item == writable, "swapped");
        f.game->submit(1, I{server::intent::EditBook{40, {"first", "second"}, std::string("My Book")}});
        f.game->tick();
        require(slot(45).item == written && codec->book_pages_of(slot(45).components) == 2, "signed");
        f.game->submit(1, I{server::intent::EditBook{40, {"again"}, std::nullopt}});
        f.game->tick();
        require(slot(45).item == written && codec->book_pages_of(slot(45).components) == 2, "a written book is not edited");
    });

    suite.run("knowledge books give their recipes when every one is a recipe, and are used up", [] {
        const char* jar = std::getenv("CIRCUIT_VANILLA_JAR_1_21_8");
        if (!jar || !*jar) {
            std::puts("  (no official 1.21.8 server jar given: the knowledge book checks did not run)");
            return;
        }
        using I = server::Intent;
        auto d = server::load_server_data(**contracts::BundleRegistry::find("java-1.21.8"), {jar, {}});
        require(d.has_value() && d->recipe_book, d ? "" : d.error());
        GameFixture f(offline_config(), std::move(*d));
        auto peer = f.join(1, "Reader");
        const contracts::PlayerKey key{1, 1};
        const auto& codec = f.wire.components();
        // A knowledge book's components: its recipes as a list of ids.
        const auto book_of = [&](std::vector<std::string> ids) {
            auto list = protocol::nbt::Tag::list(protocol::nbt::TagType::string);
            for (auto& id : ids) list.push(protocol::nbt::Tag::string(std::move(id)));
            Writer w;
            protocol::nbt::write_network(w, list);
            const auto type = *codec.type_id("recipes");
            auto value = codec.canonical_value(type, w.bytes(), f.wire.component_context());
            require(value.has_value(), value ? "" : value.error());
            return contracts::ComponentPatch::make(772, {{type, std::move(*value)}}, {}, {});
        };
        const auto give = [&](std::vector<std::string> ids) {
            f.game->submit(1, I{server::intent::CreativeSlot{36, 1, content().item_id("minecraft:knowledge_book").value(), 0, book_of(std::move(ids))}});
        };
        // What the rules know of the player's book (its crafting recipes).
        const auto knows = [&](std::string_view id) {
            const auto& c = f.data.game->crafting;
            const auto index = static_cast<std::uint32_t>(std::ranges::find(c, id, &gameplay::recipes::Recipe::id) - c.begin());
            const auto inv = f.game->engine().view().player(key).value();
            return inv && std::ranges::binary_search(inv->known_crafting, index);
        };
        f.game->submit(1, I{server::intent::Command{"recipe take Reader *"}});
        f.game->tick();
        give({"minecraft:no_such_recipe", "minecraft:chest"});
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->tick();
        f.game->submit(1, I{server::intent::Use{0, 1, 0.0f, 0.0f}});
        f.game->tick();
        f.game->tick();
        const auto held = [&] {
            const auto inv = f.game->engine().view().player(key).value();
            return inv ? inv->slots[36] : contracts::ItemStack{};
        };
        require(!knows("minecraft:chest") && held().empty(), "a recipe that does not exist: none given, the book used up");
        f.game->submit(1, I{server::intent::Command{"gamemode creative"}});
        give({"minecraft:stick", "minecraft:chest"});
        f.game->tick();
        f.game->submit(1, I{server::intent::Use{0, 2, 0.0f, 0.0f}});
        f.game->tick();
        f.game->tick();
        require(knows("minecraft:stick") && knows("minecraft:chest") && !held().empty(), "creative: given, the book kept");
    });

    suite.run("composters take held items and the players nearby get the level event", [] {
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Clerk");
        f.game->submit(1, I{server::intent::Command{"setblock 2 -60 1 minecraft:composter[level=0]"}});
        f.hold(1, "minecraft:pumpkin_pie");
        f.game->tick();
        std::size_t seen = 0;
        // Level events since the last call: (event, x, y, z, data).
        const auto events = [&] {
            const auto id = f.wire.ids().id(State::play, Bound::clientbound, "level_event");
            std::vector<std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t>> out;
            std::lock_guard lock(peer->mutex);
            for (; seen < peer->packets.size(); ++seen) {
                auto pk = protocol::open_packet(peer->packets[seen]);
                if (!pk || pk->id != id) continue;
                const auto event = pk->fields.i32().value();
                const auto pos = pk->fields.position().value();
                out.emplace_back(event, pos.x, pos.y, pos.z, pk->fields.i32().value());
            }
            return out;
        };
        (void)events();
        f.game->submit(1, GameFixture::click({2, -60, 1}, 1));
        f.game->tick();
        using Event = std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t>;
        require(events() == std::vector<Event>{{1500, 2, -60, 1, 1}} && f.at({2, -60, 1}) == "minecraft:composter[level=1]",
                "a layer added, and the event with 1");
    });

    suite.run("survival movement is replayed against block shapes", [] {
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Walker");
        // A two-block wall east of the spawn, a repeater to the south.
        f.hold(1, "minecraft:stone");
        f.game->submit(1, GameFixture::click({2, -61, 0}, 1));
        f.game->tick();
        f.game->submit(1, GameFixture::click({2, -60, 0}, 2));
        f.hold(1, "minecraft:repeater");
        f.game->submit(1, GameFixture::click({0, -61, 2}, 3));
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
        f.game->tick();
        require(f.at({2, -59, 0}) == "minecraft:stone" && f.at({0, -60, 2}).starts_with("minecraft:repeater"), "built");
        std::size_t seen = 0;
        // Teleports sent since the last call, as (id, x, y, z).
        const auto corrections = [&] {
            const auto id = f.wire.ids().id(State::play, Bound::clientbound, "player_position");
            std::vector<std::array<double, 4>> out;
            std::lock_guard lock(peer->mutex);
            for (; seen < peer->packets.size(); ++seen)
                if (auto pk = protocol::open_packet(peer->packets[seen]); pk && pk->id == id) {
                    const auto t = pk->fields.varint().value();
                    const auto x = pk->fields.f64().value(), y = pk->fields.f64().value(), z = pk->fields.f64().value();
                    out.push_back({static_cast<double>(t), x, y, z});
                }
            return out;
        };
        (void)corrections();
        const auto move = [&](double x, double y, double z, bool ground = true) {
            f.game->submit(1, I{server::intent::Move{server::Vec3{x, y, z}, std::nullopt, ground}});
            f.game->tick();
        };
        move(1.0, -60.0, 0.5);
        require(corrections().empty(), "walking is accepted");
        // Walking into the wall: the box stops at x = 1.7, 0.8 short of the claim.
        move(2.5, -60.0, 0.5);
        auto c = corrections();
        require(c.size() == 1 && c[0][1] == 1.0 && c[0][2] == -60.0, "moved wrongly: back to the last position");
        f.game->submit(1, I{server::intent::ConfirmTeleport{static_cast<std::int32_t>(c[0][0])}});
        // Up to the wall's face is fine; stepping onto a repeater (1/8 high) is too.
        move(1.65, -60.0, 0.5);
        move(0.5, -60.0, 1.5);
        move(0.5, -59.875, 2.5);
        require(corrections().empty(), "steps up 1/8 of a block");
        // A full block is too high to step onto; a move that rises first (a
        // jump) gets on top, since vertical movement is applied first and is
        // not compared, as in the target version.
        move(0.5, -59.875, 1.5);
        move(1.65, -60.0, 0.5);
        require(corrections().empty(), "back at the wall");
        move(2.1, -60.0, 0.5);
        c = corrections();
        require(c.size() == 1 && c[0][1] == 1.65, "no stepping onto a full block");
        f.game->submit(1, I{server::intent::ConfirmTeleport{static_cast<std::int32_t>(c[0][0])}});
        move(2.5, -58.0, 0.5);
        require(corrections().empty(), "a jump onto the wall is accepted");
        // Creative players may move anywhere free, but not into a block.
        f.game->submit(1, I{server::intent::Command{"gamemode creative"}});
        move(1.5, -55.0, 0.5, false);
        require(corrections().empty(), "creative players fly");
        move(2.5, -59.5, 0.5, false);
        c = corrections();
        require(c.size() == 1 && c[0][1] == 1.5 && c[0][2] == -55.0, "not into the wall");
    });

    suite.run("survival players float, fall, die and respawn", [] {
        using I = server::Intent;
        auto config = offline_config();
        GameFixture f(config);
        auto peer = f.join(1, "Faller");
        const contracts::PlayerKey key{1, 1};
        f.game->submit(1, I{server::intent::CreativeSlot{36, 10, content().item_id("minecraft:redstone").value()}});
        f.game->submit(1, I{server::intent::CreativeSlot{9, 1, content().item_id("minecraft:iron_pickaxe").value(), 7}});
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
        f.game->tick();
        std::size_t seen = 0;
        // Health updates and death screens sent since the last call.
        const auto vitals = [&] {
            const auto health = f.wire.ids().id(State::play, Bound::clientbound, "set_health");
            const auto kill = f.wire.ids().id(State::play, Bound::clientbound, "player_combat_kill");
            const auto respawn = f.wire.ids().id(State::play, Bound::clientbound, "respawn");
            std::vector<std::pair<std::string, float>> out;
            std::lock_guard lock(peer->mutex);
            for (; seen < peer->packets.size(); ++seen) {
                auto pk = protocol::open_packet(peer->packets[seen]);
                if (!pk) continue;
                if (pk->id == health) out.emplace_back("health", pk->fields.f32().value());
                if (pk->id == kill) out.emplace_back("death", 0.0f);
                if (pk->id == respawn) out.emplace_back("respawn", 0.0f);
            }
            return out;
        };
        const auto move = [&](double y, bool ground) {
            f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, y, 0.5}, std::nullopt, ground}});
        };
        // Falling 9 blocks: 6 damage on landing. As in the target version, the
        // distance is what the player descended before the move that lands.
        const auto land = [&] { f.game->submit(1, I{server::intent::Move{std::nullopt, std::nullopt, true}}); };
        move(-51.0, false);
        for (double y = -52.0; y >= -60.0; y -= 1.0) move(y, false);
        land();
        f.game->tick();
        auto v = vitals();
        require(!v.empty() && v.back() == std::pair<std::string, float>{"health", 14.0f}, "six points of fall damage");
        // With full food, health comes back by saturation/6 every 10 ticks.
        for (int i = 0; i < 10; ++i) f.game->tick();
        v = vitals();
        require(!v.empty() && std::abs(v.back().second - (14.0f + 5.0f / 6.0f)) < 1e-4f, "regeneration");
        // Hovering 81 ticks gets a survival player kicked; it does not end the test's session here,
        // because the fixture's peer records the kick.
        move(-55.0, false);
        for (int i = 0; i < 81; ++i) {
            f.game->submit(1, I{server::intent::Move{std::nullopt, std::nullopt, false}});
            f.game->tick();
        }
        {
            std::lock_guard lock(peer->mutex);
            require(peer->kicks.size() == 1 && peer->kicks[0] == "translate:multiplayer.disconnect.flying", "floating kick");
        }
        // Falling 25 blocks is deadly: the inventory drops around the player.
        move(-46.0, false);
        move(-37.0, false);
        move(-35.0, false);
        for (double y = -36.0; y >= -60.0; y -= 1.0) move(y, false);
        land();
        f.game->tick();
        f.game->tick();
        v = vitals();
        require(std::ranges::any_of(v, [](const auto& e) { return e.first == "death"; }), "the death screen");
        const auto d = f.game->engine().dump();
        require(d.players.empty() || std::ranges::all_of(d.players[0].slots, [](const auto& st) { return st.empty(); }), "inventory dropped");
        std::map<std::uint16_t, int> dropped;
        for (const auto& e : d.overworld().entities) dropped[e.stack.item] += e.stack.count;
        require(dropped[gameplay::items::redstone] == 10 && dropped[gameplay::items::iron_pickaxe] == 1, "every item dropped");
        // Dead players do not move or act; asking to respawn brings them back to the spawn.
        move(-59.0, true);
        f.game->tick();
        f.game->submit(1, I{server::intent::ClientCommand{0}});
        f.game->tick();
        v = vitals();
        require(std::ranges::any_of(v, [](const auto& e) { return e.first == "respawn"; }) && v.back() == std::pair<std::string, float>{"health", 20.0f},
                "respawned with full health");
        (void)key;
    });

    suite.run("players below the world take void damage until they die", [] {
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Diver");
        f.game->submit(1, I{server::intent::CreativeSlot{36, 3, content().item_id("minecraft:stone").value()}});
        f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
        f.game->submit(1, I{server::intent::Command{"tp 0.5 -140 0.5"}});
        f.game->tick();
        f.game->submit(1, I{server::intent::ConfirmTeleport{2}});
        // Creative players are not spared: four points every ten ticks, against
        // regeneration while saturation lasts.
        int ticks = 0;
        const auto kill = f.wire.ids().id(State::play, Bound::clientbound, "player_combat_kill");
        const auto died = [&] {
            std::lock_guard lock(peer->mutex);
            return std::ranges::any_of(peer->packets, [&](const auto& b) {
                auto pk = protocol::open_packet(b);
                return pk && pk->id == kill;
            });
        };
        while (!died() && ticks < 200) {
            f.game->tick();
            ++ticks;
        }
        require(died() && ticks > 40 && ticks < 80, std::format("died after {} ticks", ticks));
        const auto d = f.game->engine().dump();
        require(d.overworld().entities.empty() && (d.players.empty() || d.players[0].slots[36].empty()), "the items are lost below the world");
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

    suite.run("a button and a redstone torch light lamps through the engine", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.join("Presser");
        confirm_spawn(c);
        use_item_on(c, "minecraft:redstone_lamp", {2, -61, 1}, 1);
        require(block_update(c, {2, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=false]"), "lamp placed");
        use_item_on(c, "minecraft:stone_button", {1, -61, 1}, 2);
        const auto button = block_update(c, {1, -60, 1});
        require(button.has_value() && content().notation(*button).value_or("").starts_with("minecraft:stone_button[face=floor,"),
                "a floor button placed");
        c.send(State::play, "use_item_on",
               Writer().varint(0).position({1, -60, 1}).varint(1).f32(0.5f).f32(0.1f).f32(0.5f).boolean(false).boolean(false).varint(3));
        require(block_update(c, {2, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=true]"), "pressed, the lamp lights");
        // Released after 20 ticks, the lamp goes out 4 ticks later.
        require(block_update(c, {2, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=false]"), "released, the lamp goes out");
        use_item_on(c, "minecraft:redstone_lamp", {5, -61, 1}, 4);
        require(block_update(c, {5, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=false]"), "second lamp placed");
        use_item_on(c, "minecraft:redstone_torch", {4, -61, 1}, 5);
        require(block_update(c, {4, -60, 1}) == content().state_id("minecraft:redstone_torch[lit=true]"), "a floor torch placed");
        require(block_update(c, {5, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=true]"), "the torch lights the lamp");
        s.stop();
    });

    suite.run("TNT placed beside power is primed and shown to players", [] {
        server::Server s(offline_config());
        s.start();
        TestClient c(s.port());
        c.join("Igniter");
        confirm_spawn(c);
        use_item_on(c, "minecraft:redstone_block", {2, -61, 1}, 1);
        require(block_update(c, {2, -60, 1}) == content().state_id("minecraft:redstone_block"), "redstone block placed");
        use_item_on(c, "minecraft:tnt", {3, -61, 1}, 2);
        auto spawn = c.until(State::play, "add_entity");
        require(spawn.has_value(), "primed TNT spawned");
        auto r = spawn->reader();
        const auto id = r.varint().value();
        (void)r.uuid().value();
        require(r.varint().value() == content().entity_type("minecraft:tnt").value(), "entity type");
        const auto x = r.f64().value(), y = r.f64().value(), z = r.f64().value();
        // Entities are shown as they stand after the tick: launched from the
        // block's bottom center by at most 0.02 sideways and 0.16 up.
        require(std::abs(x - 3.5) < 0.03 && y > -60.0 && y < -59.8 && std::abs(z - 1.5) < 0.03,
                std::format("from the block's bottom center: {} {} {}", x, y, z));
        auto data = c.until(State::play, "set_entity_data");
        require(data.has_value(), "entity data");
        auto d = data->reader();
        require(d.varint().value() == id && d.u8().value() == 8 && d.varint().value() == 1 && d.varint().value() == 79
                    && d.u8().value() == 0xFF,
                "the fuse (index 8, a VarInt) is 79 after the first tick");
        // 79 ticks later it explodes: players nearby get the explosion, a
        // sixteenth of the TNT's height above its feet.
        auto boom = c.until(State::play, "explode", std::chrono::milliseconds(15000));
        require(boom.has_value(), "explosion sent");
        auto e = boom->reader();
        const auto ex = e.f64().value(), ey = e.f64().value(), ez = e.f64().value();
        require(std::abs(ex - 3.5) < 0.3 && std::abs(ey - (-60.0 + static_cast<double>(0.98f * 0.0625f))) < 1e-9 && std::abs(ez - 1.5) < 0.3,
                std::format("explosion center {} {} {}", ex, ey, ez));
        require(!e.boolean().value() && e.varint().value() == 21 && e.varint().value() == 616, "no knockback, explosion_emitter, the explode sound");
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

    suite.run("synchronized registries and game rules match the official servers", [] {
        // The tables are compiled from public lists; the official servers'
        // configuration and command trees (tests/fixtures/registries) check them.
        for (const auto* id : {"java-1.21.8", "java-1.21.4"}) {
            const auto& bundle = **contracts::BundleRegistry::find(id);
            std::map<std::string, std::set<std::string>> observed;
            std::set<std::string> rules;
            std::ifstream in(std::format("{}/tests/fixtures/registries/{}.txt", CIRCUIT_SOURCE_DIR, id));
            for (std::string line; std::getline(in, line);) {
                std::istringstream words(line);
                std::string kind, name, entry;
                words >> kind;
                if (kind == "registry") {
                    words >> name;
                    auto& set = observed[name];
                    while (words >> entry) set.insert(entry);
                } else if (kind == "game_rules") {
                    while (words >> entry) rules.insert(entry);
                }
            }
            require(!observed.empty() && !rules.empty(), "observations read");
            const auto* registries = server::synced_registries(bundle.schema.registries);
            std::set<std::string> sent;
            for (const auto& r : *registries) {
                const std::set<std::string> entries(r.entries.begin(), r.entries.end());
                require(observed.contains(std::string(r.registry)) && observed.at(std::string(r.registry)) == entries,
                        std::format("{}: {} matches the official server", id, r.registry));
                sent.insert(std::string(r.registry));
            }
            // Registries the official server sends that Circuit does not, yet.
            std::set<std::string> missing;
            for (const auto& [name, entries] : observed)
                if (!sent.contains(name)) missing.insert(name);
            const std::set<std::string> expected_missing = std::string_view(id) == "java-1.21.8"
                ? std::set<std::string>{"minecraft:dialog", "minecraft:enchantment", "minecraft:test_environment", "minecraft:test_instance"}
                : std::set<std::string>{"minecraft:enchantment"};
            require(missing == expected_missing, std::format("{}: {} registries not sent", id, missing.size()));
            std::set<std::string> names;
            for (const auto& r : bundle.schema.game_rules)
                if (!r.experimental) names.insert(std::string(r.name));
            require(names == rules, std::format("{}: {} game rules, {} observed", id, names.size(), rules.size()));
        }
    });

    suite.run("every block state of both protocols has a palette entry", [] {
        // (Every state's collision boxes: evidence_tests, against the official servers.)
        for (const auto& [protocol, version] : {std::pair{772, "1.21.8"}, std::pair{769, "1.21.4"}}) {
            const auto& tables = *protocol::ContentTables::for_protocol(protocol);
            const auto& mapping = *evidence::ContentMapping::for_game_version(version);
            const server::BlockPalette palette(tables, mapping);
            for (std::int32_t id = 0; id < tables.state_count(); ++id) {
                const auto s = palette.reference(id);
                require(s && palette.wire(*s) == id, std::format("{}: state {} maps both ways", version, id));
                require(mapping.block(*tables.notation(id)) == *s, std::format("{}: state {} is {}", version, id, *tables.notation(id)));
            }
        }
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
        // The next tick's light update carries the lamp's block light.
        const auto block_light_at = [&](protocol::WirePosition at) -> std::optional<int> {
            const auto want = c.id(State::play, Bound::clientbound, "light_update");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                auto p = c.next(std::chrono::milliseconds(500));
                if (!p || p->id != want) continue;
                auto r = p->reader();
                const auto x = r.varint().value(), z = r.varint().value();
                if (x != (at.x >> 4) || z != (at.z >> 4)) continue;
                std::array<std::vector<std::uint64_t>, 4> masks;
                for (auto& m : masks) {
                    const auto n = r.varint().value();
                    for (int i = 0; i < n; ++i) m.push_back(static_cast<std::uint64_t>(r.i64().value()));
                }
                const auto has = [](const std::vector<std::uint64_t>& m, int i) {
                    return static_cast<std::size_t>(i / 64) < m.size() && ((m[static_cast<std::size_t>(i / 64)] >> (i % 64)) & 1);
                };
                const int index = ((at.y + 64) >> 4) + 1; // light sections start one below the world
                std::array<std::vector<Bytes>, 2> arrays;
                for (auto& a : arrays) {
                    const auto n = r.varint().value();
                    for (int i = 0; i < n; ++i) a.push_back(r.prefixed_bytes(2048).value());
                }
                if (!has(masks[1], index)) continue;
                int k = 0;
                for (int i = 0; i < index; ++i) k += has(masks[1], i) ? 1 : 0;
                const auto& nibbles = arrays[1].at(static_cast<std::size_t>(k));
                const int cell = ((at.y + 64) & 15) << 8 | (at.z & 15) << 4 | (at.x & 15);
                const auto byte = static_cast<std::uint8_t>(nibbles.at(static_cast<std::size_t>(cell / 2)));
                return cell % 2 == 0 ? byte & 15 : byte >> 4;
            }
            return std::nullopt;
        };
        require(block_light_at({2, -60, 1}) == 15, "the lit lamp's light reaches the client");
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
            // A player's packets are handled in order: once a later command
            // is answered, the move has been taken.
            c.send(State::play, "chat_command", Writer().string("time query gametime"));
            require(c.until(State::play, "system_chat").has_value(), "the command after the move answered");
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

    suite.run("recipe books are saved with the players", [] {
        // The recipe book needs the official data packs (CIRCUIT_VANILLA_JAR_1_21_8).
        const char* jar = std::getenv("CIRCUIT_VANILLA_JAR_1_21_8");
        if (!jar || !*jar) {
            std::puts("  (no official 1.21.8 server jar given: the saved recipe book checks did not run)");
            return;
        }
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        config.vanilla_server_jar = jar;
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Reader");
            confirm_spawn(c);
            // The blast furnace's book opened; the crafting table came on the first tick.
            c.send(State::play, "chat_command", Writer().string("time query gametime"));
            require(c.until(State::play, "system_chat").has_value(), "joined");
            c.send(State::play, "recipe_book_change_settings", Writer().varint(2).boolean(true).boolean(false));
            c.send(State::play, "chat_command", Writer().string("time query gametime"));
            require(c.until(State::play, "system_chat").has_value(), "the command after the settings answered");
            s.stop();
        }
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Reader");
            const auto settings = c.until(State::play, "recipe_book_settings");
            require(settings.has_value(), "settings sent");
            auto r = settings->reader();
            std::vector<bool> books;
            for (int k = 0; k < 8; ++k) books.push_back(r.boolean().value());
            require((books == std::vector<bool>{false, false, false, false, true, false, false, false}), "the blast furnace's book open");
            const auto book = c.until(State::play, "recipe_book_add");
            require(book.has_value(), "the book sent");
            auto b = book->reader();
            const auto n = b.varint().value();
            const auto first = b.varint().value();
            require(n == 1 && first == 262 && book->fields.back() == std::byte{1} && book->fields[book->fields.size() - 2] == std::byte{2},
                    "the crafting table, still highlighted, replacing the book");
            s.stop();
        }
        std::filesystem::remove_all(dir);
    });

    suite.run("players' attributes go to them and to those who see them as both official servers send them", [] {
        // circuit-clienttest/attributes.js, attributes-order.js and
        // attributes-join.js: both versions alike but for the attribute ids
        // (here 1.21.8's) and fall damage's rounding; the official servers
        // list a packet's attributes in an order that varies from run to
        // run, here in id order.
        using I = server::Intent;
        GameFixture f;
        // Since the last call, as text: attribute packets "attributes
        // <entity> <id> <base> [<modifier> <amount> <operation>]... | ...",
        // "absorption <entity> <amount>" (entity data 15), "health
        // <health>" and "spawn <entity>".
        std::map<std::uint64_t, std::size_t> seen;
        const auto packets = [&](Recorder& r) {
            const auto attributes = f.wire.ids().id(State::play, Bound::clientbound, "update_attributes");
            const auto data = f.wire.ids().id(State::play, Bound::clientbound, "set_entity_data");
            const auto health = f.wire.ids().id(State::play, Bound::clientbound, "set_health");
            const auto spawn = f.wire.ids().id(State::play, Bound::clientbound, "add_entity");
            std::vector<std::string> out;
            std::lock_guard lock(r.mutex);
            for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                auto pk = protocol::open_packet(r.packets[at]);
                if (!pk) continue;
                auto& in = pk->fields;
                if (pk->id == attributes) {
                    std::string text = std::format("attributes {}", in.varint().value());
                    const auto n = in.varint().value();
                    for (std::int32_t k = 0; k < n; ++k) {
                        const auto id = in.varint().value();
                        text += std::format("{} {} {}", k ? " |" : "", id, in.f64().value());
                        const auto modifiers = in.varint().value();
                        for (std::int32_t m = 0; m < modifiers; ++m) {
                            const auto name = in.string(32767).value();
                            const auto amount = in.f64().value();
                            text += std::format(" {} {} {}", name, amount, in.u8().value());
                        }
                    }
                    out.push_back(std::move(text));
                } else if (pk->id == data) {
                    // The effects' entries are passed over (shared flags, particles, ambience).
                    const auto entity = in.varint().value();
                    for (auto index = in.u8().value(); index != 0xFF; index = in.u8().value()) {
                        const auto type = in.varint().value();
                        if (type == 0) (void)in.u8();
                        else if (type == 8) (void)in.boolean();
                        else if (type == 3) out.push_back(std::format("absorption {} {}", entity, in.f32().value()));
                        else if (type == 18) {
                            const auto count = in.varint().value();
                            for (std::int32_t k = 0; k < count; ++k)
                                if (in.varint().value() == 20) (void)in.i32();
                        }
                    }
                } else if (pk->id == health) {
                    out.push_back(std::format("health {}", in.f32().value()));
                } else if (pk->id == spawn) {
                    out.push_back(std::format("spawn {}", in.varint().value()));
                }
            }
            return out;
        };
        const auto only = [](const std::string& line) { return std::vector<std::string>{line}; };
        const auto command = [&](std::string text) {
            f.game->submit(1, I{server::intent::Command{std::move(text)}});
            f.game->tick();
        };
        const auto then = [&] { f.game->tick(); };
        // A first join: after the health, the movement speed and both
        // reaches, with creative mode's modifiers (players join in creative).
        auto alice = f.join(1, "Alice");
        auto a = packets(*alice);
        const auto first = std::ranges::find_if(a, [](const auto& t) { return t.starts_with("attributes "); });
        require(first != a.end() && std::ranges::find(a, std::string("health 20")) < first, "the attributes after the health");
        auto entity = first->substr(11, first->find(' ', 11) - 11);
        const auto creative = [&](const std::string& e) {
            return std::format("attributes {} 6 4.5 minecraft:creative_mode_block_range 0.5 0 | 10 3 minecraft:creative_mode_entity_range 2 0 | "
                               "22 0.10000000149011612", e);
        };
        require(*first == creative(entity), "a first join's: " + *first);
        // Bob sees Alice appear with her attributes; Alice sees him with his
        // once (the first join's attributes, after he appeared).
        auto bob = f.join(2, "Bob");
        for (int i = 0; i < 3; ++i) then();
        auto b = packets(*bob);
        const auto spawned = std::ranges::find(b, std::format("spawn {}", entity));
        require(spawned != b.end() && spawned + 1 != b.end() && *(spawned + 1) == creative(entity), "bob sees alice's attributes");
        a = packets(*alice);
        require(a.size() == 2 && a[0].starts_with("spawn ") && a[1] == creative(a[0].substr(6)), "alice sees bob's once");
        // Game modes: creative's reach goes, with the next tick's entity data.
        command("gamemode survival");
        require(packets(*alice).empty(), "not in the command's tick");
        then();
        const auto survival = std::format("attributes {} 6 4.5 | 10 3", entity);
        require(packets(*alice) == only(survival) && packets(*bob) == only(survival), "creative mode's reach removed");
        // Effects: a modifier added; added again (at the end) when the level
        // is lengthened or changes; nothing when refused.
        const auto speed = [&](std::string mods) { return std::format("attributes {} 22 0.10000000149011612{}", entity, mods); };
        const auto speed1 = std::string(" minecraft:effect.speed 0.4000000059604645 2"), sprint = std::string(" minecraft:sprinting 0.30000001192092896 2");
        command("effect give Alice minecraft:speed 30 1");
        then();
        require(packets(*alice) == only(speed(speed1)) && packets(*bob) == only(speed(speed1)), "speed's modifier");
        command("effect give Alice minecraft:speed 40 1");
        then();
        require(packets(*alice) == only(speed(speed1)), "lengthened: sent again");
        command("effect give Alice minecraft:speed 10 1");
        then();
        require(packets(*alice).empty(), "refused: nothing");
        f.game->submit(1, I{server::intent::Sprint{true}});
        then();
        then();
        require(packets(*alice) == only(speed(speed1 + sprint)), "sprinting");
        command("effect give Alice minecraft:speed 30 2");
        then();
        require(packets(*alice) == only(speed(sprint + " minecraft:effect.speed 0.6000000089406967 2")), "a stronger speed moves to the end");
        f.game->submit(1, I{server::intent::Sprint{false}});
        then();
        then();
        require(packets(*alice) == only(speed(" minecraft:effect.speed 0.6000000089406967 2")), "sprinting stopped");
        command("effect clear Alice");
        then();
        require(packets(*alice) == only(speed("")), "cleared");
        b = packets(*bob);
        require(b.size() == 5 && b.back() == speed(""), "bob saw each change since: lengthened, sprinting, stronger, not sprinting, cleared");
        // Health boost: the health regenerates to the boosted maximum.
        command("effect give Alice minecraft:health_boost 120 0");
        then();
        require(packets(*alice) == only(std::format("attributes {} 19 20 minecraft:effect.health_boost 4 0", entity)), "health boost");
        for (int i = 0; i < 400; ++i) then();
        a = packets(*alice);
        require(!a.empty() && a.back() == "health 24", "regenerated to 24: " + (a.empty() ? "" : a.back()));
        // A rejoin: every attribute before the health (24 kept); those who
        // see the player get them twice.
        f.game->leave(1);
        then();
        (void)packets(*bob);
        alice = f.join(1, "Alice");
        seen[1] = 0;
        for (int i = 0; i < 3; ++i) then();
        a = packets(*alice);
        const auto again = std::ranges::find_if(a, [](const auto& t) { return t.starts_with("attributes "); });
        require(again != a.end(), "attributes on rejoining");
        entity = again->substr(11, again->find(' ', 11) - 11);
        const auto all = std::format("attributes {} 6 4.5 | 10 3 | 19 20 minecraft:effect.health_boost 4 0 | 22 0.10000000149011612", entity);
        require(*again == all && again + 1 != a.end() && *(again + 1) == "health 24", "all of them, then the health: " + *again);
        b = packets(*bob);
        require(std::ranges::count(b, all) == 2 && std::ranges::find(b, std::format("spawn {}", entity)) < std::ranges::find(b, all),
                "bob gets them twice after alice appears");
        // Clearing the boost cuts the health, after the attributes.
        command("effect clear Alice minecraft:health_boost");
        then();
        require(packets(*alice) == std::vector<std::string>{std::format("attributes {} 19 20", entity), "health 20"}, "the health cut to 20");
        // Absorption: given with its maximum; a fall takes it first.
        command("effect give Alice minecraft:absorption 60 1");
        then();
        const auto absorbing = std::format("attributes {} 18 0 minecraft:effect.absorption 8 0", entity);
        require(packets(*alice) == std::vector<std::string>{std::format("absorption {} 8", entity), absorbing}, "absorption 8");
        f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
        const auto move = [&](double y, bool ground) { f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, y, 0.5}, std::nullopt, ground}}); };
        const auto fall = [&](double height) {
            move(-60.0 + std::min(height, 9.0), false);
            if (height > 9.0) move(-60.0 + height, false);
            for (double y = -60.0 + height - 1.0; y > -60.0; y -= 1.0) move(y, false);
            move(-60.0, false);
            move(-60.0, true);
            then();
            then();
        };
        fall(10.0);
        require(packets(*alice) == only(std::format("absorption {} 1", entity)), "seven points: absorption 1 left, the health untouched");
        command("effect give Alice minecraft:absorption 80 1");
        then();
        require(packets(*alice) == std::vector<std::string>{std::format("absorption {} 8", entity), absorbing}, "lengthened: back to 8");
        command("effect clear Alice minecraft:absorption");
        then();
        require(packets(*alice) == std::vector<std::string>{std::format("absorption {} 0", entity), std::format("attributes {} 18 0", entity)},
                "cleared");
        // Falls: beyond the safe fall distance, rounded down on 1.21.8; jump
        // boost adds to it. (Each after the hurt cooldown's first half.)
        for (int i = 0; i < 20; ++i) then();
        fall(5.5);
        require(packets(*alice) == only("health 18"), "5.5 blocks: 2");
        command("effect give Alice minecraft:jump_boost 60 1");
        then();
        require(packets(*alice) == only(std::format("attributes {} 24 3 minecraft:effect.jump_boost 2 0", entity)), "jump boost");
        for (int i = 0; i < 20; ++i) then();
        fall(10.0);
        require(packets(*alice) == only("health 13"), "10 blocks with jump boost 1: 5");
        // A respawn: every attribute, without modifiers, before the health;
        // bob appears to alice again, and bob sees her with them twice.
        command("kill Alice");
        (void)packets(*bob);
        f.game->submit(1, I{server::intent::ClientCommand{0}});
        for (int i = 0; i < 3; ++i) then();
        a = packets(*alice);
        const auto bare = std::format("attributes {} 6 4.5 | 10 3 | 18 0 | 19 20 | 22 0.10000000149011612 | 24 3", entity);
        const auto respawned = std::ranges::find(a, bare);
        require(respawned != a.end() && respawned + 1 != a.end() && *(respawned + 1) == "health 20", "respawned: all of them, then the health");
        require(std::ranges::any_of(a, [](const auto& t) { return t.starts_with("spawn "); }), "bob appears again");
        b = packets(*bob);
        require(std::ranges::count(b, bare) == 2, "bob gets them twice");
        // A creative rejoin with absorption: the absorption, then every
        // attribute (without creative mode's), the health, then creative
        // mode's reach; bob sees the absorption and the attributes twice,
        // then the reach.
        command("gamemode creative");
        then();
        (void)packets(*alice);
        command("effect give Alice minecraft:absorption 60 1");
        then();
        (void)packets(*alice);
        f.game->leave(1);
        then();
        (void)packets(*bob);
        alice = f.join(1, "Alice");
        seen[1] = 0;
        for (int i = 0; i < 3; ++i) then();
        a = packets(*alice);
        const auto told = std::ranges::find_if(a, [](const auto& t) { return t.starts_with("absorption "); });
        require(told != a.end(), "absorption on rejoining");
        entity = told->substr(11, told->find(' ', 11) - 11);
        const auto kept = std::format("attributes {} 6 4.5 | 10 3 | 18 0 minecraft:effect.absorption 8 0 | 19 20 | 22 0.10000000149011612 | 24 3", entity);
        const auto reach = std::format("attributes {} 6 4.5 minecraft:creative_mode_block_range 0.5 0 | 10 3 minecraft:creative_mode_entity_range 2 0", entity);
        require(*told == std::format("absorption {} 8", entity) && told + 2 < a.end() && *(told + 1) == kept && *(told + 2) == "health 20",
                "the absorption, every attribute, the health");
        require(std::ranges::find(told + 3, a.end(), reach) != a.end(), "creative mode's reach after the health");
        b = packets(*bob);
        const auto appear = std::ranges::find(b, std::format("spawn {}", entity));
        require(appear != b.end() && std::vector<std::string>(appear + 1, b.end())
                    == std::vector<std::string>{std::format("absorption {} 8", entity), kept, std::format("absorption {} 8", entity), kept, reach},
                "bob: the absorption and the attributes twice, then the reach");
    });

    suite.run("haste, conduit power and mining fatigue time digging as both official servers do", [] {
        // circuit-clienttest/dig-effects.js, both versions identical: oak planks by
        // hand take 60 ticks; a client may finish at 70%.
        using I = server::Intent;
        GameFixture f;
        auto peer = f.join(1, "Digger");
        f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
        f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
        f.game->submit(1, I{server::intent::Move{std::nullopt, std::nullopt, true}}); // on the ground
        f.game->tick();
        std::int32_t seq = 1;
        // Digs at x 2, z `z`, finished after `ticks`; returns the ticks after finishing until it broke (-1: not within `wait`).
        const auto dig = [&](std::int32_t z, const std::vector<std::string>& effects, int ticks, int wait) {
            f.game->submit(1, I{server::intent::Command{std::format("setblock 2 -60 {} minecraft:oak_planks", z)}});
            f.game->submit(1, I{server::intent::Command{"effect clear Digger"}});
            for (const auto& e : effects) f.game->submit(1, I{server::intent::Command{"effect give Digger " + e}});
            f.game->tick();
            f.game->submit(1, I{server::intent::Dig{0, {2, -60, z}, 1, seq++}});
            f.game->tick();
            for (int i = 1; i < ticks; ++i) f.game->tick();
            f.game->submit(1, I{server::intent::Dig{2, {2, -60, z}, 1, seq++}});
            for (int i = 1; i <= wait; ++i) {
                f.game->tick();
                if (f.at({2, -60, z}) == "minecraft:air") return i;
            }
            return -1;
        };
        require(dig(-3, {}, 34, 40) > 20, "no effect, finished after 34 ticks: later");
        require(dig(-2, {}, 50, 5) == 1, "no effect, finished after 50 ticks: at once");
        require(dig(-1, {"minecraft:haste 60 1"}, 34, 5) == 1, "haste 1: at once");
        require(dig(1, {"minecraft:haste 60 0"}, 38, 5) == 1, "haste 0: at once");
        require(dig(2, {"minecraft:conduit_power 60 1"}, 34, 5) == 1, "conduit power 1: at once");
        const auto fatigued = dig(3, {"minecraft:mining_fatigue 60 0"}, 50, 200);
        require(fatigued > 140 && fatigued < 160, std::format("mining fatigue 0: about 200 ticks from the start ({} after finishing)", fatigued));
        const auto both = dig(4, {"minecraft:haste 60 1", "minecraft:mining_fatigue 60 0"}, 50, 150);
        require(both > 85 && both < 100, std::format("haste 1 and fatigue 0: about 142 ticks from the start ({} after finishing)", both));
        (void)peer;
    });

    suite.run("falls count the move that lands on 1.21.8 and are rounded as each version does", [] {
        // circuit-clienttest/falls.js and attributes-order.js: 1.21.8 counts the
        // landing move and rounds down, 1.21.4 leaves it out and rounds up.
        using I = server::Intent;
        for (const auto& [version, expected] : {std::pair{"1.21.8", std::array{18.0f, 15.0f, 9.0f, 8.0f}}, std::pair{"1.21.4", std::array{17.0f, 14.0f, 8.0f, 7.0f}}}) {
            GameFixture f(version, offline_config());
            auto peer = f.join(1, "Faller");
            // Flying in creative, then survival: the abilities sent stop the flight, so the falls hurt.
            f.game->submit(1, I{server::intent::Flying{true}});
            f.game->tick();
            f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
            f.game->submit(1, I{server::intent::Command{"gamerule naturalRegeneration false"}}); // no healing between the falls
            f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
            f.game->tick();
            const auto move = [&](double y, bool ground) { f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, y, 0.5}, std::nullopt, ground}}); };
            const auto health = [&] {
                const auto id = f.wire.ids().id(State::play, Bound::clientbound, "set_health");
                float last = 0.0f;
                std::lock_guard lock(peer->mutex);
                for (const auto& bytes : peer->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == id) last = pk->fields.f32().value();
                return last;
            };
            // From `top` up, a block a move down to `last`, then the landing (apart, without height, or from `last`).
            const auto fall = [&](double top, double last, bool apart) {
                move(-60.0 + top, false);
                for (double y = top - 1.0; y > last + 1e-9; y -= 1.0) move(-60.0 + y, false);
                move(-60.0 + last, false);
                if (apart) move(-60.0, false);
                move(-60.0, true);
                for (int i = 0; i < 25; ++i) f.game->tick(); // past the hurt cooldown's first half
                return health();
            };
            auto h = fall(5.5, 0.5, true);
            require(h == expected[0], std::format("{}: 5.5 blocks, landing apart: {}", version, h));
            h = fall(6.3, 0.5, false);
            require(h == expected[1], std::format("{}: 6.3 blocks, landing 0.5 down: {}", version, h));
            h = fall(9.0, 0.416, false); // as a client falling from rest lands
            require(h == expected[2], std::format("{}: 9 blocks, landing 0.416 down: {}", version, h));
            h = fall(4.4, 0.4, false);
            require(h == expected[3], std::format("{}: 4.4 blocks, landing 0.4 down: {}", version, h));
            // The naturalRegeneration rule stopped regeneration; on again, it heals.
            for (int i = 0; i < 200; ++i) f.game->tick();
            require(health() == expected[3], std::format("{}: no regeneration while the rule is off", version));
            f.game->submit(1, I{server::intent::Command{"gamerule naturalRegeneration true"}});
            for (int i = 0; i < 20; ++i) f.game->tick();
            require(health() > expected[3], std::format("{}: regeneration with the rule on", version));
            // The attribute ids are each version's: block reach 6, entity reach 10 or 9, movement speed 22 or 21.
            const auto attributes = f.wire.ids().id(State::play, Bound::clientbound, "update_attributes");
            std::lock_guard lock(peer->mutex);
            bool found = false;
            const auto want = std::string_view(version) == "1.21.8" ? std::vector<std::int32_t>{6, 10, 22} : std::vector<std::int32_t>{6, 9, 21};
            for (const auto& bytes : peer->packets) {
                auto pk = protocol::open_packet(bytes);
                if (!pk || pk->id != attributes) continue;
                auto& in = pk->fields;
                (void)in.varint();
                const auto n = in.varint().value();
                std::vector<std::int32_t> ids;
                for (std::int32_t k = 0; k < n; ++k) {
                    ids.push_back(in.varint().value());
                    (void)in.f64();
                    for (auto m = in.varint().value(); m > 0; --m) {
                        (void)in.string(32767);
                        (void)in.f64();
                        (void)in.u8();
                    }
                }
                found = found || ids == want;
            }
            require(found, std::format("{}: the first join's attribute ids", version));
        }
    });

    suite.run("status effects are saved with the players", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Brewer");
            confirm_spawn(c);
            c.send(State::play, "chat_command", Writer().string("effect give @s minecraft:speed 210 0"));
            c.send(State::play, "chat_command", Writer().string("effect give @s minecraft:speed 3 2 true"));
            c.send(State::play, "chat_command", Writer().string("effect give @s minecraft:night_vision infinite 0"));
            c.send(State::play, "chat_command", Writer().string("effect give @s minecraft:health_boost 120 0"));
            c.send(State::play, "chat_command", Writer().string("effect give @s minecraft:absorption 120 1"));
            // Luck given and cleared: the player keeps the attribute, with no effect to bring it back.
            c.send(State::play, "chat_command", Writer().string("effect give @s minecraft:luck 120 0"));
            c.send(State::play, "chat_command", Writer().string("effect clear @s minecraft:luck"));
            // The sixth effect's packet, then luck's removal: all were given.
            for (int k = 0; k < 6; ++k) require(c.until(State::play, "update_mob_effect").has_value(), "given");
            require(c.until(State::play, "remove_mob_effect").has_value(), "luck cleared");
            s.stop();
        }
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Brewer");
            // Speed 2 with its particles hidden, then night vision; the
            // weaker speed was kept underneath and comes back.
            const auto first = c.until(State::play, "update_mob_effect");
            require(first.has_value(), "effects sent again");
            auto r = first->reader();
            (void)r.varint();
            const auto id = r.varint().value(), amplifier = r.varint().value(), duration = r.varint().value();
            const auto flags = r.u8().value();
            require(id == 0 && amplifier == 2 && duration > 0 && duration < 60 && flags == 0, std::format("speed 2: {} {} {} {}", id, amplifier, duration, flags));
            const auto second = c.until(State::play, "update_mob_effect");
            require(second.has_value(), "night vision too");
            auto n = second->reader();
            (void)n.varint();
            const auto nv = n.varint().value(), nv_amplifier = n.varint().value(), nv_duration = n.varint().value();
            require(nv == 15 && nv_amplifier == 0 && nv_duration == -1 && n.u8().value() == 6, "night vision, infinite");
            for (const auto expected : {20, 21}) {
                const auto e = c.until(State::play, "update_mob_effect");
                require(e.has_value(), "health boost and absorption too");
                auto er = e->reader();
                (void)er.varint();
                require(er.varint().value() == expected, std::format("effect {}", expected));
            }
            // Player data version 6: the absorption, then the attributes with
            // the effects' modifiers (max absorption 18, max health 19).
            const auto data = c.until(State::play, "set_entity_data");
            require(data.has_value(), "the absorption");
            auto d = data->reader();
            (void)d.varint();
            require(d.u8().value() == 15 && d.varint().value() == 3 && d.f32().value() == 8.0f, "absorption 8");
            const auto attributes = c.until(State::play, "update_attributes");
            require(attributes.has_value(), "the attributes");
            auto a = attributes->reader();
            (void)a.varint();
            std::map<std::int32_t, std::string> modifiers;
            std::set<std::int32_t> listed;
            for (auto k = a.varint().value(); k > 0; --k) {
                const auto attribute = a.varint().value();
                listed.insert(attribute);
                (void)a.f64();
                for (auto m = a.varint().value(); m > 0; --m) {
                    const auto name = a.string(32767).value();
                    const auto amount = a.f64().value(); // read in order
                    modifiers[attribute] += std::format("{} {} {};", name, amount, a.u8().value());
                }
            }
            require(modifiers[18] == "minecraft:effect.absorption 8 0;" && modifiers[19] == "minecraft:effect.health_boost 4 0;",
                    "the effects' modifiers back");
            require(listed.contains(17) && modifiers[17].empty(), "luck kept by its saved name, without a modifier");
            // The stronger speed ends: the weaker one comes back from underneath.
            const auto back = c.until(State::play, "update_mob_effect");
            require(back.has_value(), "the weaker speed back");
            auto w = back->reader();
            (void)w.varint();
            const auto back_id = w.varint().value(), back_amplifier = w.varint().value(), back_duration = w.varint().value();
            require(back_id == 0 && back_amplifier == 0 && back_duration > 4000 && back_duration < 4200 && w.u8().value() == 6,
                    std::format("speed 1 underneath: {} {} {}", back_id, back_amplifier, back_duration));
            s.stop();
        }
        std::filesystem::remove_all(dir);
    });

    suite.run("primed TNT survives a restart and explodes when its fuse runs out", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Igniter");
            confirm_spawn(c);
            use_item_on(c, "minecraft:redstone_block", {2, -61, 1}, 1);
            require(block_update(c, {2, -60, 1}) == content().state_id("minecraft:redstone_block"), "redstone block placed");
            use_item_on(c, "minecraft:tnt", {3, -61, 1}, 2);
            require(c.until(State::play, "add_entity").has_value(), "primed");
            s.stop();
        }
        const auto saved = recovered_state(config.world_directory);
        const auto tnt = std::ranges::find_if(saved.overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::primed_tnt; });
        require(tnt != saved.overworld().entities.end() && tnt->fuse > 0 && tnt->fuse < 80, "saved with its fuse");
        {
            // The player's chunk loads again and the fuse runs on.
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Igniter");
            confirm_spawn(c);
            require(c.until(State::play, "explode", std::chrono::milliseconds(15000)).has_value(), "it explodes after the restart");
            s.stop();
        }
        const auto after = recovered_state(config.world_directory);
        require(std::ranges::none_of(after.overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::primed_tnt; })
                    && block_at(after, {2, -60, 1}) == 0,
                "gone, and the redstone block with it");
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

    // A data pack directory: the 42 enchantments, a block tag that makes dirt
    // need an iron pickaxe, and an enchantment tag.
    const auto make_pack = [](const std::filesystem::path& root, std::string_view extra_tag) {
        const auto file = [&](const std::filesystem::path& rel, std::string_view text) {
            std::filesystem::create_directories((root / rel).parent_path());
            std::ofstream(root / rel) << text;
        };
        file("pack.mcmeta", R"({"pack": {"pack_format": 81, "description": "test"}})");
        for (const auto e : server::enchantment_registry().entries)
            file(std::format("data/minecraft/enchantment/{}.json", e.substr(10)), "{}");
        file("data/minecraft/tags/block/mineable/pickaxe.json", R"({"values": ["minecraft:dirt"]})");
        file("data/minecraft/tags/block/needs_iron_tool.json", R"({"values": ["minecraft:dirt"]})");
        file("data/minecraft/tags/enchantment/sharp.json", std::format(R"({{"values": ["minecraft:sharpness"{}]}})", extra_tag));
    };

    suite.run("data packs give registries, tags, tools and components", [&] {
        const auto dir = temporary_directory();
        make_pack(dir / "pack", "");
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        const auto d = server::load_server_data(bundle, {{}, {dir / "pack"}});
        require(d.has_value(), d ? "" : d.error());
        require(std::ranges::any_of(d->registries, [](const auto& r) { return r.registry == "minecraft:enchantment"; }),
                "enchantments are synchronized with their tags");
        const auto& content = *protocol::ContentTables::for_protocol(772);
        const auto find_tag = [&](std::string_view registry, std::string_view name) -> std::vector<std::int32_t> {
            for (const auto& r : d->tags)
                if (r.registry == registry)
                    for (const auto& [n, ids] : r.tags)
                        if (n == name) return ids;
            return {-1};
        };
        require(find_tag("minecraft:block", "minecraft:mineable/pickaxe") == std::vector<std::int32_t>{*content.block_id("minecraft:dirt")},
                "block tags by block id");
        const auto sharpness = server::registry_index(d->registries, "minecraft:enchantment", "minecraft:sharpness");
        require(find_tag("minecraft:enchantment", "minecraft:sharp") == std::vector<std::int32_t>{*sharpness}, "enchantment tags by id");
        // Enchantments are accepted only with the data.
        const auto& codec = *protocol::ComponentCodec::for_protocol(772);
        Writer w;
        w.varint(1).varint(0).varint(*codec.type_id("enchantments")).varint(1).varint(*sharpness).varint(5);
        protocol::Reader with(w.bytes()), without(w.bytes());
        require(codec.read(with, d->components).has_value() && !codec.read(without, server::component_context(bundle)).has_value(),
                "enchantment components");
        // Dirt now needs an iron pickaxe for its drops.
        const auto dirt = gameplay::registry::find_block("1.21.8", "minecraft:dirt").value();
        const auto& tool = d->game->block_tools.at(dirt);
        require(tool.tool == gameplay::registry::Tool::pickaxe && tool.tier == 3, "tool and tier from the tags");
        const auto dirt_state = *evidence::ContentMapping::for_game_version("1.21.8")->block("minecraft:dirt");
        const contracts::ItemStack iron{gameplay::items::iron_pickaxe, 1}, stone{gameplay::items::stone_pickaxe, 1};
        require(gameplay::harvests(dirt_state, iron, d->game.get()) && !gameplay::harvests(dirt_state, stone, d->game.get()) &&
                    gameplay::harvests(dirt_state, stone),
                "harvesting follows the data");
        // Packs must define the vanilla enchantments, no more and no fewer.
        std::filesystem::remove(dir / "pack/data/minecraft/enchantment/mending.json");
        const auto fewer = server::load_server_data(bundle, {{}, {dir / "pack"}});
        require(!fewer && fewer.error().find("add or remove enchantments") != std::string::npos, "changed enchantments refused");
        std::filesystem::remove_all(dir);
    });

    suite.run("recipes of several namespaces in the official order: by path, then namespace", [&] {
        const auto dir = temporary_directory();
        make_pack(dir / "pack", "");
        const auto recipe = [&](std::string_view id, std::string_view json) {
            const auto colon = id.find(':');
            const auto rel = std::filesystem::path("data") / id.substr(0, colon) / "recipe" / std::format("{}.json", id.substr(colon + 1));
            std::filesystem::create_directories((dir / "pack" / rel).parent_path());
            std::ofstream(dir / "pack" / rel) << json;
        };
        // The test pack of client/ns_crafting and client/ns_book, with two special recipes.
        const auto shapeless = [](std::string_view a, std::string_view b, std::string_view result) {
            return std::format(R"({{"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:{}", "minecraft:{}"], "result": {{"id": "minecraft:{}"}}}})",
                               a, b, result);
        };
        const auto smelt = [](std::string_view result) {
            return std::format(R"({{"type": "minecraft:smelting", "ingredient": "minecraft:dirt", "result": {{"id": "minecraft:{}"}}, "cookingtime": 40}})", result);
        };
        recipe("aaa:zzz_mix", shapeless("stick", "dirt", "oak_planks"));
        recipe("zzz:aaa_mix", shapeless("stick", "dirt", "cobblestone"));
        recipe("bbb:same_mix", shapeless("stick", "cobblestone", "birch_log"));
        recipe("aaa:same_mix", shapeless("stick", "cobblestone", "oak_log"));
        recipe("example:rocket", shapeless("paper", "gunpowder", "stick"));
        recipe("test:a_rocket", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:paper", "minecraft:gunpowder", "minecraft:gunpowder"], "result": {"id": "minecraft:bone"}})");
        recipe("aaa:zzz_smelt", smelt("oak_planks"));
        recipe("zzz:aaa_smelt", smelt("cobblestone"));
        recipe("minecraft:firework_rocket", R"({"type": "minecraft:crafting_special_firework_rocket"})");
        recipe("aaa:repair", R"({"type": "minecraft:crafting_special_repairitem"})");
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        const auto d = server::load_server_data(bundle, {{}, {dir / "pack"}});
        require(d.has_value() && d->recipe_book, d ? "" : d.error());
        const auto ids = [](const auto& list) {
            std::vector<std::string> out;
            for (const auto& r : list) out.push_back(r.id);
            return out;
        };
        require(ids(d->game->crafting) == std::vector<std::string>{"test:a_rocket", "zzz:aaa_mix", "example:rocket", "aaa:same_mix", "bbb:same_mix", "aaa:zzz_mix"},
                "crafting recipes by path, then namespace");
        require(ids(d->game->cooking) == std::vector<std::string>{"zzz:aaa_smelt", "aaa:zzz_smelt"}, "cooking recipes too");
        require(ids(d->game->special) == std::vector<std::string>{"minecraft:firework_rocket", "aaa:repair"}, "special recipes too");
        auto book = ids(d->recipe_book->recipes);
        std::erase_if(book, [](const std::string& id) { return id == "minecraft:firework_rocket" || id == "aaa:repair"; });
        require(book == std::vector<std::string>{"test:a_rocket", "zzz:aaa_mix", "zzz:aaa_smelt", "example:rocket", "aaa:same_mix", "bbb:same_mix", "aaa:zzz_mix", "aaa:zzz_smelt"},
                "the recipe book numbers them in that order (client/ns_book)");
        // The grids make the first in that order (client/ns_crafting).
        std::array<contracts::ItemStack, 4> grid{};
        grid[0] = {gameplay::items::stick, 1};
        grid[1] = {gameplay::items::dirt, 1};
        require(gameplay::crafted(*d->game, {2, 2, grid}) == contracts::ItemStack{gameplay::items::cobblestone, 1}, "zzz:aaa_mix before aaa:zzz_mix");
        grid[0] = {*gameplay::items::by_name("minecraft:paper"), 1};
        grid[1] = {*gameplay::items::by_name("minecraft:gunpowder"), 1};
        const auto rockets = gameplay::crafted(*d->game, {2, 2, grid});
        require(rockets.item == *gameplay::items::by_name("minecraft:firework_rocket") && rockets.count == 3, "minecraft:firework_rocket before example:rocket");
        grid[2] = grid[1];
        require(gameplay::crafted(*d->game, {2, 2, grid}) == contracts::ItemStack{*gameplay::items::by_name("minecraft:bone"), 1},
                "test:a_rocket before minecraft:firework_rocket");
        std::filesystem::remove_all(dir);
    });

    suite.run("data packs give block loot tables and recipes", [&] {
        const auto dir = temporary_directory();
        make_pack(dir / "pack", "");
        const auto file = [&](const std::filesystem::path& rel, std::string_view text) {
            std::filesystem::create_directories((dir / "pack" / rel).parent_path());
            std::ofstream(dir / "pack" / rel) << text;
        };
        const auto table = [&](std::string_view block, std::string_view pool) {
            file(std::format("data/minecraft/loot_table/blocks/{}.json", block),
                 std::format(R"({{"type": "minecraft:block", "pools": [{{"rolls": 1, "bonus_rolls": 0, {}}}]}})", pool));
        };
        table("bookshelf", R"("entries": [{"type": "minecraft:alternatives", "children": [
            {"type": "minecraft:item", "name": "minecraft:bookshelf", "conditions": [{"condition": "minecraft:match_tool",
              "predicate": {"predicates": {"minecraft:enchantments": [{"enchantments": "minecraft:silk_touch", "levels": {"min": 1}}]}}}]},
            {"type": "minecraft:item", "name": "minecraft:book", "functions": [{"function": "minecraft:set_count", "count": 3, "add": false},
              {"function": "minecraft:explosion_decay"}]}]}], "conditions": [{"condition": "minecraft:survives_explosion"}])");
        table("coal_ore", R"("entries": [{"type": "minecraft:item", "name": "minecraft:coal", "functions": [{"function": "minecraft:apply_bonus",
            "enchantment": "minecraft:fortune", "formula": "minecraft:ore_drops"}]}])");
        table("lilac", R"("entries": [{"type": "minecraft:item", "name": "minecraft:lilac", "conditions": [{"condition":
            "minecraft:block_state_property", "block": "minecraft:lilac", "properties": {"half": "lower"}}]}])");
        table("glowstone", R"("entries": [{"type": "minecraft:item", "name": "minecraft:glowstone_dust", "functions": [{"function":
            "minecraft:set_count", "count": {"type": "minecraft:uniform", "min": 2, "max": 4}}]}])");
        table("gravel", R"("entries": [{"type": "minecraft:item", "name": "minecraft:flint", "conditions": [{"condition": "minecraft:table_bonus",
            "enchantment": "minecraft:fortune", "chances": [0.1, 0.14]}]}])");
        table("clay", R"("entries": [{"type": "minecraft:dynamic", "name": "minecraft:contents"}])");
        file("data/minecraft/tags/item/test_logs.json", R"({"values": ["minecraft:oak_log", "minecraft:birch_log"]})");
        const auto recipe = [&](std::string_view name, std::string_view json) { file(std::format("data/test/recipe/{}.json", name), json); };
        recipe("planks", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["#minecraft:test_logs"], "result": {"id": "minecraft:oak_planks", "count": 4}})");
        recipe("shears", R"({"type": "minecraft:crafting_shaped", "category": "equipment", "pattern": [" #", "# "], "key": {"#": "minecraft:iron_ingot"},
            "result": {"id": "minecraft:shears"}})");
        recipe("box", R"({"type": "minecraft:crafting_transmute", "input": "minecraft:shulker_box", "material": "minecraft:red_dye",
            "result": {"id": "minecraft:red_shulker_box"}})");
        recipe("repair", R"({"type": "minecraft:crafting_special_repairitem", "category": "equipment"})");
        recipe("stew", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:bowl", ["minecraft:poppy", "minecraft:dandelion"]],
            "result": {"id": "minecraft:suspicious_stew", "components": {"minecraft:suspicious_stew_effects": []}}})");
        recipe("smelt", R"({"type": "minecraft:smelting", "category": "food", "ingredient": "minecraft:porkchop", "result": {"id": "minecraft:cooked_porkchop"},
            "experience": 0.35, "cookingtime": 200})");
        recipe("cut", R"({"type": "minecraft:stonecutting", "ingredient": "minecraft:stone", "result": {"id": "minecraft:stone_slab", "count": 2}})");
        recipe("widths", R"({"type": "minecraft:crafting_shaped", "pattern": ["##", "#"], "key": {"#": "minecraft:stick"}, "result": {"id": "minecraft:ladder"}})");
        recipe("unused", R"({"type": "minecraft:crafting_shaped", "pattern": ["#"], "key": {"#": "minecraft:stick", "X": "minecraft:coal"}, "result": {"id": "minecraft:torch"}})");
        recipe("unknown_item", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:no_such_item"], "result": {"id": "minecraft:stick"}})");
        recipe("unknown_tag", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["#minecraft:no_such_tag"], "result": {"id": "minecraft:stick"}})");
        recipe("unknown_type", R"({"type": "minecraft:crafting_magic", "result": {"id": "minecraft:stick"}})");
        recipe("too_many", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:stick"], "result": {"id": "minecraft:shears", "count": 2}})");
        recipe("air", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:air"], "result": {"id": "minecraft:stick"}})");
        recipe("trim", R"({"type": "minecraft:smithing_trim", "template": "minecraft:coast_armor_trim_smithing_template",
            "base": "#minecraft:test_logs", "addition": "minecraft:iron_ingot"})");
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        const auto d = server::load_server_data(bundle, {{}, {dir / "pack"}});
        require(d.has_value(), d ? "" : d.error());
        // Loot: five tables compile; the other names why not.
        require(d->loot.blocks == 6 && d->game->block_loot.size() == 5, std::format("{} tables, {} used", d->loot.blocks, d->game->block_loot.size()));
        require(d->loot.refused == std::map<std::string, std::size_t>{{"entry minecraft:dynamic", 1}}, "loot refusals by reason");
        // Recipes: every type checked; crafting recipes compiled; invalid ones skipped with a reason.
        std::vector<std::string> crafted;
        for (const auto& r : d->game->crafting) crafted.push_back(r.id);
        require(crafted == std::vector<std::string>{"test:box", "test:planks", "test:shears"}, "crafting recipes in id order");
        require(d->recipes.not_crafted == std::map<std::string, std::size_t>{{"result components", 1}}, "valid, not crafted");
        require((d->game->special == std::vector<gameplay::recipes::Special>{{gameplay::recipes::Special::Kind::repair_item, "test:repair"}}), "the special recipe");
        require(d->recipes.invalid == std::map<std::string, std::size_t>{{"air", 1}, {"key symbol not in the pattern", 1}, {"no trim pattern", 1},
                                                                         {"pattern rows of different widths", 1}, {"result count above the item's stack size", 1},
                                                                         {"unknown item", 1}, {"unknown item tag", 1}, {"unknown type minecraft:crafting_magic", 1}},
                "invalid recipes by reason");
        std::size_t valid = 0;
        for (const auto& [type, n] : d->recipes.valid) valid += n;
        require(valid == 7, "seven valid recipes");
        // The compiled shapeless recipe takes either log.
        std::array<contracts::ItemStack, 4> grid{};
        grid[3] = {gameplay::items::birch_log, 1};
        const auto made = gameplay::recipes::match(d->game->crafting, {2, 2, grid});
        require(made && made->result == contracts::ItemStack{gameplay::items::oak_planks, 4}, "planks from a tag");
        require(server::describe(*d).find("crafting recipes the rules match: 3") != std::string::npos, "the data report counts them");
        // 1.21.4 (data pack format 61): a transmute result is an item id.
        const auto old = dir / "old";
        std::filesystem::create_directories(old / "data/test/recipe");
        std::ofstream(old / "pack.mcmeta") << R"({"pack": {"pack_format": 61, "description": "test"}})";
        std::ofstream(old / "data/test/recipe/box.json")
            << R"({"type": "minecraft:crafting_transmute", "input": "minecraft:shulker_box", "material": "minecraft:red_dye", "result": "minecraft:red_shulker_box"})";
        std::ofstream(old / "data/test/recipe/box2.json")
            << R"({"type": "minecraft:crafting_transmute", "input": "minecraft:shulker_box", "material": "minecraft:red_dye", "result": {"id": "minecraft:red_shulker_box"}})";
        const auto d4 = server::load_server_data(**contracts::BundleRegistry::find("java-1.21.4"), {{}, {old}});
        require(d4 && d4->game->crafting.size() == 1 && d4->recipes.invalid.contains("result is not an item id"), "1.21.4 transmute results");
        std::filesystem::remove_all(dir);
    });

    suite.run("every official trace passes with the official data packs", [] {
        // With the operator's jars (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4), the
        // rules run as a server with them does: every trace of the version
        // passes, those of loot tables among them, and their skeletons record
        // as the committed reference recordings.
        namespace fs = std::filesystem;
        const fs::path fixtures = std::string(CIRCUIT_SOURCE_DIR) + "/tests/fixtures";
        for (const auto& [variable, bundle_id, directory] : {std::tuple{"CIRCUIT_VANILLA_JAR_1_21_8", "java-1.21.8", "harness"},
                                                              std::tuple{"CIRCUIT_VANILLA_JAR_1_21_4", "java-1.21.4", "harness-1.21.4"}}) {
            const char* jar = std::getenv(variable);
            if (!jar || !*jar) continue;
            const auto& bundle = **contracts::BundleRegistry::find(bundle_id);
            const auto loaded = server::load_server_data(bundle, {jar, {}});
            require(loaded.has_value(), loaded ? "" : loaded.error());
            const auto game = loaded->game;
            const auto executor = evidence::engine_executor(4, [game](const contracts::VersionBundle&) { return game; });
            const auto text = [](const fs::path& p) {
                std::ifstream in(p, std::ios::binary);
                return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            };
            std::size_t traces = 0, loot = 0;
            for (const auto& entry : fs::directory_iterator(fixtures / directory)) {
                const auto trace = evidence::parse_fixture(text(entry.path()));
                require(trace.has_value(), entry.path().string());
                const auto r = evidence::evaluate_fixture(*trace, executor);
                require(r.outcome == evidence::FixtureReport::Outcome::passed, std::format("{}: {}", entry.path().string(), r.detail));
                ++traces;
                if (std::ranges::find(trace->mechanisms, "loot") == trace->mechanisms.end()) continue;
                ++loot;
                if (std::string_view(bundle_id) != "java-1.21.8") continue;
                const auto skeleton = evidence::parse_fixture(text(fixtures / "skeletons" / entry.path().filename()));
                const auto recorded = evidence::record_fixture(*skeleton, executor);
                require(recorded && evidence::write_fixture(*recorded) == text(fixtures / "reference" / entry.path().filename()),
                        std::format("{}: the reference recording no longer matches", entry.path().filename().string()));
            }
            require(traces >= 140 && loot >= 3, std::format("{}: {} traces, {} of loot", bundle_id, traces, loot));
        }
    });

    suite.run("loot tables and recipes of the official packs agree with the official servers", [] {
        // The operator's jars, when given (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        struct Expected {
            const char* variable;
            const char* bundle;
            std::string_view release;
            std::size_t loot_blocks, loot_used, recipes, crafted;
        };
        std::size_t checked = 0;
        // Tables of blocks with rules of their own are not counted: since
        // rules v11 also the redstone torch's, the 14 buttons', the 16
        // pressure plates', the target block's, the 54 doors', trapdoors'
        // and fence gates', the honey block's, the daylight detector's, the
        // note block's, the four rails', the ten coral fans', the
        // dispenser's and dropper's, the furnace's, smoker's and blast
        // furnace's, the crafter's, the brewing stand's and the lectern's; but wheat's,
        // which its rules use. The 49
        // wall torches, signs, hanging signs, banners and heads count with the
        // standing ones' tables (the banners' and the player head's are
        // refused: they copy components).
        for (const auto& e : {Expected{"CIRCUIT_VANILLA_JAR_1_21_8", "java-1.21.8", "1.21.8", 926, 887, 1407, 984},
                              Expected{"CIRCUIT_VANILLA_JAR_1_21_4", "java-1.21.4", "1.21.4", 918, 879, 1370, 948}}) {
            const char* jar = std::getenv(e.variable);
            if (!jar || !*jar) continue;
            const auto& bundle = **contracts::BundleRegistry::find(e.bundle);
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value(), d ? "" : d.error());
            std::size_t valid = 0;
            for (const auto& [type, n] : d->recipes.valid) valid += n;
            require(d->loot.blocks == e.loot_blocks && d->game->block_loot.size() == e.loot_used && valid == e.recipes &&
                        d->recipes.invalid.empty() && d->game->crafting.size() == e.crafted,
                    std::format("{}: {} tables, {} used; {} recipes, {} invalid, {} crafted", e.bundle, d->loot.blocks,
                                d->game->block_loot.size(), valid, d->recipes.invalid.size(), d->game->crafting.size()));
            // Block drops (tests/fixtures/loot): what the rules drop (drops.cpp)
            // for each block and tool the official servers were shown.
            const auto& mapping = *evidence::ContentMapping::for_game_version(e.release);
            const auto& codec = *protocol::ComponentCodec::for_protocol(bundle.schema.component_schema);
            std::ifstream drops_in(std::string(CIRCUIT_SOURCE_DIR) + "/tests/fixtures/loot/block-drops.tsv");
            std::size_t cases = 0;
            for (std::string line; std::getline(drops_in, line);) {
                if (line.empty() || line[0] == '#') continue;
                std::vector<std::string> f;
                for (std::size_t i = 0;;) {
                    const auto tab = line.find('\t', i);
                    f.push_back(line.substr(i, tab == std::string::npos ? std::string::npos : tab - i));
                    if (tab == std::string::npos) break;
                    i = tab + 1;
                }
                if (f.size() != 6 || f[0] != e.release) continue;
                const auto state = mapping.block(f[1]);
                require(state.has_value(), f[1]);
                contracts::ItemStack tool{};
                if (f[2] != "-") tool = {gameplay::items::by_name(f[2]).value(), 1};
                if (f[3] != "-") {
                    const auto eq = f[3].find('=');
                    const auto id = server::registry_index(d->registries, "minecraft:enchantment", f[3].substr(0, eq)).value();
                    Writer w;
                    w.varint(1).varint(id).varint(std::stoi(f[3].substr(eq + 1)));
                    if (bundle.schema.component_schema == 769) w.boolean(true); // 1.21.4: shown in the tooltip
                    tool.components = contracts::ComponentPatch::make(bundle.schema.component_schema,
                                                                      {{*codec.type_id("enchantments"), w.take()}}, {}, {});
                }
                std::string got;
                // A block with rules of its own (an ender chest) by the registry state it stands for.
                const auto registered = gameplay::physics::registry_state_of(*state);
                const auto block = registered ? gameplay::registry::block_of(*registered) : std::nullopt;
                const auto table = block ? d->game->block_loot.find(*block) : d->game->block_loot.end();
                if (gameplay::harvests(*state, tool, d->game.get()) && table != d->game->block_loot.end()) {
                    const gameplay::loot::BreakContext context{gameplay::registry::properties(*registered), tool,
                                                               [&](std::uint32_t en) { return d->game->enchantment_level(tool, en); },
                                                               nullptr, {}};
                    if (const auto made = gameplay::loot::drops(table->second, context))
                        for (const auto& s : *made) got += std::format("{}{}*{}", got.empty() ? "" : " ", gameplay::items::info(s.item)->name, s.count);
                }
                if (got.empty()) got = "-";
                // Rolls that draw random numbers have no sequence here (the
                // calibration world's are not known) and drop nothing.
                require(got == f[4] || (f[5] == "random" && got == "-"), std::format("{} {} {} {}: {} (official {})", f[0], f[1], f[2], f[3], got, f[4]));
                ++cases;
            }
            require(cases == 23, std::format("{} drop cases", cases));
            // Crafting in the own window (tests/fixtures/crafting), replayed on
            // a 1.21.8 server; the 1.21.4 servers gave the same results.
            if (e.release == "1.21.8") {
                GameFixture f(offline_config(), std::move(*d));
                f.join(1, "alice");
                const contracts::PlayerKey key{1, 1};
                const auto name = [](const contracts::ItemStack& s) {
                    return std::format("{}*{}", gameplay::items::info(s.item)->name.substr(10), s.count);
                };
                const auto window = [&] {
                    const auto inv = f.game->engine().view().player(key).value();
                    const auto i = inv ? *inv : empty_inventory(key);
                    std::string own;
                    for (std::size_t n = 0; n < i.slots.size(); ++n)
                        if (!i.slots[n].empty()) own += std::format("{}{}:{}", own.empty() ? "" : " ", n, name(i.slots[n]));
                    return std::format("{}\t{}", own.empty() ? "-" : own, i.cursor.empty() ? "-" : name(i.cursor));
                };
                using I = server::Intent;
                std::ifstream in(std::string(CIRCUIT_SOURCE_DIR) + "/tests/fixtures/crafting/own-window.tsv");
                std::size_t steps = 0;
                for (std::string line; std::getline(in, line);) {
                    if (line.empty() || line[0] == '#') continue;
                    std::vector<std::string> fields;
                    for (std::size_t i = 0;;) {
                        const auto tab = line.find('\t', i);
                        fields.push_back(line.substr(i, tab == std::string::npos ? std::string::npos : tab - i));
                        if (tab == std::string::npos) break;
                        i = tab + 1;
                    }
                    std::istringstream args(fields.size() > 1 ? fields[1] : "");
                    const auto& op = fields[0];
                    if (op == "gamemode") {
                        f.game->submit(1, I{server::intent::Command{"gamemode " + fields[1]}});
                    } else if (op == "creative") {
                        int slot = 0, count = 0;
                        std::string item;
                        args >> slot >> item >> count;
                        f.game->submit(1, I{server::intent::CreativeSlot{static_cast<std::int16_t>(slot), count,
                                                                          content().item_id("minecraft:" + item).value()}});
                    } else if (op == "click") {
                        int slot = 0, button = 0, mode = 0;
                        args >> slot >> button >> mode;
                        f.game->submit(1, window_click(0, static_cast<std::int16_t>(slot), static_cast<std::int8_t>(button), mode));
                    } else if (op == "close") {
                        f.game->submit(1, I{server::intent::ContainerClose{0}});
                    } else if (op == "away") {
                        f.game->submit(1, I{server::intent::Command{"tp 40.5 -60 40.5"}});
                    }
                    f.game->tick();
                    if (op == "click" || op == "close" || op == "check") {
                        require(fields.size() >= 4 && window() == fields[2] + "\t" + fields[3],
                                std::format("{}: {} (official {}\t{})", fields.size() > 4 ? fields[4] : op, window(), fields[2], fields[3]));
                        ++steps;
                    }
                }
                require(steps == 48, std::format("{} crafting steps", steps));
            }
            ++checked;
        }
        if (checked == 0) std::puts("  (no official server jar given: the official loot and recipe checks did not run)");
    });

    suite.run("a world's log replays only under the data it was written with", [&] {
        const auto dir = temporary_directory();
        make_pack(dir / "a", "");
        make_pack(dir / "b", R"(, "minecraft:smite")");
        auto config = offline_config();
        config.world_directory = dir / "world";
        config.data_packs = {dir / "a"};
        { server::Server s(config); }
        // Add a logged tick after the checkpoint, as a crash would leave it.
        {
            storage::PosixFilesystem fs;
            const auto& bundle = **contracts::BundleRegistry::select_for_simulation("java-1.21.8");
            storage::WorldDirectory world(fs, config.world_directory, "java-1.21.8", std::string(bundle.rules->id));
            auto r = world.recover().value().value();
            require(world.checkpoint(r.checkpoint).has_value(), "checkpoint");
            require(world.append({r.checkpoint.state.time + 1, {}}, true).has_value(), "logged tick");
        }
        config.data_packs = {dir / "b"};
        bool refused = false;
        try {
            server::Server s(config);
        } catch (const std::runtime_error& e) {
            refused = std::string_view(e.what()).find("its log was written with data") != std::string_view::npos;
        }
        require(refused, "other data with a log is refused");
        config.data_packs = {dir / "a"};
        { server::Server s(config); } // replays its log and saves without one
        config.data_packs = {dir / "b"};
        { server::Server s(config); } // without a log the data may change
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

    suite.run("a world of the previous rule set is refused and migrates", [] {
        // tests/fixtures/storage/v9world-*: a world the last circuit-reference-rules-v9
        // server wrote and stopped cleanly, after randomTickSpeed 5, doDaylightCycle
        // false, /time set 6000, doWeatherCycle false, /weather thunder 100000 and
        // /forceload add 40 40.
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "v9";
        std::filesystem::create_directories(config.world_directory);
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(CIRCUIT_SOURCE_DIR) / "tests/fixtures/storage")) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("v9world-")) std::filesystem::copy_file(entry.path(), config.world_directory / name.substr(8));
        }
        std::string error;
        try {
            server::Server s(config);
        } catch (const std::runtime_error& e) {
            error = e.what();
        }
        require(error.find("circuit-reference-rules-v9") != std::string::npos && error.find("--migrate-world") != std::string::npos, error);
        const auto tick = server::migrate_world(config.world_directory, dir / "current", "java-1.21.8");
        require(tick == 311, std::format("migration: {}", tick ? std::to_string(*tick) : tick.error()));
        config.world_directory = dir / "current";
        GameFixture f(config);
        f.game->tick();
        const auto d = f.game->engine().dump();
        require(d.game_rules == std::map<std::string, std::int32_t>{{"doDaylightCycle", 0}, {"doWeatherCycle", 0}, {"randomTickSpeed", 5}},
                "game rules carried over");
        require(d.day_time == 6000 && d.weather.raining && d.weather.thundering && d.weather.rain_time > 99000
                    && d.weather.rain_level == 1.0f && d.weather.thunder_level == 1.0f,
                "day time and weather carried over; the start sets the levels");
        require(std::ranges::any_of(d.tickets, [](const auto& t) { return t.type == contracts::TicketType::forced && t.center == contracts::ChunkPos{2, 2}; }),
                "the forced chunk carried over");
        std::filesystem::remove_all(dir);
    });

    suite.run("a world of the previous rule set migrates with its live entities", [] {
        // tests/fixtures/storage/v10world-*: a world the last
        // circuit-reference-rules-v10 server wrote and stopped cleanly, 20
        // ticks after a player placed TNT beside a redstone block in a forced
        // chunk (/forceload add 0 0, randomTickSpeed 2).
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "v10";
        std::filesystem::create_directories(config.world_directory);
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(CIRCUIT_SOURCE_DIR) / "tests/fixtures/storage")) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("v10world-")) std::filesystem::copy_file(entry.path(), config.world_directory / name.substr(9));
        }
        std::string error;
        try {
            server::Server s(config);
        } catch (const std::runtime_error& e) {
            error = e.what();
        }
        require(error.find("circuit-reference-rules-v10") != std::string::npos && error.find("--migrate-world") != std::string::npos, error);
        const auto tick = server::migrate_world(config.world_directory, dir / "current", "java-1.21.8");
        require(tick == 151, std::format("migration: {}", tick ? std::to_string(*tick) : tick.error()));
        config.world_directory = dir / "current";
        GameFixture f(config);
        const auto primed = [&] {
            const auto d = f.game->engine().dump();
            const auto it = std::ranges::find_if(d.overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::primed_tnt; });
            return it == d.overworld().entities.end() ? -1 : it->fuse;
        };
        const auto fuse = primed();
        require(fuse > 0 && fuse < 80, std::format("the primed TNT carried over with its fuse: {}", fuse));
        require(f.game->engine().dump().game_rules.at("randomTickSpeed") == 2, "the game rule carried over");
        int ticks = 0;
        bool exploded = false;
        while (!exploded && ticks < 100) {
            f.game->tick();
            ++ticks;
            exploded = std::ranges::any_of(f.game->engine().trace(), [](const auto& r) { return r.kind == contracts::TraceKind::explosion; });
        }
        require(exploded && ticks == fuse, std::format("it explodes when its fuse runs out: after {} ticks, fuse {}", ticks, fuse));
        std::filesystem::remove_all(dir);
    });

    // tests/fixtures/storage/v11world-*: a world the last
    // circuit-reference-rules-v11 server wrote and stopped cleanly, 3 seconds
    // after a player started two-way flying engine A (minecraft.wiki's flying
    // machine gallery) eastward at y -56, z 4 and 5, in forced chunks
    // (/forceload add 0 0 47 15), toward an obsidian wall at x 30.
    const auto migrated_v11 = [](const std::filesystem::path& dir) {
        auto config = offline_config();
        config.world_directory = dir / "v11";
        std::filesystem::create_directories(config.world_directory);
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(CIRCUIT_SOURCE_DIR) / "tests/fixtures/storage")) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("v11world-")) std::filesystem::copy_file(entry.path(), config.world_directory / name.substr(9));
        }
        std::string error;
        try {
            server::Server s(config);
        } catch (const std::runtime_error& e) {
            error = e.what();
        }
        require(error.find("circuit-reference-rules-v11") != std::string::npos && error.find("--migrate-world") != std::string::npos, error);
        const auto tick = server::migrate_world(config.world_directory, dir / "current", "java-1.21.8");
        require(tick == 496, std::format("migration: {}", tick ? std::to_string(*tick) : tick.error()));
        config.world_directory = dir / "current";
        return config;
    };
    // The engine's blocks from x 0 to 31, the wall included.
    const auto engine_blocks = [](const GameFixture& f) {
        std::string blocks;
        for (std::int32_t x = 0; x < 32; ++x)
            for (std::int32_t y = -57; y <= -54; ++y)
                for (std::int32_t z = 4; z <= 5; ++z)
                    if (const auto s = f.at({x, y, z}); s != "minecraft:air") blocks += std::format("{} {} {} {}\n", x, y, z, s);
        return blocks;
    };
    const auto slime_x = [](const GameFixture& f) {
        for (std::int32_t x = 0; x < 32; ++x)
            if (f.at({x, -56, 4}) == "minecraft:slime_block") return x;
        return -1;
    };

    suite.run("a world of the previous rule set migrates with a flying machine in flight", [&] {
        const auto dir = temporary_directory();
        {
            GameFixture f(migrated_v11(dir));
            const auto start = slime_x(f);
            require(start > 4 && start < 28, std::format("the engine carried over in flight: slime at x {}", start));
            // It flies on until the wall stops it, its front column against
            // the wall: slime A and the west-facing piston at z 4, the
            // east-facing piston and slime B at z 5, an observer on each slime.
            int ticks = 0;
            int still = 0;
            for (auto last = start; ticks < 600 && still < 100; ++ticks) {
                f.game->tick();
                const auto now = slime_x(f);
                still = now == last ? still + 1 : 0;
                last = now;
            }
            require(still == 100 && slime_x(f) == 28, std::format("stopped at the wall: slime at x {} after {} ticks", slime_x(f), ticks));
            require(f.at({29, -56, 4}).starts_with("minecraft:sticky_piston[") && f.at({29, -56, 4}).contains("facing=west")
                        && f.at({28, -56, 5}).starts_with("minecraft:sticky_piston[") && f.at({28, -56, 5}).contains("facing=east")
                        && f.at({29, -56, 5}) == "minecraft:slime_block" && f.at({28, -55, 4}).starts_with("minecraft:observer[")
                        && f.at({29, -55, 5}).starts_with("minecraft:observer[") && f.at({30, -56, 4}) == "minecraft:obsidian",
                    engine_blocks(f));
        }
        std::filesystem::remove_all(dir);
    });

    suite.run("a flying machine survives a server restart", [&] {
        // One migrated copy runs 40 ticks, stops and starts again; another runs
        // without stopping. The machine is the same after the restart and at
        // every later tick.
        const auto dir = temporary_directory();
        const auto restarted = migrated_v11(dir / "restarted");
        const auto straight = migrated_v11(dir / "straight");
        {
            GameFixture through(straight);
            std::string stopped;
            {
                GameFixture f(restarted);
                for (int i = 0; i < 40; ++i) {
                    f.game->tick();
                    through.game->tick();
                }
                stopped = engine_blocks(f);
                require(stopped == engine_blocks(through), "the same before the restart");
            }
            GameFixture f(restarted);
            require(f.game->engine().dump().time == through.game->engine().dump().time && engine_blocks(f) == stopped,
                    std::format("the machine as it was saved:\n{}", engine_blocks(f)));
            for (int i = 0; i < 300; ++i) {
                f.game->tick();
                through.game->tick();
                const auto a = engine_blocks(f);
                const auto b = engine_blocks(through);
                require(a == b, std::format("tick {} after the restart:\n{}\nwithout it:\n{}", i + 1, a, b));
            }
            require(slime_x(f) == 28, std::format("stopped at the wall: slime at x {}", slime_x(f)));
        }
        std::filesystem::remove_all(dir);
    });

    return suite.finish();
}
