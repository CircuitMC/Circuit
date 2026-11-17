// SPDX-License-Identifier: AGPL-3.0-only
// In-process server tests: a real Server on an ephemeral port, driven by a
// small client built on Circuit's codec. They check protocol state handling,
// limits and the translation of player actions into simulation commands.
#include "../support/check.hpp"
#include "test_client.hpp"
#include "../../src/server/command_reader.hpp"
#include "../../src/server/game.hpp"
#include "../../src/server/server_data.hpp"
#include <circuit/gameplay/damage.hpp>
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
#include <functional>
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
    c.initial_game_rules["spawnRadius"] = 0; // players appear at the spawn itself
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
    // On a target version, with the data a server of it loaded from data packs.
    GameFixture(std::string_view v, ServerConfig c, server::ServerData d)
        : version(v), data(std::move(d)), wire(version == "1.21.4" ? 769 : 772, data.components), config(std::move(c)) {
        game = std::make_unique<server::Game>(config, bundle, wire, *protocol::ContentTables::for_protocol(wire.protocol()), mapping, data);
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
        const auto r = server::parse_config("game-rule.spawnRadius=3\ngame-rule.keepInventory=true\n");
        require(r && r->initial_game_rules == std::map<std::string, std::int64_t>{{"keepInventory", 1}, {"spawnRadius", 3}}, "a new world's game rules");
        require(!server::parse_config("game-rule.keepInventory=maybe\n"), "a game rule's value");
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
            require(d.game_rules == std::map<std::string, std::int32_t>{{"keepInventory", 1}, {"randomTickSpeed", 7}, {"spawnChunkRadius", 0}, {"spawnRadius", 0}},
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
                        else if (type == 3) text += std::format("{}", in.f32().value());
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
        // Bob appears with his data as last told (glowing, his health, his
        // effects ambient).
        auto bob_appears = bob_seen;
        bob_appears.insert(bob_appears.find(" 11=true"), " 9=20");
        require(std::erase(a, bob_appears) == 1, "bob appears glowing");
        // Alice's first data (her health) follows her effects, then the health packet (client/player_data).
        const auto health = std::ranges::find(a, std::string("health"));
        require(health != a.end() && health - a.begin() == 4 && health + 1 == a.end() && a[3].ends_with(" 9=20"),
                "three effects, her first data, then the health");
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
        // The advancements completed send the experience again, unchanged,
        // once (client/xp_death); then nothing more.
        const auto experience_packets = [&](std::size_t from) {
            const auto id = f.wire.ids().id(State::play, Bound::clientbound, "set_experience");
            std::lock_guard lock(peer->mutex);
            return std::ranges::count_if(std::span(peer->packets).subspan(from), [&](const auto& bytes) {
                const auto pk = protocol::open_packet(bytes);
                return pk && pk->id == id;
            });
        };
        f.game->tick();
        require(experience_packets(mark) == 1, "the experience again after the advancements");
        mark = [&] { std::lock_guard lock(peer->mutex); return peer->packets.size(); }();
        f.game->tick();
        require(experience_packets(mark) == 0, "and not again");
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
                        else if (type == 3) { // the health (9) is passed over too
                            const auto value = in.f32().value();
                            if (index == 15) out.push_back(std::format("absorption {} {}", entity, value));
                        }
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

    suite.run("beacons give their powers to players in range and play their sounds as both official servers do", [] {
        // circuit-clienttest/beacon.js, both versions identical.
        using I = server::Intent;
        GameFixture f;
        auto alice = f.join(1, "Alice");
        auto bob = f.join(2, "Bob");
        // Since the last call, as text: "data <x> <y> <z> <type> <tag>" (block
        // entity data), "sound <id> <category> <x> <y> <z>" (the wire's id),
        // "screen <type> <title key>", "property <index> <value>", "effect
        // <id> <amplifier> <duration> <flags>".
        std::map<std::uint64_t, std::size_t> seen;
        std::int32_t window = 0;
        const auto packets = [&](Recorder& r) {
            const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
            std::vector<std::string> out;
            std::lock_guard lock(r.mutex);
            for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                auto pk = protocol::open_packet(r.packets[at]);
                if (!pk) continue;
                auto& in = pk->fields;
                if (pk->id == id("block_entity_data")) {
                    const auto pos = in.position().value();
                    const auto type = in.varint().value();
                    const auto tag = protocol::nbt::read_network(in).value();
                    const auto* levels = tag.find("Levels");
                    out.push_back(std::format("data {} {} {} {} Levels={}", pos.x, pos.y, pos.z, type, levels ? levels->integer() : -1));
                } else if (pk->id == id("sound")) {
                    const auto sound = in.varint().value(), category = in.varint().value();
                    const auto x = in.i32().value(), y = in.i32().value(), z = in.i32().value();
                    out.push_back(std::format("sound {} {} {} {} {}", sound, category, x, y, z));
                } else if (pk->id == id("open_screen")) {
                    window = in.varint().value();
                    const auto type = in.varint().value();
                    const auto title = protocol::nbt::read_network(in).value();
                    out.push_back(std::format("screen {} {}", type, title.find("translate") ? title.find("translate")->text() : "?"));
                } else if (pk->id == id("container_set_data")) {
                    (void)in.varint();
                    const auto property = in.i16().value();
                    out.push_back(std::format("property {} {}", property, in.i16().value()));
                } else if (pk->id == id("update_mob_effect")) {
                    (void)in.varint();
                    const auto effect = in.varint().value(), amplifier = in.varint().value(), duration = in.varint().value();
                    out.push_back(std::format("effect {} {} {} {}", effect, amplifier, duration, in.u8().value()));
                }
            }
            return out;
        };
        const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
        command(1, "tp 8.5 -60 4.5");
        command(2, "tp 8.5 -60 31.5");
        command(1, "fill 7 -60 7 9 -60 9 minecraft:iron_block");
        command(1, "setblock 8 -59 8 minecraft:beacon");
        f.game->tick();
        f.game->tick();
        auto a = packets(*alice);
        require(std::ranges::count(a, std::string("data 8 -59 8 15 Levels=0")) == 1, "the new beacon's block entity data");
        (void)packets(*bob);
        // Up to the first cycle: the ambient, then the activate sound (ids plus one), at the block's center.
        const auto until_sound = [&] {
            for (int k = 0; k < 81; ++k) {
                f.game->tick();
                auto now = packets(*alice);
                if (std::ranges::any_of(now, [](const auto& t) { return t.starts_with("sound "); })) return now;
            }
            return std::vector<std::string>{};
        };
        a = until_sound();
        require(a == std::vector<std::string>{"sound 142 4 68 -468 68", "sound 141 4 68 -468 68"}, "ambient then activate");
        // The window: menu type 9, its title, the levels and no powers.
        f.game->submit(1, I{server::intent::UseOn{0, {8, -59, 8}, 1, 0.5f, 1.0f, 0.5f, false, 1}});
        f.game->tick();
        a = packets(*alice);
        require(std::ranges::find(a, std::string("screen 9 container.beacon")) != a.end(), "the beacon window");
        for (const auto* p : {"property 0 1", "property 1 0", "property 2 0"})
            require(std::ranges::find(a, std::string(p)) != a.end(), std::format("{}", p));
        // A payment shift-clicked from the hotbar, then strength chosen.
        f.game->submit(1, I{server::intent::CreativeSlot{36, 1, content().item_id("minecraft:iron_ingot").value()}});
        f.game->tick();
        f.game->submit(1, I{server::intent::ContainerClick{window, 0, 28, 0, 1, {}, std::nullopt}});
        f.game->tick();
        f.game->submit(1, I{server::intent::SetBeacon{4, std::nullopt}});
        f.game->tick();
        f.game->tick();
        a = packets(*alice);
        require(std::ranges::find(a, std::string("sound 144 4 68 -468 68")) != a.end() && std::ranges::find(a, std::string("property 1 5")) != a.end(),
                "strength chosen: the power_select sound and the property");
        // The next cycle: strength for 11 seconds, ambient, to alice; bob, 23 blocks away, gets nothing.
        a = until_sound();
        require(a == std::vector<std::string>{"effect 4 0 220 15", "sound 142 4 68 -468 68"}, "the cycle gives strength, then its sound");
        require(std::ranges::none_of(packets(*bob), [](const auto& t) { return t.starts_with("effect "); }), "bob is out of range");
        command(2, "tp 8.5 -60 27.5");
        until_sound();
        const auto b = packets(*bob);
        require(std::ranges::find(b, std::string("effect 4 0 220 15")) != b.end(), "19 blocks away, bob gets it");
        // client/beacon2: a spectator gets nothing; the range reaches the
        // world's height (384) above the grown cell: y 346 here.
        const auto given = [&](Recorder& r) {
            for (int k = 0; k < 80; ++k) f.game->tick();
            return std::ranges::any_of(packets(r), [](const auto& t) { return t.starts_with("effect 4 0 220"); });
        };
        command(2, "gamemode spectator");
        f.game->tick();
        (void)packets(*bob);
        require(!given(*bob), "a spectator gets nothing");
        command(1, "tp 8.5 345.5 8.5");
        f.game->tick();
        (void)packets(*alice);
        require(given(*alice), "y 345.5: in range");
        command(1, "tp 8.5 346.5 8.5");
        f.game->tick();
        (void)packets(*alice);
        require(!given(*alice), "y 346.5: out of range");
    });

    suite.run("effects heal, hurt and feed players, who are hurt, die and respawn as both official servers show them", [] {
        // circuit-clienttest/effects-heal.js: alice in survival, bob in creative
        // beside her; both versions alike but for the sound ids and the name's form.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            const bool v772 = std::string_view(version) == "1.21.8";
            GameFixture f(version, offline_config());
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            command(1, "gamemode survival");
            command(1, "gamerule naturalRegeneration false"); // Circuit's default difficulty, easy: food runs down
            command(1, "tp 8.5 -60 8.5");
            command(2, "tp 8.5 -60 12.5");
            for (int k = 0; k < 5; ++k) f.game->tick();
            // Each player's entity id, from their login.
            std::map<std::int32_t, std::string> names;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = name;
            }
            // Since the last call, as text.
            std::map<std::uint64_t, std::size_t> seen;
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        const auto health = in.f32().value();
                        const auto food = in.varint().value();
                        out.push_back(std::format("health {} food {} saturation {}", health, food, in.f32().value()));
                    } else if (pk->id == id("damage_event")) {
                        const auto e = in.varint().value();
                        out.push_back(std::format("damage {} type {}", who(e), in.varint().value()));
                    } else if (pk->id == id("set_entity_motion")) {
                        const auto e = in.varint().value();
                        const auto x = in.i16().value(), y = in.i16().value(), z = in.i16().value();
                        out.push_back(std::format("velocity {} {} {} {}", who(e), x, y, z));
                    } else if (pk->id == id("sound")) {
                        const auto sound = in.varint().value() - 1, category = in.varint().value();
                        const auto x = in.i32().value(), y = in.i32().value(), z = in.i32().value();
                        out.push_back(std::format("sound {} category {} at {} {} {} volume {}", sound, category, x, y, z, in.f32().value()));
                    } else if (pk->id == id("entity_event")) {
                        const auto e = in.i32().value();
                        out.push_back(std::format("status {} {}", who(e), static_cast<int>(in.i8().value())));
                    } else if (pk->id == id("player_combat_kill")) {
                        (void)in.varint();
                        const auto message = protocol::nbt::read_network(in).value();
                        out.push_back(std::format("death screen {}", message.find("translate")->text()));
                        // The whole message, keys in order (an int array by its length).
                        std::function<std::string(const protocol::nbt::Tag&)> text;
                        text = [&text](const protocol::nbt::Tag& t) -> std::string {
                            const auto& self = text;
                            switch (t.type()) {
                            case protocol::nbt::TagType::string: return std::format("\"{}\"", t.text());
                            case protocol::nbt::TagType::int_array: return std::format("[I;{}]", t.array().size());
                            case protocol::nbt::TagType::list: {
                                std::string out = "[";
                                for (const auto& i : t.items()) out += (out.size() > 1 ? "," : "") + self(i);
                                return out + "]";
                            }
                            case protocol::nbt::TagType::compound: {
                                std::string out = "{";
                                for (std::size_t k = 0; k < t.keys().size(); ++k)
                                    out += std::format("{}{}:{}", k ? "," : "", t.keys()[k], self(t.items()[k]));
                                return out + "}";
                            }
                            default: return "?";
                            }
                        };
                        out.push_back("message " + text(message));
                    } else if (pk->id == id("remove_entities")) {
                        for (auto n = in.varint().value(); n > 0; --n) out.push_back(std::format("removed {}", who(in.varint().value())));
                    } else if (pk->id == id("add_entity")) {
                        out.push_back(std::format("spawned {}", who(in.varint().value())));
                    } else if (pk->id == id("respawn")) {
                        out.emplace_back("respawn");
                    }
                }
                return out;
            };
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
            };
            const auto give = [&](std::string what) { command(1, "effect give " + what); };
            const std::int32_t magic = 27, wither = 47;
            const auto hurt = v772 ? 1175 : 1135, death = v772 ? 1174 : 1134;
            const auto at = std::format("at 68 -480 68");
            (void)packets(*alice);
            (void)packets(*bob);
            // Instant damage: 6, magic; the velocity at the next entity update; bob hears the hurt.
            give("alice minecraft:instant_damage 1 0");
            ticks(2);
            auto a = packets(*alice);
            auto b = packets(*bob);
            require(a == std::vector<std::string>{std::format("damage alice type {}", magic), "health 14 food 20 saturation 5", "velocity alice 0 -627 0"},
                    std::format("{}: instant damage: {}", version, a.size()));
            require(b == std::vector<std::string>{std::format("damage alice type {}", magic), std::format("sound {} category 7 {} volume 1", hurt, at),
                                                  "velocity alice 0 -627 0"},
                    std::format("{}: what bob sees of it", version));
            // Again at once: the hurt cooldown keeps it out.
            give("alice minecraft:instant_damage 1 0");
            ticks(2);
            require(packets(*alice).empty(), std::format("{}: within the hurt cooldown, nothing", version));
            // Regeneration I for 10 s: a heal at once, then every 50 ticks (4 in all).
            ticks(30);
            give("alice minecraft:regeneration 10 0");
            ticks(1);
            require(packets(*alice) == std::vector<std::string>{"health 15 food 20 saturation 5"}, std::format("{}: the first heal at once", version));
            ticks(210);
            a = packets(*alice);
            require(a.size() == 3 && a.back() == "health 18 food 20 saturation 5", std::format("{}: three more heals", version));
            // Poison III for 1 s (every 6 ticks): the hurt cooldown lets every other one through; magic damage.
            give("alice minecraft:poison 1 2");
            ticks(21);
            a = packets(*alice);
            require(std::ranges::count(a, std::format("damage alice type {}", magic)) == 2 && std::ranges::count(a, std::string("health 16 food 20 saturation 5")) == 1,
                    std::format("{}: poison III twice in 20 ticks", version));
            // Wither V until she dies: the death screen, the death animation, health 0; bob hears the death and keeps the body.
            ticks(20);
            (void)packets(*bob);
            give("alice minecraft:wither 30 4");
            const auto dead = std::string("death screen death.attack.wither");
            a.clear();
            b.clear();
            for (int k = 0; k < 300 && std::ranges::find(a, dead) == a.end(); ++k) {
                f.game->tick();
                std::ranges::copy(packets(*alice), std::back_inserter(a));
                std::ranges::copy(packets(*bob), std::back_inserter(b));
            }
            const auto from = std::ranges::find(a, dead);
            const auto message = std::string(v772 ? R"(message {with:[{click_event:{action:"suggest_command",command:"/tell alice "},insertion:"alice",text:"alice",)"
                                                    R"(hover_event:{name:"alice",action:"show_entity",id:"minecraft:player",uuid:[I;4]}}],translate:"death.attack.wither"})"
                                                  : R"(message {with:[{hoverEvent:{contents:{name:"alice",id:[I;4],type:"minecraft:player"},action:"show_entity"},)"
                                                    R"(clickEvent:{action:"suggest_command",value:"/tell alice "},insertion:"alice",text:"alice"}],translate:"death.attack.wither"})");
            require(from != a.end() && std::vector(from, a.end()) == std::vector<std::string>{dead, message, "status alice 3", "health 0 food 20 saturation 5"},
                    std::format("{}: the wither's death, the name in the version's form", version));
            require(std::ranges::count(a, std::format("damage alice type {}", wither)) == 16, std::format("{}: 16 wither hits", version));
            const auto last = std::ranges::find(b, std::format("sound {} category 7 {} volume 1", death, at));
            require(last != b.end() && std::vector(last - 1, b.end()) == std::vector<std::string>{std::format("damage alice type {}", wither),
                        std::format("sound {} category 7 {} volume 1", death, at), "status alice 3"},
                    std::format("{}: bob hears the death and sees the animation; the body stays", version));
            // On the death screen: the velocity at the next entity update; 20
            // ticks after the death the body's last event, then it goes from
            // bob's view and bob from hers (client/player_data).
            ticks(1);
            require(packets(*alice) == std::vector<std::string>{"velocity alice 0 -627 0"} && packets(*bob) == std::vector<std::string>{"velocity alice 0 -627 0"},
                    std::format("{}: the body's velocity", version));
            ticks(18);
            require(packets(*alice).empty() && packets(*bob).empty(), std::format("{}: the body stays 19 ticks", version));
            ticks(1);
            require(packets(*alice) == std::vector<std::string>{"status alice 60", "removed bob"} &&
                        packets(*bob) == std::vector<std::string>{"status alice 60", "removed alice"},
                    std::format("{}: then it goes", version));
            // The respawn: nothing left to remove; each sees the other again.
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            ticks(2);
            a = packets(*alice);
            b = packets(*bob);
            require(a.size() >= 2 && a[0] == "respawn" && std::ranges::count(a, std::string("spawned bob")) == 1 && std::ranges::count(a, std::string("removed bob")) == 0,
                    std::format("{}: alice respawns", version));
            require(b == std::vector<std::string>{"spawned alice"}, std::format("{}: bob sees her come back", version));
            // Hunger L for 5 s: 25 exhaustion, the saturation, then a point of food.
            command(1, "tp 8.5 -60 8.5");
            ticks(2);
            (void)packets(*alice);
            give("alice minecraft:hunger 5 49");
            ticks(110);
            require(packets(*alice) == std::vector<std::string>{"health 20 food 20 saturation 0", "health 20 food 19 saturation 0"},
                    std::format("{}: hunger", version));
            // Saturation I: a point of food and 2 saturation.
            give("alice minecraft:saturation 1 0");
            ticks(2);
            require(packets(*alice) == std::vector<std::string>{"health 20 food 20 saturation 2"}, std::format("{}: saturation", version));
            // Bob in creative: no damage, no hurt.
            (void)packets(*bob);
            give("bob minecraft:instant_damage 1 1");
            give("bob minecraft:wither 2 2");
            ticks(45);
            b = packets(*bob);
            require(std::ranges::none_of(b, [](const auto& t) { return t.starts_with("damage") || t.starts_with("health"); }),
                    std::format("{}: a creative player is not hurt", version));
        }
    });

    suite.run("players burn in fire and lava, armor and resistance reducing it, as both official servers show it", [] {
        // circuit-clienttest/fire.js: alice in survival in a pit with a
        // netherrack floor into which fire, lava and water are set; bob in
        // creative mode watching.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            GameFixture f(version, offline_config());
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const bool v772 = std::string_view(version) == "1.21.8";
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::map<std::int32_t, std::string> names;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = name;
            }
            // Since the last call, as text: damage events, sounds, the shared
            // flags, health, velocity, the own window's slots, death messages.
            std::map<std::uint64_t, std::size_t> seen;
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        out.push_back(std::format("health {}", in.f32().value()));
                    } else if (pk->id == id("damage_event")) {
                        const auto e = in.varint().value();
                        out.push_back(std::format("damage {} {}", who(e), in.varint().value()));
                    } else if (pk->id == id("set_entity_motion")) {
                        const auto e = in.varint().value();
                        (void)in.i16();
                        out.push_back(std::format("velocity {} {}", who(e), in.i16().value()));
                    } else if (pk->id == id("sound")) {
                        const auto sound = in.varint().value() - 1, category = in.varint().value();
                        (void)in.i32(), (void)in.i32(), (void)in.i32();
                        out.push_back(std::format("sound {} {} {}", sound, category, in.f32().value()));
                    } else if (pk->id == id("set_entity_data")) {
                        const auto e = in.varint().value();
                        if (in.u8().value() == 0 && in.varint().value() == 0) out.push_back(std::format("flags {} {}", who(e), in.u8().value()));
                    } else if (pk->id == id("container_set_slot")) {
                        if (in.varint().value() != 0) continue;
                        (void)in.varint();
                        const auto slot = in.i16().value();
                        const auto count = in.varint().value();
                        std::string line = std::format("slot {}", slot);
                        if (count > 0) {
                            line += " " + std::string(tables.item_name(in.varint().value()).value_or("?").substr(10));
                            if (in.varint().value() == 1) {
                                (void)in.varint(); // removed
                                (void)in.varint(); // the damage component
                                line += std::format(" damage {}", in.varint().value());
                            }
                        }
                        out.push_back(line);
                    } else if (pk->id == id("player_combat_kill")) {
                        out.emplace_back("death screen");
                    } else if (pk->id == id("system_chat")) {
                        // The death message's translation key, read from its bytes.
                        const std::string text(reinterpret_cast<const char*>(r.packets[at].data()), r.packets[at].size());
                        if (const auto k = text.find("death."); k != std::string::npos) {
                            auto e = k;
                            while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '.')) ++e;
                            out.push_back("message " + text.substr(k, e - k));
                        }
                    } else if (pk->id == id("set_equipment")) {
                        out.push_back(std::format("equipment {}", who(in.varint().value())));
                    }
                }
                return out;
            };
            const auto has = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::find(v, x) != v.end(); };
            const auto count = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::count(v, x); };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            // One tick: what each got in it.
            const auto tick = [&] {
                f.game->tick();
                return std::pair{packets(*alice), packets(*bob)};
            };
            const auto ticks = [&](int n) {
                std::pair<std::vector<std::string>, std::vector<std::string>> all;
                for (int k = 0; k < n; ++k) {
                    auto [a, b] = tick();
                    all.first.insert(all.first.end(), a.begin(), a.end());
                    all.second.insert(all.second.end(), b.begin(), b.end());
                }
                return all;
            };
            const auto hurt_on_fire = std::format("sound {} 7 1", v772 ? 1178 : 1138);
            const auto burn = std::format("sound {} 7 0.4", v772 ? 611 : 598);
            const auto extinguish = std::format("sound {} 7 0.7", v772 ? 616 : 603);
            for (const auto* c : {"gamerule doFireTick false", "gamerule naturalRegeneration false", "difficulty normal", "setblock 8 -62 8 netherrack", "setblock 8 -61 8 air", "tp 8.5 -61 8.5", "gamemode survival"})
                command(1, c);
            command(2, "tp 8.5 -60 12.5");
            (void)ticks(5);

            // Fire: a hurt (in_fire, 21) at once and 10 ticks later; at the
            // 20th tick in it the burning's first (on_fire, 31) instead, and
            // the flag in the next.
            command(1, "setblock 8 -61 8 fire");
            std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> timeline;
            for (int k = 0; k < 24; ++k) timeline.push_back(tick());
            std::size_t t0 = 0;
            while (t0 < timeline.size() && !has(timeline[t0].first, "damage alice 21")) ++t0;
            require(t0 + 22 < timeline.size(), std::format("{}: the first hurt in fire", version));
            require(has(timeline[t0].first, "health 19") && has(timeline[t0].second, "damage alice 21") && has(timeline[t0].second, hurt_on_fire),
                    std::format("{}: the hurt, bob's sound", version));
            require(has(timeline[t0 + 10].first, "damage alice 21") && has(timeline[t0 + 20].first, "damage alice 31")
                        && !has(timeline[t0 + 20].first, "damage alice 21") && has(timeline[t0 + 21].first, "flags alice 1")
                        && has(timeline[t0 + 21].second, "flags alice 1") && !has(timeline[t0 + 20].first, "flags alice 1"),
                    std::format("{}: catching fire at the 20th tick", version));
            // Out of it at once: seven more hurts, then the flag goes.
            command(1, "setblock 8 -61 8 air");
            auto [a, b] = ticks(200);
            require(count(a, "damage alice 31") == 7 && has(a, "flags alice 0") && count(b, hurt_on_fire) == 7 && has(a, "health 12"),
                    std::format("{}: burning out of fire ({} hurts; {})", version, count(a, "damage alice 31"), [&] {
                        std::string t;
                        for (const auto& x : a) if (!x.starts_with("velocity")) t += x + "; ";
                        return t;
                    }()));

            // Fire resistance: in fire without hurts, shown burning.
            command(1, "effect give alice minecraft:instant_health 1 3");
            command(1, "effect give alice minecraft:fire_resistance 100 0");
            (void)ticks(2);
            command(1, "setblock 8 -61 8 fire");
            std::tie(a, b) = ticks(25);
            command(1, "setblock 8 -61 8 air");
            auto [a2, b2] = ticks(200);
            require(!std::ranges::any_of(a, [](const auto& x) { return x.starts_with("damage"); }) && has(a, "flags alice 1") && has(a2, "flags alice 0")
                        && !std::ranges::any_of(a2, [](const auto& x) { return x.starts_with("damage"); }),
                    std::format("{}: fire resistance", version));
            command(1, "effect clear alice minecraft:fire_resistance");

            // Resistance II: 0.6 of each.
            command(1, "effect give alice minecraft:resistance 100 1");
            (void)ticks(25);
            command(1, "setblock 8 -61 8 fire");
            std::tie(a, b) = ticks(3);
            command(1, "setblock 8 -61 8 air");
            require(has(a, "damage alice 21") && has(a, std::format("health {}", 20.0f - 0.6f)), std::format("{}: resistance", version));
            command(1, "effect clear alice minecraft:resistance");
            command(1, "effect give alice minecraft:instant_health 1 3");
            (void)ticks(25);

            // Armor: an iron helmet, a diamond chestplate, netherite boots.
            command(1, "gamemode creative");
            (void)ticks(1);
            for (const auto& [slot, item] : {std::pair<std::int16_t, std::string_view>{5, "minecraft:iron_helmet"},
                                             {6, "minecraft:diamond_chestplate"}, {8, "minecraft:netherite_boots"}})
                f.game->submit(1, I{server::intent::CreativeSlot{slot, 1, tables.item_id(item).value(), 0}});
            (void)ticks(1);
            command(1, "gamemode survival");
            (void)ticks(3);
            command(1, "setblock 8 -61 8 fire");
            std::tie(a, b) = ticks(3);
            command(1, "setblock 8 -61 8 air");
            std::tie(a2, b2) = ticks(2);
            require(has(a, "damage alice 21") && has(a, std::format("health {}", 19.507692337036133f)), std::format("{}: armor's reduction", version));
            a.insert(a.end(), a2.begin(), a2.end());
            require(has(a, "slot 5 iron_helmet damage 1") && has(a, "slot 6 diamond_chestplate damage 1")
                        && !std::ranges::any_of(a, [](const auto& x) { return x.starts_with("slot 8"); }),
                    std::format("{}: armor wears, not netherite by fire", version));
            (void)ticks(25);

            // Lava: 4 reduced by the armor, its burn sound for all, the
            // velocity at the bottom of lava, the flag a tick later on 1.21.8;
            // water in its place puts the fire out.
            command(1, "effect give alice minecraft:instant_health 1 3");
            (void)ticks(25);
            command(1, "setblock 8 -61 8 lava");
            timeline.clear();
            for (int k = 0; k < 6; ++k) timeline.push_back(tick());
            std::size_t l = 0;
            while (l < timeline.size() && !has(timeline[l].first, "damage alice 24")) ++l;
            require(l + 3 < timeline.size(), std::format("{}: a lava hurt", version));
            require(has(timeline[l].first, burn) && has(timeline[l].first, std::format("health {}", 20.0f - 2.1169229f))
                        && has(timeline[l].second, hurt_on_fire) && has(timeline[l].second, burn) && has(timeline[l + 1].first, "velocity alice -160"),
                    std::format("{}: the lava hurt", version));
            require(has(timeline[v772 ? l + 2 : l + 1].first, "flags alice 1") && !has(timeline[v772 ? l + 1 : l].first, "flags alice 1"),
                    std::format("{}: the flag after lava", version));
            // The armor it wore: bob sees it in the hurt's tick on 1.21.4, at
            // the next on 1.21.8; alice's slots come at the next on both.
            require(has(timeline[v772 ? l + 1 : l].second, "equipment alice") && !has(timeline[v772 ? l : l + 1].second, "equipment alice")
                        && has(timeline[l + 1].first, "slot 5 iron_helmet damage 2") && !has(timeline[l].first, "slot 5 iron_helmet damage 2"),
                    std::format("{}: the lava's wear (alice {} / {}; bob {} / {})", version, [&] { std::string t; for (const auto& x : timeline[l].first) t += x + "; "; return t; }(),
                                [&] { std::string t; for (const auto& x : timeline[l + 1].first) t += x + "; "; return t; }(),
                                [&] { std::string t; for (const auto& x : timeline[l].second) t += x + "; "; return t; }(),
                                [&] { std::string t; for (const auto& x : timeline[l + 1].second) t += x + "; "; return t; }()));
            command(1, "setblock 8 -61 8 water[level=0]");
            std::tie(a, b) = ticks(4);
            const auto text = [](const std::vector<std::string>& v) {
                std::string t;
                for (const auto& x : v) t += x + "; ";
                return t;
            };
            require(has(a, extinguish) == v772 && has(b, extinguish) && has(a, "flags alice 0"),
                    std::format("{}: water puts it out (alice: {} bob: {}; {})", version, text(a), text(b), f.at({8, -61, 8})));
            command(1, "setblock 8 -61 8 air");
            (void)ticks(25);

            // Burning with armor on: 1 a hurt, no wear.
            command(1, "effect give alice minecraft:instant_health 1 3");
            (void)ticks(25);
            command(1, "setblock 8 -61 8 lava");
            (void)ticks(2);
            command(1, "setblock 8 -61 8 air");
            (void)ticks(25);
            {
                timeline.clear();
                for (int k = 0; k < 21; ++k) timeline.push_back(tick());
                std::size_t h = 0;
                while (h < timeline.size() && !has(timeline[h].first, "damage alice 31")) ++h;
                require(h < timeline.size(), std::format("{}: a burning hurt with armor", version));
                // Full health, lava's 4 reduced by the armor, then two burning
                // hurts of 1 each, not reduced.
                const float expect = 20.0f - circuit::gameplay::damage::after_armor(4.0f, 13.0f, 5.0f) - 1.0f - 1.0f;
                bool worn = false;
                for (std::size_t k = h; k < timeline.size(); ++k)
                    worn = worn || std::ranges::any_of(timeline[k].first, [](const auto& x) { return x.starts_with("slot "); });
                require(has(timeline[h].first, std::format("health {}", expect)) && !worn, std::format("{}: burning not reduced, no wear", version));
                // The next burning hurt is due 20 ticks after this one: water
                // set by a command in its tick (the players' tick sees a
                // command's blocks at once). 1.21.8 hurts, then puts her out
                // (the flag two ticks later); 1.21.4 puts her out first.
                const auto past = static_cast<int>(timeline.size() - 1 - h); // ticks run since the hurt
                (void)ticks(19 - past);
                command(1, "setblock 8 -61 8 water[level=0]");
                timeline.clear();
                for (int k = 0; k < 4; ++k) timeline.push_back(tick());
                const auto& due = timeline[0];
                require(has(due.first, "damage alice 31") == v772 && has(due.second, extinguish) && has(due.first, extinguish) == v772,
                        std::format("{}: water as a burning hurt is due", version));
                require(has(timeline[v772 ? 2 : 1].first, "flags alice 0") && !has(timeline[v772 ? 1 : 0].first, "flags alice 0"),
                        std::format("{}: the flag after water", version));
                command(1, "setblock 8 -61 8 air");
                (void)ticks(2);
            }

            // Rain puts her out (the sound for bob; for alice too on 1.21.8).
            command(1, "setblock 8 -61 8 lava");
            (void)ticks(2);
            command(1, "setblock 8 -61 8 air");
            (void)ticks(25);
            command(1, "weather rain");
            std::tie(a, b) = ticks(40);
            require(has(a, "flags alice 0") && has(b, extinguish) && has(a, extinguish) == v772, std::format("{}: rain puts her out", version));
            command(1, "weather clear");
            (void)ticks(40);

            // A magma block under her: a hurt (hot_floor, 20) every 10
            // ticks while she stands, none while she sneaks.
            command(1, "effect give alice minecraft:instant_health 1 3");
            f.game->submit(1, I{server::intent::Move{std::nullopt, std::nullopt, true}});
            (void)ticks(25);
            command(1, "setblock 8 -62 8 magma_block");
            std::tie(a, b) = ticks(25);
            require(count(a, "damage alice 20") == 3 && has(b, hurt_on_fire), std::format("{}: magma", version));
            f.game->submit(1, I{server::intent::Sneak{true}});
            (void)ticks(15);
            std::tie(a, b) = ticks(30);
            require(count(a, "damage alice 20") == 0, std::format("{}: none while sneaking", version));
            f.game->submit(1, I{server::intent::Sneak{false}});
            std::tie(a, b) = ticks(5);
            require(count(a, "damage alice 20") == 1, std::format("{}: standing up", version));
            command(1, "setblock 8 -62 8 netherrack");
            (void)ticks(2);

            // bob in creative mode in fire: never shown burning, never hurt.
            command(1, "setblock 8 -60 12 fire");
            std::tie(a, b) = ticks(40);
            command(1, "setblock 8 -60 12 air");
            require(!has(b, "flags bob 1") && !std::ranges::any_of(b, [](const auto& x) { return x.starts_with("damage bob"); }),
                    std::format("{}: creative bob in fire", version));

            // Burning across a rejoin: the flag with her first data, the burning going on.
            command(1, "setblock 8 -61 8 lava");
            (void)ticks(2);
            command(1, "setblock 8 -61 8 air");
            (void)ticks(5);
            f.game->leave(1);
            f.game->tick();
            alice = f.join(1, "alice");
            {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(alice->mutex);
                for (const auto& bytes : alice->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = "alice";
            }
            seen[1] = 0;
            require(has(packets(*alice), "flags alice 1"), std::format("{}: shown burning on rejoining", version));
            std::tie(a, b) = ticks(30);
            require(count(a, "damage alice 31") >= 1, std::format("{}: burning after a rejoin", version));

            // Burning to death after lava (from 4 health: burnt, death.attack.onFire).
            command(1, "effect give alice minecraft:instant_health 1 3");
            (void)ticks(25);
            command(1, "setblock 8 -61 8 lava");
            (void)ticks(3);
            command(1, "setblock 8 -61 8 air");
            command(1, "effect give alice minecraft:instant_damage 1 1");
            std::tie(a, b) = ticks(320);
            require(has(a, "death screen") && has(a, "health 0") && has(a, "message death.attack.onFire"), std::format("{}: burnt to death", version));
            // The armor the death drops: its emptied slots before the health
            // of 0 (client/xp_death's order, Circuit's for every death).
            const auto before = [](const std::vector<std::string>& v, std::string_view first, std::string_view then) {
                const auto x = std::ranges::find(v, first), y = std::ranges::find(v, then);
                return x != v.end() && y != v.end() && x < y;
            };
            require(before(a, "slot 5", "health 0") && before(a, "slot 6", "health 0"), std::format("{}: burnt, the slots first", version));
            // Killed by lava, whose hurt wears the armor in the death's tick
            // (its slots held to the next tick): still the slots first.
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            (void)ticks(2);
            command(1, "tp 8.5 -61 8.5");
            command(1, "gamemode creative");
            (void)ticks(1);
            for (const auto& [slot, item] : {std::pair<std::int16_t, std::string_view>{5, "minecraft:iron_helmet"},
                                             {6, "minecraft:diamond_chestplate"}, {8, "minecraft:netherite_boots"}})
                f.game->submit(1, I{server::intent::CreativeSlot{slot, 1, tables.item_id(item).value(), 0}});
            (void)ticks(1);
            command(1, "gamemode survival");
            command(1, "effect give alice minecraft:instant_damage 1 1");
            (void)ticks(25);
            command(1, "effect give alice minecraft:instant_damage 1 0");
            (void)ticks(25);
            command(1, "setblock 8 -61 8 lava");
            std::tie(a, b) = ticks(3);
            command(1, "setblock 8 -61 8 air");
            require(has(a, "message death.attack.lava") && before(a, "slot 5", "health 0") && before(a, "slot 6", "health 0")
                        && before(a, "slot 8", "health 0"),
                    std::format("{}: killed by lava, the worn slots first: {}", version, [&] {
                        std::string t;
                        for (const auto& x : a) if (!x.starts_with("velocity")) t += x + "; ";
                        return t;
                    }()));
        }
    });

    suite.run("players drown, suffocate, crawl, freeze and are hurt by cacti and berry bushes as both official servers show it", [] {
        // circuit-clienttest/drown.js: alice in survival in a pit (8 -62 8
        // and 8 -61 8) in a floor of dirt (the fixture's world is empty) into
        // which water, stone and powder snow are set; bob in creative mode
        // watching.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            GameFixture f(version, offline_config());
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const bool v772 = std::string_view(version) == "1.21.8";
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::map<std::int32_t, std::string> names;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = name;
            }
            // Since the last call, as text: damage events, sounds, entity
            // events, the air, pose and frozen ticks of entity data, health,
            // velocity, effects and attributes (id: modifier amounts).
            std::map<std::uint64_t, std::size_t> seen;
            std::int32_t last_teleport = 0; // alice's
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        out.push_back(std::format("health {}", in.f32().value()));
                    } else if (pk->id == id("player_position")) {
                        if (&r == alice.get()) last_teleport = in.varint().value();
                    } else if (pk->id == id("damage_event")) {
                        const auto e = in.varint().value();
                        out.push_back(std::format("damage {} {}", who(e), in.varint().value()));
                    } else if (pk->id == id("set_entity_motion")) {
                        const auto e = in.varint().value();
                        (void)in.i16();
                        out.push_back(std::format("velocity {} {}", who(e), in.i16().value()));
                    } else if (pk->id == id("sound")) {
                        const auto sound = in.varint().value() - 1;
                        out.push_back(std::format("sound {}", sound));
                    } else if (pk->id == id("entity_event")) {
                        const auto e = in.i32().value();
                        out.push_back(std::format("status {} {}", who(e), static_cast<int>(in.i8().value())));
                    } else if (pk->id == id("set_entity_data")) {
                        const auto e = in.varint().value();
                        for (auto index = in.u8().value(); index != 0xFF; index = in.u8().value()) {
                            const auto type = in.varint().value();
                            if (type == 0) {
                                const auto v = in.u8().value();
                                if (index == 0) out.push_back(std::format("flags {} {}", who(e), v));
                            } else if (type == 3) (void)in.f32();
                            else if (type == 8) (void)in.u8();
                            else if (type == 1 || type == 21) {
                                const auto v = in.varint().value();
                                if (index == 1) out.push_back(std::format("air {} {}", who(e), v));
                                if (index == 6) out.push_back(std::format("pose {} {}", who(e), v));
                                if (index == 7) out.push_back(std::format("frozen {} {}", who(e), v));
                            } else {
                                break; // particles: not read here
                            }
                        }
                    } else if (pk->id == id("update_mob_effect")) {
                        (void)in.varint();
                        const auto effect = in.varint().value(), amplifier = in.varint().value(), duration = in.varint().value();
                        out.push_back(std::format("effect {} {} {} flags {}", effect, amplifier, duration, in.u8().value()));
                    } else if (pk->id == id("remove_mob_effect")) {
                        (void)in.varint();
                        out.push_back(std::format("removed {}", in.varint().value()));
                    } else if (pk->id == id("update_attributes")) {
                        const auto e = in.varint().value();
                        for (auto n = in.varint().value(); n > 0; --n) {
                            std::string line = std::format("attribute {} {}", who(e), in.varint().value());
                            (void)in.f64();
                            for (auto m = in.varint().value(); m > 0; --m) {
                                line += " " + in.string(32767).value();
                                line += std::format(" {}", in.f64().value());
                                (void)in.u8();
                            }
                            out.push_back(line);
                        }
                    } else if (pk->id == id("player_combat_kill")) {
                        out.emplace_back("death screen");
                    } else if (pk->id == id("system_chat")) {
                        const std::string text(reinterpret_cast<const char*>(r.packets[at].data()), r.packets[at].size());
                        if (const auto k = text.find("death."); k != std::string::npos) {
                            auto e = k;
                            while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '.')) ++e;
                            out.push_back("message " + text.substr(k, e - k));
                        }
                    }
                }
                return out;
            };
            const auto has = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::find(v, x) != v.end(); };
            const auto count = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::count(v, x); };
            const auto starting = [](const std::vector<std::string>& v, std::string_view x) {
                return std::ranges::count_if(v, [&](const auto& s) { return s.starts_with(x); });
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const auto tick = [&] {
                f.game->tick();
                return std::pair{packets(*alice), packets(*bob)};
            };
            const auto ticks = [&](int n) {
                std::pair<std::vector<std::string>, std::vector<std::string>> all;
                for (int k = 0; k < n; ++k) {
                    auto [a, b] = tick();
                    all.first.insert(all.first.end(), a.begin(), a.end());
                    all.second.insert(all.second.end(), b.begin(), b.end());
                }
                return all;
            };
            const auto text = [](const std::vector<std::string>& v) {
                std::string t;
                for (const auto& x : v) t += x + "; ";
                return t;
            };
            // Whether the air told rose by 4 from one tick to the next.
            const auto refills = [](const std::vector<std::string>& v) {
                std::vector<int> air;
                for (const auto& x : v)
                    if (x.starts_with("air alice ")) air.push_back(std::stoi(x.substr(10)));
                for (std::size_t k = 1; k < air.size(); ++k)
                    if (air[k] == air[k - 1] + 4) return true;
                return false;
            };
            const std::string heal = "effect give alice minecraft:instant_health 1 3";
            const auto drown_sound = std::format("sound {}", v772 ? 1176 : 1136);
            const auto freeze_sound = std::format("sound {}", v772 ? 1177 : 1137);
            const auto bush_sound = std::format("sound {}", v772 ? 1179 : 1139);
            const auto oxygen = std::format("attribute alice {}", v772 ? 23 : 22);
            const auto speed = std::format("attribute alice {}", v772 ? 22 : 21);
            const auto water_breathing = std::to_string(*circuit::gameplay::effects::id("water_breathing"));
            for (const auto* c : {"gamerule naturalRegeneration false", "difficulty normal", "fill 4 -63 4 12 -61 12 dirt", "fill 8 -62 8 8 -61 8 air",
                                  "tp 8.5 -62 8.5", "gamemode survival"})
                command(1, c);
            command(2, "tp 8.5 -60 12.5");
            (void)ticks(5);
            command(1, heal);
            (void)ticks(25);

            // Drowning: 319 ticks of air told down to -19, the hurt (2,
            // drown 6) in the next, on 1.21.8 entity event 67 before it;
            // no velocity; bob hears hurt_drown; 0 told after it; the
            // oxygen bonus the player's at the first tick, without modifiers.
            command(1, "fill 8 -62 8 8 -61 8 water[level=0]");
            std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> timeline;
            for (int k = 0; k < 330; ++k) timeline.push_back(tick());
            std::size_t t0 = 0;
            while (t0 < timeline.size() && !has(timeline[t0].first, "damage alice 6")) ++t0;
            require(t0 + 2 < timeline.size(), std::format("{}: the drowning hurt", version));
            std::size_t first = 0;
            while (first < t0 && !has(timeline[first].first, "air alice 299")) ++first;
            require(first < t0 && t0 - first == 318 && has(timeline[t0].first, "air alice -19") && has(timeline[t0 + 1].first, "air alice 0"),
                    std::format("{}: air from 299 to -19, the hurt at -20 ({} ticks)", version, t0 - first));
            require(has(timeline[first].first, oxygen) && has(timeline[first].second, oxygen), std::format("{}: the oxygen bonus", version));
            const auto& at_hurt = timeline[t0].first;
            const auto status = std::ranges::find(at_hurt, "status alice 67"), damage = std::ranges::find(at_hurt, "damage alice 6");
            require(v772 ? status < damage : status == at_hurt.end(), std::format("{}: the bubbles' event: {}", version, text(at_hurt)));
            require(has(timeline[t0].second, "status alice 67") == v772, std::format("{}: bob sees the bubbles' event on 1.21.8", version));
            require(has(timeline[t0].second, drown_sound) && has(at_hurt, "health 18") && starting(timeline[t0 + 1].first, "velocity") == 0,
                    std::format("{}: bob's sound, no velocity: {} / {} / {}", version, text(timeline[t0].second), text(at_hurt), text(timeline[t0 + 1].first)));
            auto [a, b] = ticks(20);
            require(count(a, "damage alice 6") == 1 && has(a, "health 16"), std::format("{}: every 20 ticks", version));
            // Out of the water, 4 a tick back.
            command(1, "fill 8 -62 8 8 -61 8 air");
            std::tie(a, b) = ticks(5);
            require(refills(a), std::format("{}: air back: {}", version, text(a)));
            (void)ticks(80);
            // Water breathing under water: 4 a tick back too; drowningDamage off: the event but no hurt.
            command(1, heal);
            command(1, "gamerule drowningDamage false");
            command(1, "fill 8 -62 8 8 -61 8 water[level=0]");
            std::tie(a, b) = ticks(330);
            require(count(a, "status alice 67") == (v772 ? 1 : 0) && starting(a, "damage") == 0, std::format("{}: drowningDamage off", version));
            command(1, "gamerule drowningDamage true");
            command(1, "effect give alice minecraft:water_breathing 60 0");
            std::tie(a, b) = ticks(5);
            require(refills(a), std::format("{}: water breathing gives air back: {}", version, text(a)));
            command(1, "effect clear alice minecraft:water_breathing");
            command(1, "fill 8 -62 8 8 -61 8 air");
            (void)ticks(80);

            // The turtle shell: water breathing for 10 s without particles at
            // the tick after it goes on, every tick out of water; none under it.
            command(1, "gamemode creative");
            (void)ticks(1);
            f.game->submit(1, I{server::intent::CreativeSlot{5, 1, tables.item_id("minecraft:turtle_helmet").value(), 0}});
            (void)ticks(1);
            command(1, "gamemode survival");
            std::tie(a, b) = ticks(3);
            require(has(a, std::format("effect {} 0 200 flags 12", water_breathing)) && has(a, std::format("effect {} 0 200 flags 4", water_breathing))
                        && starting(b, "effect") == 0,
                    std::format("{}: the turtle shell: {}", version, text(a)));
            command(1, "fill 8 -62 8 8 -61 8 water[level=0]");
            (void)ticks(1);
            std::tie(a, b) = ticks(10);
            require(starting(a, "effect") == 0, std::format("{}: none under water", version));
            command(1, "fill 8 -62 8 8 -61 8 air");
            command(1, "gamemode creative");
            (void)ticks(1);
            f.game->submit(1, I{server::intent::CreativeSlot{5, 0, 0, 0}});
            (void)ticks(1);
            command(1, "gamemode survival");
            command(1, "effect clear alice minecraft:water_breathing");
            (void)ticks(5);

            // Suffocation: stone at the head, one hurt (in_wall 22), then
            // crawling (pose 3) told with the velocity; stone at the feet and
            // head, a hurt every 10 ticks; glass none.
            command(1, heal);
            (void)ticks(25);
            command(1, "setblock 8 -61 8 stone");
            std::tie(a, b) = ticks(1);
            const auto [a2, b2] = ticks(1);
            const auto [a3, b3] = ticks(30);
            require(has(a, "damage alice 22") && has(a2, "pose alice 3") && has(b2, "pose alice 3") && has(a2, "velocity alice -627")
                        && starting(a3, "damage") == 0,
                    std::format("{}: crawling under the stone: {} / {}", version, text(a), text(a2)));
            command(1, "setblock 8 -61 8 air");
            std::tie(a, b) = ticks(3);
            require(has(a, "pose alice 0") && has(b, "pose alice 0"), std::format("{}: standing again", version));
            command(1, "fill 8 -62 8 8 -61 8 stone");
            std::tie(a, b) = ticks(22);
            require(count(a, "damage alice 22") == 3 && starting(a, "pose") == 0, std::format("{}: in stone: {}", version, text(a)));
            command(1, "fill 8 -62 8 8 -61 8 glass");
            (void)ticks(1);
            std::tie(a, b) = ticks(25);
            require(starting(a, "damage") == 0, std::format("{}: glass does not suffocate", version));
            command(1, "fill 8 -62 8 8 -61 8 air");
            command(1, heal);
            (void)ticks(25);

            // A cactus's cell (not its box): a hurt every 10 ticks, the first
            // in the tick after the teleport; the velocity 0 when a teleport
            // comes before it is told.
            command(1, "setblock 11 -61 8 sand");
            command(1, "setblock 11 -60 8 cactus");
            (void)ticks(1);
            command(1, "tp 10.705 -60 8.5");
            std::tie(a, b) = ticks(1);
            require(starting(a, "damage") == 0, std::format("{}: not in the teleport's tick", version));
            std::tie(a, b) = ticks(11);
            require(count(a, "damage alice 2") == 2 && has(a, "velocity alice -627") && has(b, "sound " + std::to_string(v772 ? 1175 : 1135)),
                    std::format("{}: the cactus: {}", version, text(a)));
            (void)ticks(9);
            std::tie(a, b) = ticks(1);
            require(has(a, "damage alice 2"), std::format("{}: the cactus again", version));
            command(1, "tp 8.5 -62 8.5");
            std::tie(a, b) = ticks(1);
            require(has(a, "velocity alice 0"), std::format("{}: teleported before the velocity: {}", version, text(a)));
            command(1, "setblock 11 -60 8 air");
            command(1, heal);
            (void)ticks(25);

            // A sweet berry bush grown at least once: no hurt standing in it;
            // moving across it a hurt (sweet_berry_bush 41) every 10 ticks,
            // its velocity 0; one not grown, none.
            command(1, "setblock 5 -60 8 sweet_berry_bush[age=3]");
            command(1, "tp 5.5 -60 8.5");
            std::tie(a, b) = ticks(30);
            require(starting(a, "damage") == 0, std::format("{}: standing in the bush", version));
            const auto walk = [&](int n) {
                f.game->submit(1, I{server::intent::ConfirmTeleport{last_teleport}});
                std::pair<std::vector<std::string>, std::vector<std::string>> all;
                for (int k = 0; k < n; ++k) {
                    f.game->submit(1, I{server::intent::Move{server::Vec3{5.5, -60.0, 8.5 - 0.01 * (k + 1)}, std::nullopt, true}});
                    auto [x, y] = tick();
                    all.first.insert(all.first.end(), x.begin(), x.end());
                    all.second.insert(all.second.end(), y.begin(), y.end());
                }
                return all;
            };
            std::tie(a, b) = walk(21);
            require(count(a, "damage alice 41") == 3 && has(b, bush_sound) && has(a, "velocity alice 0") && !has(a, "velocity alice -627"),
                    std::format("{}: walking through the bush: {}", version, text(a)));
            command(1, "setblock 5 -60 8 sweet_berry_bush[age=0]");
            command(1, "tp 5.5 -60 8.5");
            (void)ticks(25);
            std::tie(a, b) = walk(21);
            require(starting(a, "damage") == 0, std::format("{}: a bush not grown", version));
            command(1, "setblock 5 -60 8 air");
            command(1, "tp 8.5 -62 8.5");
            command(1, heal);
            (void)ticks(25);

            // Powder snow: frozen ticks 1 a tick to 140 with the slowness each
            // time; fully frozen, a hurt (freeze 17) at each 40th tick of the
            // player's age, and the slowness every second tick; 2 a tick out
            // of it, the slowness gone at 0. Leather boots: none.
            command(1, "fill 8 -62 8 8 -61 8 powder_snow");
            (void)ticks(1);
            std::tie(a, b) = ticks(140);
            require(has(a, "frozen alice 1") && has(a, "frozen alice 140") && has(a, speed + " minecraft:powder_snow -0.0003571428533177823")
                        && has(b, speed + " minecraft:powder_snow -0.05000000074505806") && starting(a, speed) == 140,
                    std::format("{}: freezing", version));
            std::tie(a, b) = ticks(80);
            require(count(a, "damage alice 17") == 2 && has(b, freeze_sound) && count(a, "velocity alice -627") == 2 && starting(a, speed) == 40,
                    std::format("{}: fully frozen: {} hurts, {} slowness", version, count(a, "damage alice 17"), starting(a, speed)));
            command(1, "fill 8 -62 8 8 -61 8 air");
            std::tie(a, b) = ticks(72);
            require(has(a, "frozen alice 138") && has(a, "frozen alice 0") && has(a, speed) && starting(a, "damage") == 0,
                    std::format("{}: thawing: {}", version, text(a)));
            command(1, "gamemode creative");
            (void)ticks(1);
            f.game->submit(1, I{server::intent::CreativeSlot{8, 1, tables.item_id("minecraft:leather_boots").value(), 0}});
            (void)ticks(1);
            command(1, "gamemode survival");
            command(1, "fill 8 -62 8 8 -61 8 powder_snow");
            std::tie(a, b) = ticks(30);
            require(starting(a, "frozen") == 0, std::format("{}: leather boots", version));
            command(1, "fill 8 -62 8 8 -61 8 air");
            // freezeDamage off: frozen, no hurt.
            command(1, "gamemode creative");
            (void)ticks(1);
            f.game->submit(1, I{server::intent::CreativeSlot{8, 0, 0, 0}});
            (void)ticks(1);
            command(1, "gamemode survival");
            command(1, "gamerule freezeDamage false");
            command(1, "fill 8 -62 8 8 -61 8 powder_snow");
            std::tie(a, b) = ticks(200);
            require(has(a, "frozen alice 140") && starting(a, "damage") == 0, std::format("{}: freezeDamage off", version));
            command(1, "gamerule freezeDamage true");
            command(1, "fill 8 -62 8 8 -61 8 air");
            (void)ticks(75);
            command(1, heal);
            (void)ticks(25);

            // Lava a command takes away as its hurt's velocity is due: told as
            // in lava, as of the player's last tick (client/fire).
            command(1, "setblock 8 -62 8 lava");
            std::tie(a, b) = ticks(1);
            require(has(a, "damage alice 24"), std::format("{}: lava at once", version));
            command(1, "setblock 8 -62 8 air");
            std::tie(a, b) = ticks(1);
            require(has(a, "velocity alice -160"), std::format("{}: the lava's velocity after the lava went: {}", version, text(a)));
            command(1, "setblock 8 -62 8 water[level=0]"); // put out
            (void)ticks(3);
            command(1, "setblock 8 -62 8 air");
            command(1, heal);
            (void)ticks(80);

            // Falls of 10 blocks into the pit: with slow falling none; with
            // fallDamage off no hurt but the fall's sound for bob; else 7.
            const auto big_fall = std::format("sound {}", v772 ? 1171 : 1131);
            const auto fall_from = [&](double top) {
                command(1, std::format("tp 8.5 {} 8.5", top));
                (void)ticks(1);
                f.game->submit(1, I{server::intent::ConfirmTeleport{last_teleport}});
                std::pair<std::vector<std::string>, std::vector<std::string>> all;
                for (double y = top - 0.5; y >= -62.0; y -= 0.5) {
                    f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, y, 8.5}, std::nullopt, y <= -62.0}});
                    auto [x, z] = tick();
                    all.first.insert(all.first.end(), x.begin(), x.end());
                    all.second.insert(all.second.end(), z.begin(), z.end());
                }
                auto [x, z] = ticks(2);
                all.first.insert(all.first.end(), x.begin(), x.end());
                all.second.insert(all.second.end(), z.begin(), z.end());
                return all;
            };
            command(1, "effect give alice minecraft:slow_falling 30 0");
            std::tie(a, b) = fall_from(-52.0);
            require(starting(a, "damage") == 0 && !has(b, big_fall), std::format("{}: slow falling: {}", version, text(a)));
            command(1, "effect clear alice minecraft:slow_falling");
            command(1, "gamerule fallDamage false");
            std::tie(a, b) = fall_from(-52.0);
            require(starting(a, "damage") == 0 && has(b, big_fall), std::format("{}: fallDamage off: {}", version, text(b)));
            command(1, "gamerule fallDamage true");
            std::tie(a, b) = fall_from(-52.0);
            require(has(a, "damage alice 10") && has(a, "health 13") && has(b, big_fall), std::format("{}: the fall: {}", version, text(a)));
            command(1, heal);
            (void)ticks(25);

            // fireDamage off: in fire no hurt, but burning and its flag.
            command(1, "gamerule fireDamage false");
            command(1, "gamerule doFireTick false"); // the fire stays
            command(1, "setblock 8 -62 8 fire");
            std::tie(a, b) = ticks(30);
            command(1, "setblock 8 -62 8 air");
            const auto after_fire = ticks(200).first;
            require(starting(a, "damage") == 0 && starting(after_fire, "damage") == 0 && has(a, "flags alice 1") && has(after_fire, "flags alice 0"),
                    std::format("{}: fireDamage off: {}", version, text(a)));
            command(1, "gamerule fireDamage true");

            // Under a ceiling 1.5 above a bottom slab: crouching (pose 5),
            // kept as the shift key goes down and up, without suffocating.
            command(1, "setblock 10 -60 10 smooth_stone_slab");
            command(1, "setblock 10 -58 10 stone");
            command(1, "tp 10.5 -59.5 10.5");
            std::tie(a, b) = ticks(25);
            // Teleported in standing, her eyes in the stone until the pose is
            // decided: one hurt, as with stone set at the head.
            require(has(a, "pose alice 5") && starting(a, "damage") <= 1, std::format("{}: crouching under the ceiling: {}", version, text(a)));
            f.game->submit(1, I{server::intent::Sneak{true}});
            (void)ticks(5);
            f.game->submit(1, I{server::intent::Sneak{false}});
            std::tie(a, b) = ticks(25);
            require(!has(a, "pose alice 0") && starting(a, "damage") == 0, std::format("{}: still crouching: {}", version, text(a)));
            command(1, "setblock 10 -58 10 air");
            command(1, "setblock 10 -60 10 dirt");
            command(1, "tp 8.5 -62 8.5");
            (void)ticks(5);

            // A frozen and breathless player (in powder snow, then under
            // water as the frozen ticks go down) who leaves comes back so.
            command(1, "fill 8 -62 8 8 -61 8 powder_snow");
            (void)ticks(51);
            command(1, "fill 8 -62 8 8 -61 8 water[level=0]");
            std::tie(a, b) = ticks(11);
            require(has(a, "frozen alice 33") && has(a, "air alice 291"), std::format("{}: frozen and breathless: {}", version, text(a)));
            f.game->leave(1);
            f.game->tick();
            alice = f.join(1, "alice");
            seen[1] = 0;
            {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(alice->mutex);
                for (const auto& bytes : alice->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = "alice";
            }
            (void)packets(*bob);
            f.game->tick();
            f.game->tick();
            a = packets(*alice);
            const auto told_air = std::ranges::find_if(a, [](const auto& x) { return x.starts_with("air alice "); });
            require(told_air != a.end() && std::stoi(told_air->substr(10)) <= 291 && starting(a, "frozen alice") >= 1,
                    std::format("{}: rejoined: {}", version, text(a)));

            // Drowning to death: death.attack.drown.
            command(1, "fill 8 -62 8 8 -61 8 water[level=0]");
            std::tie(a, b) = ticks(800);
            require(has(a, "death screen") && has(a, "message death.attack.drown"), std::format("{}: drowned", version));
        }
    });

    suite.run("players attack players with fists and swords as both official servers show it (client/combat)", [] {
        // circuit-clienttest/combat.js: alice (8.5 -60 8.5, facing east)
        // attacks bob (10.5 -60 8.5, facing west) with carol beside him
        // (10.5 -60 9.5), all three in survival on a floor of dirt (the
        // fixture's world is empty).
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            GameFixture f(version, offline_config());
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const bool v772 = std::string_view(version) == "1.21.8";
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            auto carol = f.join(3, "carol");
            std::map<std::int32_t, std::string> names;
            std::map<std::string, std::int32_t> ids;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}, std::pair{carol, "carol"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) {
                        const auto e = pk->fields.i32().value();
                        names[e] = name;
                        ids[name] = e;
                    }
            }
            // Since the last call, as text: health, damage events (the type,
            // the causing and direct entity, a source position), hurt
            // animations, velocities, sounds, animations, particles, entity
            // events, slots, equipment, the shared flags and death messages.
            std::map<std::uint64_t, std::size_t> seen;
            std::int32_t last_teleport = 0; // alice's
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        out.push_back(std::format("health {}", in.f32().value()));
                    } else if (pk->id == id("player_position")) {
                        if (&r == alice.get()) last_teleport = in.varint().value();
                    } else if (pk->id == id("damage_event")) {
                        const auto e = in.varint().value(), type = in.varint().value(), cause = in.varint().value(), direct = in.varint().value();
                        out.push_back(std::format("damage {} {} by {} {}{}", who(e), type, cause ? who(cause - 1) : "none", direct ? who(direct - 1) : "none",
                                                  in.boolean().value() ? " from a place" : ""));
                    } else if (pk->id == id("hurt_animation")) {
                        const auto e = in.varint().value();
                        out.push_back(std::format("hurt animation {} {:.3f}", who(e), in.f32().value()));
                    } else if (pk->id == id("set_entity_motion")) {
                        const auto e = in.varint().value();
                        const auto x = in.i16().value(), y = in.i16().value(), z = in.i16().value();
                        out.push_back(std::format("velocity {} {} {} {}", who(e), x, y, z));
                    } else if (pk->id == id("sound")) {
                        out.push_back(std::format("sound {}", in.varint().value() - 1));
                    } else if (pk->id == id("animate")) {
                        const auto e = in.varint().value();
                        out.push_back(std::format("animation {} {}", who(e), in.u8().value()));
                    } else if (pk->id == id("level_particles")) {
                        const bool far = in.boolean().value(), always = in.boolean().value();
                        const auto x = in.f64().value(), y = in.f64().value(), z = in.f64().value();
                        const auto ox = in.f32().value(), oy = in.f32().value(), oz = in.f32().value(), speed = in.f32().value();
                        const auto n = in.i32().value();
                        out.push_back(std::format("particle {} at {:.3f} {:.3f} {:.3f} offset {:.3f} {:.3f} {:.3f} speed {:.3f} count {}{}{}", in.varint().value(), x, y,
                                                  z, ox, oy, oz, speed, n, far ? " far" : "", always ? " always" : ""));
                    } else if (pk->id == id("entity_event")) {
                        const auto e = in.i32().value();
                        if (const auto status = static_cast<int>(in.i8().value()); status < 24 || status > 28) out.push_back(std::format("status {} {}", who(e), status));
                    } else if (pk->id == id("container_set_slot")) {
                        if (in.varint().value() != 0) continue;
                        (void)in.varint();
                        const auto slot = in.i16().value();
                        const auto count = in.varint().value();
                        std::string line = std::format("slot {}", slot);
                        if (count > 0) {
                            line += " " + std::string(tables.item_name(in.varint().value()).value_or("?").substr(10));
                            if (in.varint().value() == 1) {
                                (void)in.varint(); // removed
                                (void)in.varint(); // the damage component
                                line += std::format(" damage {}", in.varint().value());
                            }
                        }
                        out.push_back(line);
                    } else if (pk->id == id("set_equipment")) {
                        out.push_back(std::format("equipment {}", who(in.varint().value())));
                    } else if (pk->id == id("set_entity_data")) {
                        const auto e = in.varint().value();
                        if (in.u8().value() == 0 && in.varint().value() == 0) out.push_back(std::format("flags {} {}", who(e), in.u8().value()));
                    } else if (pk->id == id("system_chat")) {
                        const std::string text(reinterpret_cast<const char*>(r.packets[at].data()), r.packets[at].size());
                        if (const auto k = text.find("death."); k != std::string::npos) {
                            auto e = k;
                            while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '.')) ++e;
                            out.push_back("message " + text.substr(k, e - k));
                        }
                    }
                }
                return out;
            };
            struct Seen {
                std::vector<std::string> alice, bob, carol;
            };
            const auto tick = [&] {
                f.game->tick();
                return Seen{packets(*alice), packets(*bob), packets(*carol)};
            };
            const auto ticks = [&](int n) {
                Seen all;
                for (int k = 0; k < n; ++k) {
                    auto t = tick();
                    all.alice.insert(all.alice.end(), t.alice.begin(), t.alice.end());
                    all.bob.insert(all.bob.end(), t.bob.begin(), t.bob.end());
                    all.carol.insert(all.carol.end(), t.carol.begin(), t.carol.end());
                }
                return all;
            };
            const auto has = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::find(v, x) != v.end(); };
            const auto count = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::count(v, x); };
            const auto starting = [](const std::vector<std::string>& v, std::string_view x) {
                return std::ranges::count_if(v, [&](const auto& s) { return s.starts_with(x); });
            };
            // Whether `v` has `want` in this order (others between).
            const auto in_order = [](const std::vector<std::string>& v, std::initializer_list<std::string> want) {
                auto at = v.begin();
                for (const auto& w : want) {
                    at = std::find(at, v.end(), w);
                    if (at == v.end()) return false;
                    ++at;
                }
                return true;
            };
            const auto text = [](const std::vector<std::string>& v) {
                std::string t;
                for (const auto& x : v) t += x + "; ";
                return t;
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const auto attack = [&](std::string_view target) {
                f.game->submit(1, I{server::intent::Attack{ids.at(std::string(target))}});
                f.game->submit(1, I{server::intent::Swing{0}});
            };
            const auto sound = [&](std::int32_t v772_id) { return std::format("sound {}", v772 ? v772_id : v772_id - 40); };
            const auto hurt = sound(1175), strong = sound(1168), nodamage = sound(1167), sweep = sound(1169), knockback = sound(1166), crit = sound(1165);
            const auto heal = [&](std::string_view who) { command(1, std::format("effect give {} minecraft:instant_health 1 3", who)); };
            const std::string hit_bob = "damage bob 34 by alice alice", hit_carol = "damage carol 34 by alice alice";
            for (const auto* c : {"gamerule naturalRegeneration false", "difficulty normal", "fill 4 -61 4 16 -61 12 dirt", "tp alice 8.5 -60 8.5 -90 0",
                                  "gamemode survival"})
                command(1, c);
            command(2, "tp bob 10.5 -60 8.5 90 0");
            command(2, "gamemode survival");
            command(3, "tp carol 10.5 -60 9.5 90 0");
            command(3, "gamemode survival");
            (void)ticks(2);
            f.game->submit(1, I{server::intent::ConfirmTeleport{last_teleport}});
            f.game->submit(1, I{server::intent::Move{std::nullopt, std::pair{-90.0f, 0.0f}, true}});
            f.game->submit(2, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            f.game->submit(3, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            (void)ticks(30);

            // The fist at full strength: the damage event naming alice, the
            // hurt animation and knockback to bob (his health at the next
            // tick), the attack sound; the others see the swing and are told
            // bob's velocity kept, the first time.
            attack("bob");
            auto t = tick();
            require(in_order(t.bob, {hit_bob, "hurt animation bob 90.000", "velocity bob 3200 2886 0", strong, "animation alice 0"}) && starting(t.bob, "health") == 0,
                    std::format("{}: bob hit: {}", version, text(t.bob)));
            require(in_order(t.alice, {hit_bob, hurt, strong, "velocity bob 0 -627 0"}) && starting(t.alice, "animation") == 0,
                    std::format("{}: alice: {}", version, text(t.alice)));
            require(in_order(t.carol, {hit_bob, hurt, strong, "animation alice 0", "velocity bob 0 -627 0"}), std::format("{}: carol: {}", version, text(t.carol)));
            t = tick();
            require(has(t.bob, "health 19"), std::format("{}: bob's health at the next tick", version));
            // Again 2 ticks later: within the hurt cooldown, the nodamage
            // sound; the swing within the first half of the last not shown.
            (void)ticks(30);
            attack("bob");
            (void)ticks(2);
            attack("bob");
            t = ticks(3);
            require(count(t.alice, hit_bob) == 0 && has(t.alice, nodamage) && count(t.carol, "animation alice 0") == 0 && !has(t.alice, "velocity bob 0 -627 0"),
                    std::format("{}: again at once: {}", version, text(t.carol)));
            // More within the hurt cooldown (strength II, 6 ticks later): only
            // the difference, without a damage event.
            heal("bob");
            (void)ticks(30);
            attack("bob");
            (void)tick();
            command(1, "effect give alice minecraft:strength 30 1");
            (void)ticks(5);
            attack("bob");
            t = ticks(2);
            require(starting(t.alice, "damage") == 0 && has(t.alice, strong) && !has(t.alice, hurt)
                        && has(t.alice, "particle 6 at 10.500 -59.100 8.500 offset 0.100 0.000 0.100 speed 0.200 count 3")
                        && has(t.bob, "health 13") && starting(t.bob, "hurt animation") == 0 && starting(t.bob, "velocity") == 0,
                    std::format("{}: harder within the cooldown: {} / {}", version, text(t.alice), text(t.bob)));
            command(1, "effect clear alice minecraft:strength");
            // Sprinting within the hurt cooldown: a weak fist (a swing at once
            // before it), then sprinting at full charge 6 ticks later: the
            // knockback sound, the sprint's knockback told to the others only
            // (0.5 along her facing), no damage event; two swings at once
            // both shown.
            heal("bob");
            (void)ticks(30);
            f.game->submit(1, I{server::intent::Swing{0}});
            attack("bob");
            t = tick();
            require(count(t.carol, "animation alice 0") == 2, std::format("{}: two swings at once: {}", version, text(t.carol)));
            f.game->submit(1, I{server::intent::Sprint{true}});
            (void)ticks(5);
            attack("bob");
            t = ticks(2);
            require(in_order(t.alice, {knockback, strong, "velocity bob 4000 3200 0"}) && starting(t.alice, "damage") == 0 && starting(t.bob, "velocity") == 0
                        && has(t.carol, "velocity bob 4000 3200 0") && has(t.bob, "flags alice 0") && has(t.bob, "health 19"),
                    std::format("{}: sprinting within the cooldown: {} / {}", version, text(t.alice), text(t.bob)));
            f.game->submit(1, I{server::intent::Sprint{false}});

            // An iron sword: bob hit, carol swept (1.21.8 the hurt's knockback
            // then the sweep's, 1.21.4 the other way), the sweep sound and
            // particle, the damage indicator; the sword's wear and what the
            // others see of it, then the health, at the next tick.
            command(1, "gamemode creative");
            (void)tick();
            f.game->submit(1, I{server::intent::CreativeSlot{36, 1, tables.item_id("minecraft:iron_sword").value(), 0}});
            (void)tick();
            command(1, "gamemode survival");
            heal("bob");
            (void)ticks(30);
            attack("bob");
            t = tick();
            const auto swept = v772 ? "velocity carol 4631 3200 715" : "velocity carol 4462 3200 1431";
            require(in_order(t.alice, {hit_bob, hurt, hit_carol, hurt, sweep, std::format("particle {} at 9.500 -59.100 8.500 offset 1.000 0.000 0.000 speed 0.000 count 0", v772 ? 64 : 63),
                                       "particle 6 at 10.500 -59.100 8.500 offset 0.100 0.000 0.100 speed 0.200 count 3", swept, swept})
                        && !has(t.alice, strong),
                    std::format("{}: a sweep: {}", version, text(t.alice)));
            require(has(t.carol, "hurt animation carol -243.435") && count(t.carol, swept) == 1, std::format("{}: carol swept: {}", version, text(t.carol)));
            t = tick();
            require(has(t.alice, "slot 36 iron_sword damage 1") && in_order(t.bob, {"equipment alice", "health 14"}) && in_order(t.carol, {"equipment alice", "health 19"}),
                    std::format("{}: the wear and the health: {} / {}", version, text(t.alice), text(t.bob)));
            // Again 4 ticks later: the nodamage sound, the second swing shown.
            heal("bob");
            heal("carol");
            (void)ticks(30);
            attack("bob");
            (void)ticks(4);
            attack("bob");
            t = ticks(3);
            require(has(t.alice, nodamage) && count(t.carol, "animation alice 0") == 1, std::format("{}: the swing 4 ticks later: {}", version, text(t.carol)));
            // An iron chestplate on bob: the next tick, bob's own slot, then
            // alice's sword, then his health; carol sees both pieces.
            heal("bob");
            heal("carol");
            command(2, "gamemode creative");
            (void)tick();
            f.game->submit(2, I{server::intent::CreativeSlot{6, 1, tables.item_id("minecraft:iron_chestplate").value(), 0}});
            (void)tick();
            command(2, "gamemode survival");
            (void)ticks(30);
            attack("bob");
            (void)tick();
            t = tick();
            require(in_order(t.bob, {"slot 6 iron_chestplate damage 1", "equipment alice", "health 14.72"}) && in_order(t.carol, {"equipment alice", "equipment bob", "health 19"})
                        && in_order(t.alice, {"slot 36 iron_sword damage 3", "equipment bob"}),
                    std::format("{}: two players' wear: {} / {}", version, text(t.bob), text(t.carol)));
            command(2, "gamemode creative");
            (void)tick();
            f.game->submit(2, I{server::intent::CreativeSlot{6, 0, 0, 0}});
            (void)tick();
            command(2, "gamemode survival");
            // A sweep reaching carol within her hurt cooldown (bob's fist on
            // her 3 ticks before): 1.21.8 does not knock her, 1.21.4 does,
            // telling only the others.
            heal("bob");
            heal("carol");
            (void)ticks(30);
            f.game->submit(2, I{server::intent::Attack{ids.at("carol")}});
            f.game->submit(2, I{server::intent::Swing{0}});
            t = tick();
            require(has(t.carol, "damage carol 34 by bob bob") && has(t.carol, "hurt animation carol -180.000"), std::format("{}: bob hits carol", version));
            (void)ticks(2);
            attack("bob");
            t = ticks(2);
            require(has(t.alice, sweep) && starting(t.alice, "damage carol") == 0 && has(t.alice, "velocity carol 3200 2886 0") == !v772
                        && starting(t.carol, "velocity carol") == 0,
                    std::format("{}: a sweep within carol's cooldown: {}", version, text(t.alice)));
            // Walking (about 0.2 a tick westwards) still sweeps.
            heal("bob");
            heal("carol");
            (void)ticks(30);
            for (int k = 1; k <= 3; ++k) {
                f.game->submit(1, I{server::intent::Move{server::Vec3{8.5 - 0.2 * k, -60.0, 8.5}, std::nullopt, true}});
                (void)tick();
            }
            attack("bob");
            t = ticks(2);
            require(has(t.alice, sweep) && !has(t.alice, strong), std::format("{}: walking: {}", version, text(t.alice)));
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -60.0, 8.5}, std::nullopt, true}});
            (void)tick();
            // In creative mode the sword does not wear.
            heal("bob");
            command(1, "gamemode creative");
            (void)ticks(30);
            attack("bob");
            t = ticks(3);
            require(has(t.alice, hit_bob) && starting(t.alice, "slot 36") == 0, std::format("{}: no wear in creative mode", version));
            command(1, "gamemode survival");
            // The held item swapped (combat-timing): in the swap's tick the
            // sword's damage and speed still hit (6, no sweep with nothing in
            // hand); one tick after the swap's, the fist's charge of 1 tick.
            heal("bob");
            heal("carol");
            (void)ticks(30);
            f.game->submit(1, I{server::intent::HeldSlot{1}});
            attack("carol");
            t = ticks(2);
            require(has(t.carol, "health 14") && !has(t.alice, sweep) && has(t.alice, strong), std::format("{}: swapped at once: {}", version, text(t.carol)));
            f.game->submit(1, I{server::intent::HeldSlot{0}});
            heal("carol");
            (void)ticks(30);
            f.game->submit(1, I{server::intent::HeldSlot{1}});
            (void)ticks(2);
            attack("carol");
            t = ticks(2);
            require(has(t.carol, "health 19.728"), std::format("{}: the charge after a swap: {}", version, text(t.carol)));
            f.game->submit(1, I{server::intent::HeldSlot{0}});

            // Sprinting: the knockback sound before the hurt, 0.5 more along
            // her facing; her sprint ends.
            heal("bob");
            heal("carol");
            (void)ticks(30);
            f.game->submit(1, I{server::intent::Sprint{true}});
            t = tick();
            require(has(t.bob, "flags alice 8"), std::format("{}: sprinting shown", version));
            attack("bob");
            t = ticks(2);
            require(in_order(t.alice, {knockback, hit_bob, strong}) && has(t.bob, "velocity bob 5600 3200 0") && has(t.bob, "flags alice 0") && !has(t.alice, hit_carol),
                    std::format("{}: a sprint's knockback: {} / {}", version, text(t.alice), text(t.bob)));

            // A jump, its velocity told to the others; falling, a critical
            // hit (9): its sound and animation.
            heal("bob");
            (void)ticks(30);
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -59.58, 8.5}, std::nullopt, false}});
            t = tick();
            require(has(t.bob, "velocity alice 0 3359 0") && has(t.carol, "velocity alice 0 3359 0") && starting(t.alice, "velocity alice") == 0,
                    std::format("{}: a jump: {}", version, text(t.bob)));
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -59.7, 8.5}, std::nullopt, false}});
            attack("bob");
            t = ticks(2);
            require(in_order(t.alice, {hit_bob, crit, "animation bob 4", "particle 6 at 10.500 -59.100 8.500 offset 0.100 0.000 0.100 speed 0.200 count 4"})
                        && has(t.bob, "health 11") && has(t.carol, "animation bob 4"),
                    std::format("{}: a critical hit: {}", version, text(t.alice)));
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -60.0, 8.5}, std::nullopt, true}});

            // Weakness I with the fist: nothing but the swing.
            heal("bob");
            command(1, "effect give alice minecraft:weakness 30 0");
            f.game->submit(1, I{server::intent::HeldSlot{1}});
            (void)ticks(30);
            attack("bob");
            t = ticks(2);
            require(t.alice.empty() && t.bob == std::vector<std::string>{"animation alice 0"}, std::format("{}: weakness: {} / {}", version, text(t.alice), text(t.bob)));
            command(1, "effect clear alice minecraft:weakness");
            // bob in creative mode: the nodamage sound.
            command(2, "gamemode creative");
            (void)ticks(30);
            attack("bob");
            t = ticks(2);
            require(has(t.alice, nodamage) && starting(t.alice, "damage") == 0, std::format("{}: creative bob", version));
            command(2, "gamemode survival");
            // Reach: 5 blocks away hit, 7 not (not even the nodamage sound).
            command(2, "tp bob 13.5 -60 8.5 90 0");
            (void)ticks(30);
            attack("bob");
            t = ticks(2);
            require(has(t.alice, hit_bob), std::format("{}: 5 blocks away", version));
            command(2, "tp bob 15.5 -60 8.5 90 0");
            (void)ticks(30);
            attack("bob");
            t = ticks(2);
            require(starting(t.alice, "damage") == 0 && !has(t.alice, nodamage) && has(t.bob, "animation alice 0"), std::format("{}: 7 blocks away", version));
            command(2, "tp bob 10.5 -60 8.5 90 0");
            // A swing starts the charge again: the fist on carol in its tick
            // keeps 0.208 of 1.
            heal("carol");
            (void)ticks(30);
            f.game->submit(1, I{server::intent::Swing{0}});
            attack("carol");
            t = ticks(2);
            require(has(t.carol, "health 19.792"), std::format("{}: the charge after a swing: {}", version, text(t.carol)));

            // To bob's death: strength II and the sword, twice.
            heal("bob");
            f.game->submit(1, I{server::intent::HeldSlot{0}});
            command(1, "effect give alice minecraft:strength 30 1");
            (void)ticks(30);
            attack("bob");
            t = ticks(30);
            require(has(t.bob, "health 8"), std::format("{}: 12 of 20", version));
            attack("bob");
            t = ticks(2);
            require(has(t.alice, "message death.attack.player") && has(t.carol, "status bob 3") && has(t.bob, "message death.attack.player"),
                    std::format("{}: killed: {}", version, text(t.alice)));
        }
    });

    suite.run("players block attacks with shields as both official servers show it (client/shield)", [] {
        // circuit-clienttest/shield.js: alice (8.5 -60 8.5, facing east) hits
        // bob (10.5 -60 8.5, facing west), who raises a shield in his off
        // hand, with carol beside him; all three in survival on a floor.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            GameFixture f(version, offline_config());
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const bool v772 = std::string_view(version) == "1.21.8";
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            auto carol = f.join(3, "carol");
            std::map<std::int32_t, std::string> names;
            std::map<std::string, std::int32_t> ids;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}, std::pair{carol, "carol"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) {
                        const auto e = pk->fields.i32().value();
                        names[e] = name;
                        ids[name] = e;
                    }
            }
            // Since the last call, as text: health, damage events, velocities,
            // sounds, entity events, the living flags, cooldowns and slots.
            std::map<std::uint64_t, std::size_t> seen;
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        out.push_back(std::format("health {}", in.f32().value()));
                    } else if (pk->id == id("damage_event")) {
                        out.push_back(std::format("damage {}", who(in.varint().value())));
                    } else if (pk->id == id("set_entity_motion")) {
                        const auto e = in.varint().value();
                        const auto x = in.i16().value(), y = in.i16().value(), z = in.i16().value();
                        out.push_back(std::format("velocity {} {} {} {}", who(e), x, y, z));
                    } else if (pk->id == id("sound")) {
                        const auto sound = in.varint().value() - 1, category = in.varint().value();
                        (void)in.i32(), (void)in.i32(), (void)in.i32();
                        out.push_back(std::format("sound {} category {} volume {}", sound, category, in.f32().value()));
                    } else if (pk->id == id("set_equipment")) {
                        out.push_back(std::format("equipment {}", who(in.varint().value())));
                    } else if (pk->id == id("entity_event")) {
                        const auto e = in.i32().value();
                        if (const auto status = static_cast<int>(in.i8().value()); status < 24 || status > 28) out.push_back(std::format("status {} {}", who(e), status));
                    } else if (pk->id == id("set_entity_data")) {
                        const auto e = in.varint().value();
                        const auto index = in.u8().value();
                        if (in.varint().value() != 0) continue;
                        if (index == 8) out.push_back(std::format("living {} {}", who(e), in.u8().value()));
                        if (index == 0) out.push_back(std::format("flags {} {}", who(e), in.u8().value()));
                    } else if (pk->id == id("cooldown")) {
                        const auto group = in.string(32767).value();
                        out.push_back(std::format("cooldown {} {}", group, in.varint().value()));
                    } else if (pk->id == id("container_set_slot")) {
                        if (in.varint().value() != 0) continue;
                        (void)in.varint();
                        const auto slot = in.i16().value();
                        std::string line = std::format("slot {}", slot);
                        if (in.varint().value() > 0) {
                            (void)in.varint();
                            if (in.varint().value() == 1) {
                                (void)in.varint();
                                (void)in.varint();
                                line += std::format(" damage {}", in.varint().value());
                            }
                        }
                        out.push_back(line);
                    }
                }
                return out;
            };
            struct Seen {
                std::vector<std::string> alice, bob, carol;
            };
            const auto tick = [&] {
                f.game->tick();
                return Seen{packets(*alice), packets(*bob), packets(*carol)};
            };
            const auto ticks = [&](int n) {
                Seen all;
                for (int k = 0; k < n; ++k) {
                    auto t = tick();
                    all.alice.insert(all.alice.end(), t.alice.begin(), t.alice.end());
                    all.bob.insert(all.bob.end(), t.bob.begin(), t.bob.end());
                    all.carol.insert(all.carol.end(), t.carol.begin(), t.carol.end());
                }
                return all;
            };
            const auto has = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::find(v, x) != v.end(); };
            const auto starting = [](const std::vector<std::string>& v, std::string_view x) {
                return std::ranges::count_if(v, [&](const auto& s) { return s.starts_with(x); });
            };
            const auto in_order = [](const std::vector<std::string>& v, std::initializer_list<std::string> want) {
                auto at = v.begin();
                for (const auto& w : want) {
                    at = std::find(at, v.end(), w);
                    if (at == v.end()) return false;
                    ++at;
                }
                return true;
            };
            const auto text = [](const std::vector<std::string>& v) {
                std::string t;
                for (const auto& x : v) t += x + "; ";
                return t;
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const auto attack = [&] {
                f.game->submit(1, I{server::intent::Attack{ids.at("bob")}});
                f.game->submit(1, I{server::intent::Swing{0}});
            };
            std::int32_t sequence = 0;
            const auto raise = [&](std::int32_t hand = 1) { f.game->submit(2, I{server::intent::Use{hand, ++sequence, 90.0f, 0.0f}}); };
            const auto lower = [&] { f.game->submit(2, I{server::intent::Dig{5, {}, 0, 0}}); };
            const auto give = [&](std::uint64_t who, std::int16_t slot, std::string_view item) {
                command(who, "gamemode creative");
                (void)tick();
                f.game->submit(who, I{server::intent::CreativeSlot{slot, item.empty() ? 0 : 1, item.empty() ? 0 : tables.item_id(item).value(), 0}});
                (void)tick();
                command(who, "gamemode survival");
                (void)tick();
            };
            const auto sound = [&](std::int32_t v772_id, std::int32_t v769_id, std::string_view volume = "1") {
                return std::format("sound {} category 7 volume {}", v772 ? v772_id : v769_id, volume);
            };
            const auto hurt = sound(1175, 1135), nodamage = sound(1167, 1127), blocked = sound(1298, 1257), broken = sound(1299, 1258, "0.8"),
                       equip = sound(70, 70), knockback = sound(1166, 1126);
            const auto none_of = [&](const Seen& t, std::string_view x) { return !has(t.alice, x) && !has(t.bob, x) && !has(t.carol, x); };
            for (const auto* c : {"gamerule naturalRegeneration false", "difficulty normal", "fill 4 -61 4 16 -61 12 dirt", "tp alice 8.5 -60 8.5 -90 0",
                                  "gamemode survival"})
                command(1, c);
            command(2, "tp bob 10.5 -60 8.5 90 0");
            command(2, "gamemode survival");
            command(3, "tp carol 10.5 -60 9.5 90 0");
            command(3, "gamemode survival");
            (void)ticks(2);
            f.game->submit(2, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            f.game->submit(3, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            // A shield into the off hand: the generic equip sound.
            command(2, "gamemode creative");
            (void)tick();
            f.game->submit(2, I{server::intent::CreativeSlot{45, 1, tables.item_id("minecraft:shield").value(), 0}});
            auto t = ticks(2);
            require(has(t.alice, equip) && has(t.bob, equip), std::format("{}: the shield's equip sound: {}", version, text(t.alice)));
            command(2, "gamemode survival");
            (void)ticks(30);
            const auto block_sign = v772 ? blocked : std::string("status bob 29");
            // The fist on the raised shield: blocked, the shield not worn (1).
            raise();
            (void)ticks(12);
            attack();
            t = ticks(2);
            require(in_order(t.alice, {block_sign, hurt, nodamage, "velocity bob 1200 3200 0"}) && has(t.bob, block_sign) && starting(t.bob, "slot 45") == 0,
                    std::format("{}: the fist blocked: {} / {}", version, text(t.alice), text(t.bob)));
            lower();
            (void)ticks(30);
            // A sword into the main hand: no equip sound.
            command(1, "gamemode creative");
            (void)tick();
            f.game->submit(1, I{server::intent::CreativeSlot{36, 1, tables.item_id("minecraft:iron_sword").value(), 0}});
            t = ticks(2);
            require(none_of(t, equip), std::format("{}: no equip sound for the main hand", version));
            command(1, "gamemode survival");
            (void)ticks(30);

            // Raised 2 ticks before the sword: not yet blocking.
            raise();
            t = tick();
            require(has(t.alice, "living bob 3") && has(t.bob, "living bob 3"), std::format("{}: raised: {}", version, text(t.alice)));
            (void)tick();
            attack();
            t = ticks(2);
            require(has(t.alice, "damage bob") && has(t.bob, "velocity bob 3200 2886 0"), std::format("{}: too soon: {}", version, text(t.bob)));
            lower();
            t = tick();
            require(has(t.alice, "living bob 2"), std::format("{}: lowered", version));
            command(1, "effect give bob minecraft:instant_health 1 3");
            command(1, "effect give carol minecraft:instant_health 1 3");
            (void)ticks(30);
            // Raised: the sword blocked (the block's sound or entity event,
            // the hurt sound for the others, nodamage, no damage event, his
            // velocity to the others only, the shield's wear of 7).
            raise();
            (void)ticks(12);
            attack();
            t = ticks(2);
            require(in_order(t.alice, {block_sign, hurt, nodamage}) && starting(t.alice, "damage") == 0 && starting(t.alice, "slot 36") == 0
                        && has(t.bob, block_sign) && starting(t.bob, "velocity") == 0 && starting(t.bob, "health") == 0 && has(t.bob, "slot 45 damage 7")
                        && none_of(t, equip),
                    std::format("{}: blocked: {} / {}", version, text(t.alice), text(t.bob)));
            // Lowered and hit again 6 ticks later: the hurt cooldown the block
            // started; the weak damage taken silently, the health after the
            // sword's wear others see.
            lower();
            (void)ticks(4);
            attack();
            t = tick();
            require(starting(t.alice, "damage") == 0 && !has(t.alice, hurt) && starting(t.bob, "health") == 0,
                    std::format("{}: after the block: {}", version, text(t.bob)));
            t = tick();
            require(in_order(t.bob, {"equipment alice", "health 17.502079"}) && has(t.alice, "slot 36 damage 2"),
                    std::format("{}: the health after the wear: {}", version, text(t.bob)));
            // Turned away by a teleport: hurt; 1.21.8 knocks him 0.5 towards
            // her first.
            command(1, "effect give bob minecraft:instant_health 1 3");
            command(2, "tp bob 10.5 -60 8.5 -90 0");
            (void)ticks(30);
            raise();
            (void)ticks(12);
            attack();
            t = ticks(2);
            require(has(t.alice, "damage bob") && has(t.bob, v772 ? "velocity bob 1200 3200 0" : "velocity bob 3200 2886 0"),
                    std::format("{}: turned away: {}", version, text(t.bob)));
            lower();
            command(2, "tp bob 10.5 -60 8.5 90 0");
            // Alice 2 west 1 south of him: blocked, his velocity to the others.
            command(1, "effect give bob minecraft:instant_health 1 3");
            command(1, "tp alice 8.5 -60 9.5 -90 0");
            (void)ticks(30);
            raise();
            (void)ticks(12);
            attack();
            t = ticks(2);
            require(has(t.bob, block_sign) && has(t.carol, "velocity bob 1073 3200 -536"), std::format("{}: off his line: {}", version, text(t.carol)));
            lower();
            command(1, "tp alice 8.5 -60 8.5 -90 0");
            // Sprinting on the raised shield: the knockback sound first, the
            // sprint going on.
            (void)ticks(30);
            f.game->submit(1, I{server::intent::Sprint{true}});
            raise();
            (void)ticks(12);
            attack();
            t = ticks(3);
            require(in_order(t.alice, {knockback, block_sign, hurt, nodamage}) && !has(t.bob, "flags alice 0"),
                    std::format("{}: sprinting on it: {}", version, text(t.alice)));
            f.game->submit(1, I{server::intent::Sprint{false}});
            lower();
            (void)tick();
            // From behind (alice east of him): hurt; 1.21.8 knocks him 0.5
            // towards her first.
            command(1, "effect give bob minecraft:instant_health 1 3");
            command(1, "tp alice 12.5 -60 8.5 90 0");
            (void)ticks(30);
            raise();
            (void)ticks(12);
            attack();
            t = ticks(2);
            require(has(t.alice, "damage bob") && has(t.bob, v772 ? "velocity bob -1200 3200 0" : "velocity bob -3200 2886 0"),
                    std::format("{}: from behind: {}", version, text(t.bob)));
            lower();
            command(1, "tp alice 8.5 -60 8.5 -90 0");
            command(1, "effect give bob minecraft:instant_health 1 3");
            // An iron axe: the shield disabled (its cooldown first; 1.21.8 the
            // break sound and the damage event, 1.21.4 entity events 30 and
            // 29), lowered, worn by 10; raised again at once: not raised.
            give(1, 36, "minecraft:iron_axe");
            (void)ticks(30);
            raise();
            (void)ticks(12);
            attack();
            t = ticks(2);
            require(v772 ? in_order(t.bob, {"cooldown minecraft:shield 100", broken, "damage bob", nodamage})
                         : in_order(t.bob, {"cooldown minecraft:shield 100", "status bob 30", "status bob 29", nodamage}),
                    std::format("{}: disabled: {}", version, text(t.bob)));
            require(has(t.alice, "living bob 2") && has(t.bob, "slot 45 damage 31") && starting(t.bob, "health") == 0 && starting(t.alice, "slot 36") == 0,
                    std::format("{}: lowered and worn: {}", version, text(t.bob)));
            raise();
            t = ticks(2);
            require(!has(t.alice, "living bob 3"), std::format("{}: not raised within the cooldown", version));
            t = ticks(100);
            require(has(t.bob, "cooldown minecraft:shield 0"), std::format("{}: the cooldown's end", version));

            // The shield in the main hand (its slot emptied in the off hand):
            // no equip sound; raised (living 1), blocking, worn in its slot;
            // lowered (0). Another hotbar slot lowers it (Circuit's choice, as
            // eating ends).
            command(1, "effect give bob minecraft:instant_health 1 3");
            give(1, 36, "minecraft:iron_sword");
            command(2, "gamemode creative");
            (void)tick();
            f.game->submit(2, I{server::intent::CreativeSlot{45, 0, 0, 0}});
            f.game->submit(2, I{server::intent::CreativeSlot{36, 1, tables.item_id("minecraft:shield").value(), 0}});
            t = ticks(2);
            require(none_of(t, equip), std::format("{}: no equip sound for a shield in the main hand", version));
            command(2, "gamemode survival");
            (void)ticks(30);
            raise(0);
            t = tick();
            require(has(t.alice, "living bob 1"), std::format("{}: raised in the main hand: {}", version, text(t.alice)));
            (void)ticks(11);
            attack();
            t = ticks(2);
            require(has(t.bob, block_sign) && has(t.bob, "slot 36 damage 7") && starting(t.alice, "damage") == 0,
                    std::format("{}: blocked from the main hand: {}", version, text(t.bob)));
            lower();
            t = tick();
            require(has(t.alice, "living bob 0"), std::format("{}: lowered from the main hand", version));
            // Another hotbar slot lowers it; an attack in that tick is not
            // blocked (Circuit's choice: not recorded).
            command(1, "effect give bob minecraft:instant_health 1 3");
            (void)ticks(30);
            raise(0);
            (void)ticks(12);
            f.game->submit(2, I{server::intent::HeldSlot{1}});
            attack();
            t = tick();
            require(has(t.alice, "living bob 0") && has(t.alice, "damage bob") && !has(t.bob, block_sign),
                    std::format("{}: another hotbar slot lowers it: {}", version, text(t.alice)));
            f.game->submit(2, I{server::intent::HeldSlot{0}});
            // Death lowers it at once (Circuit's choice: not recorded); after
            // the respawn a sword hit is not blocked.
            raise(0);
            (void)ticks(12);
            command(2, "kill");
            t = ticks(2);
            require(has(t.alice, "living bob 0") && has(t.bob, "living bob 0"), std::format("{}: lowered by death: {}", version, text(t.alice)));
            f.game->submit(2, I{server::intent::ClientCommand{0}});
            (void)ticks(40);
            command(2, "tp bob 10.5 -60 8.5 90 0");
            f.game->submit(2, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            (void)ticks(30);
            attack();
            t = ticks(2);
            require(has(t.alice, "damage bob"), std::format("{}: not raised after death: {}", version, text(t.alice)));
        }
    });

    suite.run("enchantments give players attribute modifiers, and speed digging and keep air, as both official servers do (client/ench_attributes)", [] {
        // circuit-clienttest/ench-attributes.js, ench-dig.js and ench-air.js;
        // enchantments need the official data packs (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        using I = server::Intent;
        bool ran = false;
        for (const auto& [version, variable] : {std::pair{"1.21.8", "CIRCUIT_VANILLA_JAR_1_21_8"}, std::pair{"1.21.4", "CIRCUIT_VANILLA_JAR_1_21_4"}}) {
            const char* jar = std::getenv(variable);
            if (!jar || !*jar) continue;
            ran = true;
            const auto& bundle = **contracts::BundleRegistry::find(std::format("java-{}", version));
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value() && d->game, d ? "no game data" : d.error());
            const auto enchanted = d->game->enchanted;
            GameFixture f(version, offline_config(), std::move(*d));
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const bool v772 = std::string_view(version) == "1.21.8";
            const auto item = [&](std::string_view name) { return tables.item_id(name).value(); };
            const auto stack = [&](std::string_view name, std::vector<std::pair<std::string, std::int32_t>> levels, std::int16_t slot) {
                auto patch = levels.empty() ? std::optional<contracts::ComponentPatch>(contracts::ComponentPatch{}) : enchanted(levels);
                require(patch.has_value(), "enchanted");
                return I{server::intent::CreativeSlot{slot, 1, item(name), 0, *patch}};
            };
            const auto entity_of = [&](Recorder& r) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r.mutex);
                std::int32_t e = -1;
                for (const auto& bytes : r.packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) e = pk->fields.i32().value();
                return e;
            };
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::int32_t entity = entity_of(*alice);
            // The attribute packets of alice a client got since the last
            // call: per packet, each attribute "id base [modifier amount
            // operation ...]"; and the sounds it heard and alice's equipment
            // it was sent.
            std::map<std::uint64_t, std::size_t> seen;
            const auto packets = [&](Recorder& r) {
                const auto attributes = f.wire.ids().id(State::play, Bound::clientbound, "update_attributes");
                const auto sound = f.wire.ids().id(State::play, Bound::clientbound, "sound");
                const auto equipment = f.wire.ids().id(State::play, Bound::clientbound, "set_equipment");
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == sound) {
                        out.push_back("sound");
                        continue;
                    }
                    if (pk->id == equipment) {
                        if (in.varint().value() == entity) out.push_back("equipment");
                        continue;
                    }
                    if (pk->id != attributes || in.varint().value() != entity) continue;
                    std::string line;
                    for (auto n = in.varint().value(); n > 0; --n) {
                        const auto id = in.varint().value();
                        line += std::format("{} {}", id, in.f64().value());
                        for (auto m = in.varint().value(); m > 0; --m) {
                            const auto mid = in.string(32767).value();
                            const auto amount = in.f64().value();
                            line += std::format(" [{} {} {}]", mid, amount, static_cast<int>(in.u8().value()));
                        }
                        line += "; ";
                    }
                    out.push_back(line);
                }
                return out;
            };
            const auto joined = [](const std::vector<std::string>& v) {
                std::string all;
                for (const auto& p : v) all += p + "| ";
                return all;
            };
            const auto step = [&](I intent) {
                f.game->submit(1, std::move(intent));
                for (int k = 0; k < 3; ++k) f.game->tick();
                (void)packets(*bob);
                return joined(packets(*alice));
            };
            const auto has = [](const std::string& all, std::string_view part) { return all.find(part) != std::string::npos; };
            const auto id = [&](std::int32_t later, std::int32_t earlier) { return v772 ? later : earlier; };
            f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
            f.game->submit(2, I{server::intent::ConfirmTeleport{1}});
            (void)step(I{server::intent::Command{"gamemode creative"}});
            // Each attribute's id, base and the modifier: its id with the
            // slot, the level's amount in floats, its operation.
            const std::vector<std::tuple<std::string_view, std::vector<std::pair<std::string, std::int32_t>>, std::int16_t, std::string>> pieces{
                {"minecraft:iron_helmet", {{"minecraft:respiration", 3}}, 5, std::format("{} 0 [minecraft:enchantment.respiration/head 3 0]", id(23, 22))},
                {"minecraft:iron_helmet", {{"minecraft:aqua_affinity", 1}}, 5, std::format("{} 0.2 [minecraft:enchantment.aqua_affinity/head 4 2]", id(29, 28))},
                {"minecraft:iron_leggings", {{"minecraft:swift_sneak", 3}}, 7, std::format("{} 0.3 [minecraft:enchantment.swift_sneak/legs 0.45000001788139343 0]", id(26, 25))},
                {"minecraft:iron_boots", {{"minecraft:depth_strider", 2}}, 8, std::format("{} 0 [minecraft:enchantment.depth_strider/feet 0.6666666865348816 0]", id(32, 31))},
                {"minecraft:iron_chestplate", {{"minecraft:fire_protection", 2}}, 6, "7 1 [minecraft:enchantment.fire_protection/chest -0.30000001192092896 1]"},
                {"minecraft:iron_boots", {{"minecraft:blast_protection", 4}}, 8, std::format("{} 0 [minecraft:enchantment.blast_protection/feet 0.6000000238418579 0]", id(9, 8))},
                {"minecraft:iron_sword", {{"minecraft:sweeping_edge", 3}}, 36, std::format("{} 0 [minecraft:enchantment.sweeping_edge/mainhand 0.75 0]", id(30, 29))},
                {"minecraft:stick", {{"minecraft:efficiency", 5}}, 36, std::format("{} 0 [minecraft:enchantment.efficiency/mainhand 26 0]", id(20, 19))},
            };
            for (const auto& [name, levels, slot, expected] : pieces) {
                const auto all = step(stack(name, levels, slot));
                require(has(all, expected), std::format("{}: {} expected; {}", version, expected, all));
            }
            // A slot emptied: its modifiers go.
            auto all = step(I{server::intent::CreativeSlot{6, 0, 0}});
            require(has(all, "7 1;") && !has(all, "fire_protection"), std::format("{}: the chestplate off: {}", version, all));
            // The same item, enchanted otherwise: put on again, with its sound.
            all = step(stack("minecraft:iron_helmet", {{"minecraft:respiration", 2}}, 5));
            require(has(all, "sound") && has(all, std::format("{} 0.2;", id(29, 28))) && has(all, "[minecraft:enchantment.respiration/head 2 0]"),
                    std::format("{}: another helmet: {}", version, all));
            // In the enchantment's slots only.
            all = step(stack("minecraft:iron_chestplate", {{"minecraft:swift_sneak", 1}}, 6));
            require(!has(all, "swift_sneak/chest"), std::format("{}: swift sneak on a chestplate: {}", version, all));
            all = step(stack("minecraft:diamond_pickaxe", {{"minecraft:efficiency", 5}}, 45));
            require(!has(all, "efficiency/offhand"), std::format("{}: efficiency in the off hand: {}", version, all));
            // A rejoin: the attributes without the modifiers of what she wears
            // and holds, which follow in a later packet; bob sees her appear
            // with her equipment.
            f.game->leave(1);
            f.game->tick();
            (void)packets(*bob);
            alice = f.join(1, "alice");
            seen.erase(1);
            entity = entity_of(*alice);
            const auto first = packets(*alice);
            const auto appeared = joined(packets(*bob));
            require(!first.empty() && has(first.front(), std::format("{} 0;", id(23, 22))) && !has(joined(first), "enchantment."),
                    std::format("{}: arriving without the modifiers: {}", version, joined(first)));
            require(has(appeared, "equipment") && !has(appeared, "enchantment."), std::format("{}: bob sees her appear dressed: {}", version, appeared));
            // In the next tick a helmet put on plays its sound, and the
            // modifiers come.
            f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
            all = step(stack("minecraft:iron_helmet", {{"minecraft:respiration", 3}}, 5));
            require(has(all, "sound") && has(all, "[minecraft:enchantment.respiration/head 3 0]") && has(all, "[minecraft:enchantment.efficiency/mainhand 26 0]"),
                    std::format("{}: then the modifiers: {}", version, all));

            // Digging (ench-dig.js): obsidian with an efficiency V diamond
            // pickaxe, finished after 40 ticks: at once; with a plain one not.
            (void)step(I{server::intent::Command{"gamemode survival"}});
            (void)step(I{server::intent::Command{"tp alice 0.5 -60 0.5"}});
            f.game->submit(1, I{server::intent::ConfirmTeleport{2}});
            (void)step(I{server::intent::Move{std::nullopt, std::nullopt, true}}); // on the ground
            const auto dig = [&](std::int32_t x, std::int32_t ticks) {
                f.game->submit(1, I{server::intent::Command{std::format("setblock {} -60 0 minecraft:obsidian", x)}});
                f.game->tick();
                f.game->submit(1, I{server::intent::Dig{0, {x, -60, 0}, 1, 1}});
                for (int k = 0; k < ticks; ++k) f.game->tick();
                f.game->submit(1, I{server::intent::Dig{2, {x, -60, 0}, 1, 2}});
                f.game->tick();
                f.game->tick();
                return f.at({x, -60, 0});
            };
            (void)step(I{server::intent::Command{"gamemode creative"}});
            (void)step(stack("minecraft:diamond_pickaxe", {{"minecraft:efficiency", 5}}, 36));
            (void)step(I{server::intent::Command{"gamemode survival"}});
            require(dig(2, 40) == "minecraft:air", std::format("{}: efficiency V digs obsidian in 40 ticks", version));
            (void)step(I{server::intent::Command{"gamemode creative"}});
            (void)step(stack("minecraft:diamond_pickaxe", {}, 36));
            (void)step(I{server::intent::Command{"gamemode survival"}});
            require(dig(3, 40) == "minecraft:obsidian", std::format("{}: a plain pickaxe does not", version));

            // Respiration III (ench-air.js): under water without water
            // breathing, about a quarter of the ticks lose air; without it
            // every tick.
            const auto air_lost = [&](std::int32_t ticks) {
                const auto metadata = f.wire.ids().id(State::play, Bound::clientbound, "set_entity_data");
                std::int32_t lowest = 300;
                f.game->submit(1, I{server::intent::Command{"tp alice 11.5 -60 11.5"}});
                for (int k = 0; k < ticks; ++k) f.game->tick();
                f.game->submit(1, I{server::intent::Command{"tp alice 0.5 -60 0.5"}});
                f.game->tick();
                std::lock_guard lock(alice->mutex);
                for (auto& at = seen[alice->peer_id]; at < alice->packets.size(); ++at) {
                    auto pk = protocol::open_packet(alice->packets[at]);
                    if (!pk || pk->id != metadata || pk->fields.varint().value() != entity) continue;
                    if (pk->fields.u8().value() == 1 && pk->fields.varint().value() == 1) lowest = std::min(lowest, pk->fields.varint().value());
                }
                return 300 - lowest;
            };
            (void)step(I{server::intent::Command{"fill 10 -60 10 12 -58 12 minecraft:water[level=0]"}});
            (void)step(I{server::intent::Command{"gamemode creative"}});
            (void)step(I{server::intent::CreativeSlot{5, 0, 0}});
            (void)step(I{server::intent::Command{"gamemode survival"}});
            const auto plain = air_lost(120);
            require(plain >= 118 && plain <= 121, std::format("{}: one air a tick without respiration: {}", version, plain));
            for (int k = 0; k < 100; ++k) f.game->tick(); // breathe again
            (void)step(I{server::intent::Command{"gamemode creative"}});
            (void)step(stack("minecraft:iron_helmet", {{"minecraft:respiration", 3}}, 5));
            (void)step(I{server::intent::Command{"gamemode survival"}});
            const auto kept = air_lost(400);
            require(kept >= 60 && kept <= 140, std::format("{}: respiration III loses about a quarter (100 of 400): {}", version, kept));
        }
        if (!ran) std::puts("  (no official server jar given: the enchantment checks did not run)");
    });

    suite.run("data packs whose enchantments ask for what Circuit cannot do are refused", [] {
        const char* jar = std::getenv("CIRCUIT_VANILLA_JAR_1_21_8");
        if (!jar || !*jar) {
            std::puts("  (no official 1.21.8 server jar given: the enchantment refusal checks did not run)");
            return;
        }
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        const auto pack = std::filesystem::temp_directory_path() / std::format("circuit-ench-pack-{}", ::getpid());
        const auto attempt = [&](std::string_view definition) {
            std::filesystem::remove_all(pack);
            std::filesystem::create_directories(pack / "data/minecraft/enchantment");
            std::ofstream(pack / "pack.mcmeta") << R"({"pack":{"pack_format":81,"description":"test"}})";
            std::ofstream(pack / "data/minecraft/enchantment/respiration.json") << definition;
            auto d = server::load_server_data(bundle, {jar, {pack}});
            std::filesystem::remove_all(pack);
            return d.has_value() ? std::string("loaded") : d.error();
        };
        const std::string head = R"({"description":"r","supported_items":"#minecraft:enchantable/head_armor","weight":2,"max_level":3,"min_cost":{"base":10,"per_level_above_first":10},"max_cost":{"base":40,"per_level_above_first":10},"anvil_cost":4,"slots":["head"],"effects":{"minecraft:attributes":[)";
        const auto with = [&](std::string_view effect) { return head + std::string(effect) + "]}}"; };
        require(attempt(with(R"({"id":"minecraft:enchantment.respiration","attribute":"minecraft:oxygen_bonus","amount":{"type":"minecraft:linear","base":1,"per_level_above_first":1},"operation":"add_value"})")) == "loaded",
                "a definition like the official one loads");
        require(attempt(with(R"({"id":"minecraft:enchantment.respiration","attribute":"minecraft:scale","amount":1,"operation":"add_value"})")).find("does not model") != std::string::npos,
                "an attribute Circuit does not model");
        require(attempt(with(R"({"id":"pack:bonus","attribute":"minecraft:oxygen_bonus","amount":1,"operation":"add_value"})")).find("namespace") != std::string::npos,
                "a modifier outside the minecraft namespace");
        require(attempt(with(R"({"id":"minecraft:enchantment.respiration","attribute":"minecraft:oxygen_bonus","amount":{"type":"minecraft:wavy"},"operation":"add_value"})")).find("cannot read") != std::string::npos,
                "a value form Circuit does not know");
        // Damage protection and immunity: the forms the official packs use
        // load; another value effect, a requirement on more than the damage
        // type's tags or a tag no pack defines do not.
        const std::string armor = R"({"description":"r","supported_items":"#minecraft:enchantable/head_armor","weight":2,"max_level":3,"min_cost":{"base":10,"per_level_above_first":10},"max_cost":{"base":40,"per_level_above_first":10},"anvil_cost":4,"slots":["armor"],"effects":{)";
        const auto protecting = [&](std::string_view effect, std::string_view requirement) {
            return armor + R"("minecraft:damage_protection":[{"effect":)" + std::string(effect) + R"(,"requirements":)" + std::string(requirement) + "}]}}";
        };
        const std::string add = R"({"type":"minecraft:add","value":{"type":"minecraft:linear","base":2,"per_level_above_first":2}})";
        const std::string fire = R"({"condition":"minecraft:all_of","terms":[{"condition":"minecraft:damage_source_properties","predicate":{"tags":[{"id":"minecraft:is_fire","expected":true},{"id":"minecraft:bypasses_invulnerability","expected":false}]}}]})";
        require(attempt(protecting(add, fire)) == "loaded", "fire protection's form loads");
        require(attempt(armor + R"("minecraft:damage_immunity":[{"effect":{},"requirements":{"condition":"minecraft:inverted","term":{"condition":"minecraft:damage_source_properties","predicate":{"tags":[{"id":"minecraft:is_fall","expected":true}]}}}}]}})") == "loaded",
                "an immunity, inverted, loads");
        require(attempt(protecting(R"({"type":"minecraft:multiply","factor":2})", fire)).find("damage protection effect Circuit cannot read") != std::string::npos,
                "another value effect");
        require(attempt(protecting(add, R"({"condition":"minecraft:damage_source_properties","predicate":{"is_direct":true}})")).find("damage requirement Circuit cannot read") != std::string::npos,
                "a requirement on more than tags");
        require(attempt(protecting(add, R"({"condition":"minecraft:random_chance","chance":0.5})")).find("damage requirement Circuit cannot read") != std::string::npos,
                "another condition");
        require(attempt(protecting(add, R"({"condition":"minecraft:damage_source_properties","predicate":{"tags":[{"id":"pack:nothing","expected":true}]}})")).find("damage requirement Circuit cannot read") != std::string::npos,
                "a tag no pack defines");
        require(attempt(protecting(add, R"({"condition":"minecraft:all_of","terms":[{"condition":"minecraft:random_chance","chance":0.5}]})")).find("damage requirement Circuit cannot read") != std::string::npos
                    && attempt(protecting(add, R"({"condition":"minecraft:inverted","term":{"condition":"minecraft:random_chance","chance":0.5}})")).find("damage requirement Circuit cannot read") != std::string::npos,
                "an unreadable term within all of or inverted");
        require(attempt(protecting(R"({"type":"minecraft:add","value":{"type":"minecraft:wavy"}})", fire)).find("damage protection effect Circuit cannot read") != std::string::npos,
                "an added value of a form Circuit does not know");
        require(attempt(armor + R"("minecraft:damage_immunity":[{"effect":{},"requirements":{"condition":"minecraft:random_chance","chance":0.5}}]}})").find("damage requirement Circuit cannot read") != std::string::npos,
                "an immunity with an unreadable requirement");
        // Attacks: the damage, knockback and post-attack forms the official
        // packs use load (smite's entity type tag, power's direct attacker,
        // fire aspect's direct ignite, thorns' chance, damage and wear, bane
        // of arthropods' slowness, not acted on); others do not, nor
        // is_direct or a random chance in protection.
        const std::string weapon = R"({"description":"r","supported_items":"#minecraft:enchantable/sword","weight":2,"max_level":3,"min_cost":{"base":10,"per_level_above_first":10},"max_cost":{"base":40,"per_level_above_first":10},"anvil_cost":4,"slots":["mainhand"],"effects":{)";
        const auto effects = [&](std::string_view body) { return weapon + std::string(body) + "}}"; };
        const std::string smite = R"({"condition":"minecraft:entity_properties","entity":"this","predicate":{"type":"#minecraft:sensitive_to_smite"}})";
        const std::string direct = R"({"condition":"minecraft:damage_source_properties","predicate":{"is_direct":true}})";
        require(attempt(effects(R"("minecraft:damage":[{"effect":)" + add + R"(,"requirements":)" + smite + "}]")) == "loaded"
                    && attempt(effects(R"("minecraft:knockback":[{"effect":)" + add + R"(,"requirements":{"condition":"minecraft:entity_properties","entity":"direct_attacker","predicate":{"type":["minecraft:arrow","spectral_arrow"]}}}])")) == "loaded"
                    && attempt(effects(R"("minecraft:post_attack":[{"affected":"victim","enchanted":"attacker","effect":{"type":"minecraft:ignite","duration":{"type":"minecraft:linear","base":4,"per_level_above_first":4}},"requirements":)" + direct + "}]")) == "loaded"
                    && attempt(effects(R"("minecraft:post_attack":[{"affected":"victim","enchanted":"attacker","effect":{"type":"minecraft:apply_mob_effect","to_apply":"minecraft:slowness","min_duration":1.5,"max_duration":1.5,"min_amplifier":3,"max_amplifier":3},"requirements":{"condition":"minecraft:all_of","terms":[)" + smite + "," + direct + "]}}]")) == "loaded"
                    && attempt(effects(R"("minecraft:post_attack":[{"affected":"attacker","enchanted":"victim","effect":{"type":"minecraft:all_of","effects":[{"type":"minecraft:damage_entity","damage_type":"minecraft:thorns","max_damage":5.0,"min_damage":1.0},{"type":"minecraft:change_item_damage","amount":2.0}]},"requirements":{"chance":{"type":"minecraft:enchantment_level","amount":{"type":"minecraft:linear","base":0.15,"per_level_above_first":0.15}},"condition":"minecraft:random_chance"}}])")) == "loaded",
                "the official forms of damage, knockback and post-attack effects load");
        // Forms the official packs do not use: an ignite onto the attacker,
        // a constant chance, damage_entity of another type (not acted on,
        // also among others: its requirement not read), the damaging entity
        // enchanted (a melee attack's attacker).
        require(attempt(effects(R"("minecraft:post_attack":[{"affected":"attacker","enchanted":"victim","effect":{"type":"minecraft:ignite","duration":4},"requirements":{"condition":"minecraft:random_chance","chance":{"type":"minecraft:constant","value":0.5}}}])")) == "loaded"
                    && attempt(effects(R"("minecraft:post_attack":[{"affected":"victim","enchanted":"damaging_entity","effect":{"type":"minecraft:change_item_damage","amount":1},"requirements":{"condition":"minecraft:random_chance","chance":0.25}}])")) == "loaded"
                    && attempt(effects(R"("minecraft:post_attack":[{"affected":"attacker","enchanted":"victim","effect":{"type":"minecraft:all_of","effects":[{"type":"minecraft:damage_entity","damage_type":"minecraft:magic","max_damage":5.0,"min_damage":1.0},{"type":"minecraft:change_item_damage","amount":2.0}]},"requirements":{"condition":"minecraft:weather_check","thundering":true}}])")) == "loaded",
                "other forms of post-attack effects load");
        require(attempt(effects(R"("minecraft:damage":[{"effect":{"type":"minecraft:multiply","factor":2}}])")).find("a damage effect Circuit cannot read") != std::string::npos
                    && attempt(effects(R"("minecraft:knockback":[{"effect":{"type":"minecraft:set","value":2}}])")).find("a knockback effect Circuit cannot read") != std::string::npos,
                "a damage or knockback effect other than an added value");
        const auto post_attack = [&](std::string_view effect, std::string_view requirement = "") {
            return effects(R"("minecraft:post_attack":[{"affected":"attacker","enchanted":"victim","effect":)" + std::string(effect)
                           + (requirement.empty() ? std::string{} : R"(,"requirements":)" + std::string(requirement)) + "}]");
        };
        const auto unreadable = [&](const std::string& definition) { return attempt(definition).find("post-attack effect Circuit cannot read") != std::string::npos; };
        require(unreadable(effects(R"("minecraft:post_attack":[{"affected":"killer","enchanted":"victim","effect":{"type":"minecraft:ignite","duration":4}}])"))
                    && unreadable(effects(R"("minecraft:post_attack":[{"affected":"victim","effect":{"type":"minecraft:ignite","duration":4}}])"))
                    && unreadable(post_attack(R"({"type":"minecraft:ignite","duration":{"type":"minecraft:wavy"}})"))
                    && unreadable(post_attack(R"({"duration":4})"))
                    && unreadable(post_attack(R"({"type":"minecraft:damage_entity","damage_type":"minecraft:thorns","min_damage":1.0})"))
                    && unreadable(post_attack(R"({"type":"minecraft:damage_entity","max_damage":5.0,"min_damage":1.0})"))
                    && unreadable(post_attack(R"({"type":"minecraft:change_item_damage","amount":{"type":"minecraft:wavy"}})"))
                    && unreadable(post_attack(R"({"type":"minecraft:all_of","effects":[{"type":"minecraft:change_item_damage"}]})"))
                    && unreadable(post_attack(R"({"type":"minecraft:all_of","effects":{"type":"minecraft:ignite","duration":4}})"))
                    && unreadable(post_attack(R"({"type":"minecraft:all_of","effects":[{"type":"minecraft:apply_mob_effect"},{"type":"minecraft:ignite"}]})")),
                "a post-attack effect of an unknown party, without one, an unknown duration, damage or wear form, without a type, "
                "or all of an unreadable effect or not a list (also among effects not acted on)");
        const auto bad_requirement = [&](const std::string& definition) {
            return attempt(definition).find("damage requirement Circuit cannot read") != std::string::npos;
        };
        require(bad_requirement(post_attack(R"({"type":"minecraft:ignite","duration":4})", R"({"condition":"minecraft:random_chance","chance":{"type":"minecraft:uniform","min":0,"max":1}})"))
                    && bad_requirement(post_attack(R"({"type":"minecraft:ignite","duration":4})", R"({"condition":"minecraft:random_chance","chance":{"type":"minecraft:constant","value":"half"}})"))
                    && bad_requirement(post_attack(R"({"type":"minecraft:ignite","duration":4})", R"({"condition":"minecraft:random_chance"})"))
                    && bad_requirement(post_attack(R"({"type":"minecraft:ignite","duration":4})", R"({"condition":"minecraft:random_chance","chance":{"type":"minecraft:enchantment_level","amount":{"type":"minecraft:wavy"}}})"))
                    && bad_requirement(effects(R"("minecraft:damage":[{"effect":)" + add + R"(,"requirements":{"condition":"minecraft:random_chance","chance":0.5}}])")),
                "a random chance of a uniform, unreadable or missing chance, or in an attack's damage");
        const auto damaging = [&](std::string_view requirement) {
            return effects(R"("minecraft:damage":[{"effect":)" + add + R"(,"requirements":)" + std::string(requirement) + "}]");
        };
        for (const auto* requirement : {R"({"condition":"minecraft:entity_properties","entity":"killer","predicate":{"type":"minecraft:zombie"}})",
                                        R"({"condition":"minecraft:entity_properties","entity":"this","predicate":{"type":"minecraft:zombie","flags":{"is_on_fire":true}}})",
                                        R"({"condition":"minecraft:entity_properties","entity":"this","predicate":{"type":"#pack:nothing"}})",
                                        R"({"condition":"minecraft:entity_properties","entity":"this","predicate":{"type":["#minecraft:undead"]}})",
                                        R"({"condition":"minecraft:damage_source_properties","predicate":{"is_direct":"yes"}})",
                                        R"({"condition":"minecraft:damage_source_properties","predicate":{"direct_entity":{"type":"minecraft:arrow"}}})"})
            require(attempt(damaging(requirement)).find("damage requirement Circuit cannot read") != std::string::npos, std::format("an unreadable requirement: {}", requirement));
        require(attempt(protecting(add, direct)).find("damage requirement Circuit cannot read") != std::string::npos, "is_direct in protection");
        // Item damage: unbreaking's form loads (remove_binomial by a
        // fraction, for the items of a tag or the others), and items named
        // alone or in a list; another value effect, a predicate on more than
        // the items, a tag no pack defines, a tag in a list, another
        // condition or an unreadable chance do not.
        const auto item_damage = [&](std::string_view effect, std::string_view requirement = "") {
            return effects(R"("minecraft:item_damage":[{"effect":)" + std::string(effect)
                           + (requirement.empty() ? std::string{} : R"(,"requirements":)" + std::string(requirement)) + "}]");
        };
        const std::string binomial = R"({"type":"minecraft:remove_binomial","chance":{"type":"minecraft:fraction","numerator":{"type":"minecraft:linear","base":1.0,"per_level_above_first":1.0},"denominator":{"type":"minecraft:linear","base":2.0,"per_level_above_first":1.0}}})";
        const std::string armor_items = R"({"condition":"minecraft:match_tool","predicate":{"items":"#minecraft:enchantable/armor"}})";
        require(attempt(item_damage(binomial, armor_items)) == "loaded"
                    && attempt(item_damage(binomial, R"({"condition":"minecraft:inverted","term":)" + armor_items + "}")) == "loaded"
                    && attempt(item_damage(binomial)) == "loaded"
                    && attempt(item_damage(binomial, R"({"condition":"minecraft:any_of","terms":[{"condition":"minecraft:match_tool","predicate":{"items":"minecraft:shears"}},{"condition":"minecraft:all_of","terms":[{"condition":"minecraft:match_tool","predicate":{"items":["iron_sword","minecraft:iron_axe"]}}]}]})")) == "loaded",
                "unbreaking's item damage loads, and item requirements of items alone, a list, any of and all of");
        const auto bad_item_damage = [&](const std::string& definition) {
            return attempt(definition).find("item damage effect Circuit cannot read") != std::string::npos;
        };
        const auto bad_item_requirement = [&](const std::string& definition) {
            return attempt(definition).find("item requirement Circuit cannot read") != std::string::npos;
        };
        require(bad_item_damage(item_damage(R"({"type":"minecraft:add","value":1})"))
                    && bad_item_damage(item_damage(R"({"type":"minecraft:remove_binomial"})"))
                    && bad_item_damage(item_damage(R"({"type":"minecraft:remove_binomial","chance":{"type":"minecraft:wavy"}})"))
                    && attempt(effects(R"("minecraft:item_damage":{"effect":{"type":"minecraft:remove_binomial","chance":0.5}})")).find("item damage effects are not a list") != std::string::npos,
                "an item damage effect of another form, without its chance or of an unknown one, or not a list");
        require(bad_item_requirement(item_damage(binomial, R"({"condition":"minecraft:match_tool","predicate":{"items":"#pack:nothing"}})"))
                    && bad_item_requirement(item_damage(binomial, R"({"condition":"minecraft:match_tool","predicate":{"items":["#minecraft:enchantable/armor"]}})"))
                    && bad_item_requirement(item_damage(binomial, R"({"condition":"minecraft:match_tool","predicate":{"items":"minecraft:shears","count":1}})"))
                    && bad_item_requirement(item_damage(binomial, R"({"condition":"minecraft:match_tool","predicate":{}})"))
                    && bad_item_requirement(item_damage(binomial, R"({"condition":"minecraft:random_chance","chance":0.5})"))
                    && bad_item_requirement(item_damage(binomial, R"({"condition":"minecraft:inverted","term":{"condition":"minecraft:match_tool"}})"))
                    && bad_item_requirement(item_damage(binomial, R"({"condition":"minecraft:all_of","terms":{"condition":"minecraft:match_tool","predicate":{"items":"minecraft:shears"}}})")),
                "an item requirement on a tag no pack defines, a tag in a list, more than the items, nothing, another condition, an unreadable term, terms not a list");
        // Repair with experience (client/ench_mending, client/ench_mending_pack):
        // mending's form and every value effect form load, with or without an
        // item requirement; another type, a missing or unreadable value, a
        // part unreadable, parts or effects not a list, or an unreadable
        // requirement do not.
        const auto repair = [&](std::string_view effect, std::string_view requirement = "") {
            return effects(R"("minecraft:repair_with_xp":[{"effect":)" + std::string(effect)
                           + (requirement.empty() ? std::string{} : R"(,"requirements":)" + std::string(requirement)) + "}]");
        };
        const auto bad_repair = [&](const std::string& definition) {
            return attempt(definition).find("repair effect Circuit cannot read") != std::string::npos;
        };
        require(attempt(repair(R"({"type":"minecraft:multiply","factor":2.0})")) == "loaded"
                    && attempt(repair(R"({"type":"minecraft:multiply","factor":{"type":"minecraft:lookup","values":[0.5,1.5,3.0],"fallback":2.0}})",
                                      R"({"condition":"minecraft:match_tool","predicate":{"items":"minecraft:iron_pickaxe"}})")) == "loaded"
                    && attempt(repair(R"({"type":"add","value":1.0})")) == "loaded"
                    && attempt(repair(R"({"type":"minecraft:set","value":{"type":"minecraft:linear","base":3.0,"per_level_above_first":-1.0}})")) == "loaded"
                    && attempt(repair(R"({"type":"minecraft:remove_binomial","chance":0.5})")) == "loaded"
                    && attempt(repair(R"({"type":"minecraft:all_of","effects":[{"type":"minecraft:multiply","factor":3.0},{"type":"minecraft:add","value":-1.0}]})")) == "loaded",
                "repair effects: multiply (a constant or by level, with an item requirement), add, set, remove_binomial and all of");
        require(bad_repair(repair(R"({"type":"minecraft:exponent","base":2.0})")) && bad_repair(repair(R"({"type":"minecraft:multiply"})"))
                    && bad_repair(repair(R"({"type":"minecraft:add","value":{"type":"minecraft:wavy"}})")) && bad_repair(repair(R"({"factor":2.0})"))
                    && bad_repair(repair(R"({"type":"minecraft:all_of","effects":[{"type":"minecraft:add"}]})"))
                    && bad_repair(repair(R"({"type":"minecraft:all_of","effects":{"type":"minecraft:add","value":1.0}})"))
                    && bad_repair(effects(R"("minecraft:repair_with_xp":[{"requirements":{"condition":"minecraft:match_tool","predicate":{"items":"minecraft:shears"}}}])"))
                    && attempt(effects(R"("minecraft:repair_with_xp":{"effect":{"type":"minecraft:multiply","factor":2.0}})")).find("repair effects are not a list") != std::string::npos
                    && bad_item_requirement(repair(R"({"type":"minecraft:multiply","factor":2.0})", R"({"condition":"minecraft:random_chance","chance":0.5})")),
                "a repair effect of another type, without its value or of an unknown one, a part unreadable, parts not a list, no effect, "
                "not a list, or with an unreadable requirement");
        // The curses' valueless effects (client/ench_curses_pack): an object
        // loads, with members too (the official servers read {"note": 1});
        // another kind does not.
        for (const auto* component : {"minecraft:prevent_armor_change", "minecraft:prevent_equipment_drop"}) {
            const auto curse = [&](std::string_view value) { return effects(std::format(R"("{}":{})", component, value)); };
            require(attempt(curse("{}")) == "loaded" && attempt(curse(R"({"note":1})")) == "loaded",
                    std::format("{}: an object loads", component));
            for (const auto* value : {"true", "[]", "1", R"("yes")"})
                require(attempt(curse(value)).find(std::format("a {} effect Circuit cannot read", component)) != std::string::npos,
                        std::format("{}: {} refused", component, value));
        }
    });

    suite.run("enchantments protect players and shorten their burning as both official servers do (client/ench_protect, client/ench_fire)", [] {
        // circuit-clienttest/ench-protect.js and ench-fire.js; enchantments
        // need the official data packs (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        using I = server::Intent;
        bool ran = false;
        for (const auto& [version, variable] : {std::pair{"1.21.8", "CIRCUIT_VANILLA_JAR_1_21_8"}, std::pair{"1.21.4", "CIRCUIT_VANILLA_JAR_1_21_4"}}) {
            const char* jar = std::getenv(variable);
            if (!jar || !*jar) continue;
            ran = true;
            const auto& bundle = **contracts::BundleRegistry::find(std::format("java-{}", version));
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value() && d->game, d ? "no game data" : d.error());
            const auto enchanted = d->game->enchanted;
            GameFixture f(version, offline_config(), std::move(*d));
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const auto item = [&](std::string_view name) { return tables.item_id(name).value(); };
            // A stack set in creative mode, enchanted.
            const auto put = [&](std::uint64_t who, std::string_view name, std::vector<std::pair<std::string, std::int32_t>> levels, std::int16_t slot) {
                auto patch = levels.empty() ? std::optional<contracts::ComponentPatch>(contracts::ComponentPatch{}) : enchanted(levels);
                require(patch.has_value(), "enchanted");
                f.game->submit(who, I{name.empty() ? server::intent::CreativeSlot{slot, 0, 0} : server::intent::CreativeSlot{slot, 1, item(name), 0, *patch}});
            };
            const bool v772 = std::string_view(version) == "1.21.8";
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::int32_t entity = -1;
            {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(alice->mutex);
                for (const auto& bytes : alice->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) entity = pk->fields.i32().value();
            }
            // alice's since the last call: her health, her damage events (the
            // type's name), her shared flags, her own window's slot changes and
            // the sounds she hears (by id).
            std::size_t seen = 0;
            const auto packets = [&] {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                std::vector<std::string> out;
                std::lock_guard lock(alice->mutex);
                for (; seen < alice->packets.size(); ++seen) {
                    auto pk = protocol::open_packet(alice->packets[seen]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        out.push_back(std::format("health {}", in.f32().value()));
                    } else if (pk->id == id("damage_event")) {
                        if (in.varint().value() != entity) continue;
                        const auto type = in.varint().value();
                        const auto& types = f.data.registries;
                        std::string name = "?";
                        for (const auto& r : types)
                            if (r.registry == "minecraft:damage_type" && type >= 0 && static_cast<std::size_t>(type) < r.entries.size())
                                name = std::string(r.entries[static_cast<std::size_t>(type)]);
                        out.push_back("damage " + name);
                    } else if (pk->id == id("set_entity_data")) {
                        if (in.varint().value() == entity && in.u8().value() == 0 && in.varint().value() == 0) out.push_back(std::format("flags {}", in.u8().value()));
                    } else if (pk->id == id("container_set_slot")) {
                        if (in.varint().value() != 0) continue;
                        (void)in.varint();
                        out.push_back(std::format("slot {}", in.i16().value()));
                    } else if (pk->id == id("sound")) {
                        out.push_back(std::format("sound {}", in.varint().value() - 1));
                    }
                }
                return out;
            };
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
                return packets();
            };
            const auto has = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::find(v, x) != v.end(); };
            const auto count = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::count(v, x); };
            const auto health = [](const std::vector<std::string>& v) {
                for (auto it = v.rbegin(); it != v.rend(); ++it)
                    if (it->starts_with("health ")) return std::stof(it->substr(7));
                return -1.0f;
            };
            // The health told right after the first damage of a type.
            const auto health_after = [](const std::vector<std::string>& v, std::string_view damage) {
                const auto it = std::ranges::find(v, damage);
                const auto h = std::find_if(it, v.end(), [](const auto& x) { return x.starts_with("health "); });
                return h == v.end() ? -1.0f : std::stof(h->substr(7));
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            // alice teleported by a command, the teleport confirmed.
            std::int32_t teleports = 1;
            const auto teleport = [&](std::string where) {
                command(1, "tp alice " + where);
                f.game->tick();
                f.game->submit(1, I{server::intent::ConfirmTeleport{++teleports}});
            };
            const auto heal = [&] {
                command(1, "effect give alice minecraft:instant_health 1 3");
                (void)ticks(30);
            };
            // Armor (helmet to boots) put on in creative mode, then survival.
            const auto wear = [&](std::vector<std::pair<std::string_view, std::vector<std::pair<std::string, std::int32_t>>>> pieces) {
                command(1, "gamemode creative");
                f.game->tick();
                for (std::size_t k = 0; k < pieces.size(); ++k) put(1, pieces[k].first, pieces[k].second, static_cast<std::int16_t>(5 + k));
                f.game->tick();
                command(1, "gamemode survival");
                (void)ticks(2);
            };
            const std::vector<std::pair<std::string, std::int32_t>> none, p4{{"minecraft:protection", 4}}, fp4{{"minecraft:fire_protection", 4}};
            f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
            f.game->submit(2, I{server::intent::ConfirmTeleport{1}});
            for (const auto* c : {"gamerule naturalRegeneration false", "gamerule doFireTick false", "difficulty normal", "gamemode survival"})
                command(1, c);
            command(2, "tp bob 2.5 -60 0.5");
            (void)ticks(2);
            f.game->submit(2, I{server::intent::ConfirmTeleport{2}});
            teleport("0.5 -60 0.5");
            f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, -60.0, 0.5}, std::nullopt, true}});
            (void)ticks(2);

            // A 10-block fall (7): feather falling IV boots 12 points; with
            // protection IV on all four 28, counted as 20; a protection IV
            // helmet held protects nothing.
            const auto fall = [&] {
                f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, -50.0, 0.5}, std::nullopt, false}});
                for (int y = 9; y >= 1; --y) f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, -60.0 + y, 0.5}, std::nullopt, false}});
                f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, -60.0, 0.5}, std::nullopt, false}});
                f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, -60.0, 0.5}, std::nullopt, true}});
                return ticks(25);
            };
            wear({{"", none}, {"", none}, {"", none}, {"minecraft:leather_boots", {{"minecraft:feather_falling", 4}}}});
            auto a = fall();
            require(has(a, "damage minecraft:fall") && health(a) == 16.360000610351562f, std::format("{}: feather falling IV: {}", version, health(a)));
            heal();
            wear({{"minecraft:leather_helmet", p4}, {"minecraft:leather_chestplate", p4}, {"minecraft:leather_leggings", p4},
                  {"minecraft:leather_boots", {{"minecraft:protection", 4}, {"minecraft:feather_falling", 4}}}});
            a = fall();
            require(health(a) == 18.600000381469727f, std::format("{}: 28 points count as 20: {}", version, health(a)));
            heal();
            wear({{"", none}, {"", none}, {"", none}, {"", none}});
            command(1, "gamemode creative");
            f.game->tick();
            put(1, "minecraft:leather_helmet", p4, 36);
            command(1, "gamemode survival");
            (void)ticks(2);
            a = fall();
            require(health(a) == 13.0f, std::format("{}: a protection IV helmet held: {}", version, health(a)));
            heal();
            // Instant damage (magic, 6) with protection IV on the helmet: 4
            // points; with resistance I, resistance first.
            wear({{"minecraft:leather_helmet", p4}});
            command(1, "effect give alice minecraft:instant_damage 1 0");
            a = ticks(25);
            require(health(a) == 14.960000038146973f, std::format("{}: magic, 4 points: {}", version, health(a)));
            heal();
            command(1, "effect give alice minecraft:resistance 600 0");
            command(1, "effect give alice minecraft:instant_damage 1 0");
            a = ticks(25);
            require(health(a) == 15.967999458312988f, std::format("{}: magic, 4 points and resistance I: {}", version, health(a)));
            command(1, "effect clear alice minecraft:resistance");
            heal();
            // Typed protection leaves other damage alone: projectile and blast
            // protection IV against a fall, fire protection IV against magic.
            const std::vector<std::pair<std::string, std::int32_t>> pp4{{"minecraft:projectile_protection", 4}}, bp4{{"minecraft:blast_protection", 4}};
            wear({{"minecraft:leather_helmet", pp4}, {"minecraft:leather_chestplate", pp4}, {"minecraft:leather_leggings", bp4}, {"minecraft:leather_boots", bp4}});
            a = fall();
            require(health(a) == 13.0f, std::format("{}: projectile and blast protection against a fall: {}", version, health(a)));
            heal();
            wear({{"minecraft:leather_helmet", fp4}, {"minecraft:leather_chestplate", fp4}, {"minecraft:leather_leggings", fp4}, {"minecraft:leather_boots", fp4}});
            command(1, "effect give alice minecraft:instant_damage 1 0");
            a = ticks(25);
            require(health(a) == 14.0f, std::format("{}: fire protection against magic: {}", version, health(a)));
            heal();
            // The void and starvation, with protection IV on all four: neither
            // reduced.
            wear({{"minecraft:leather_helmet", p4}, {"minecraft:leather_chestplate", p4}, {"minecraft:leather_leggings", p4}, {"minecraft:leather_boots", p4}});
            teleport("0.5 -200 0.5");
            a.clear();
            for (int k = 0; k < 40 && !has(a, "damage minecraft:out_of_world"); ++k) {
                const auto more = ticks(1);
                a.insert(a.end(), more.begin(), more.end());
            }
            teleport("0.5 -60 0.5");
            f.game->submit(1, I{server::intent::Move{server::Vec3{0.5, -60.0, 0.5}, std::nullopt, true}});
            const auto back = ticks(25);
            a.insert(a.end(), back.begin(), back.end());
            require(has(a, "damage minecraft:out_of_world") && health(a) == 16.0f, std::format("{}: the void: {}", version, health(a)));
            heal();
            command(1, "effect give alice minecraft:hunger 60 255");
            a.clear();
            for (int k = 0; k < 400 && !has(a, "damage minecraft:starve"); ++k) {
                const auto more = ticks(1);
                a.insert(a.end(), more.begin(), more.end());
            }
            require(has(a, "damage minecraft:starve") && health(a) == 19.0f, std::format("{}: starvation: {}", version, health(a)));
            command(1, "effect clear alice minecraft:hunger");
            command(1, "effect give alice minecraft:saturation 1 19");
            heal();
            // bob's iron sword at full charge: leather armor leaves 5.04,
            // protection IV on all four 1.8144.
            command(2, "gamemode creative");
            f.game->tick();
            put(2, "minecraft:iron_sword", none, 36);
            command(2, "gamemode survival");
            (void)ticks(30);
            f.game->submit(2, I{server::intent::Attack{entity}});
            a = ticks(25);
            require(has(a, "damage minecraft:player_attack") && health(a) == 18.18560028076172f, std::format("{}: the sword: {}", version, health(a)));
            heal();

            // Fire (ench-fire.js): alice in a pit with a netherrack floor.
            for (const auto* c : {"setblock 8 -62 8 netherrack", "setblock 8 -61 8 air"}) command(1, c);
            teleport("8.5 -61 8.5");
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -61.0, 8.5}, std::nullopt, true}});
            (void)ticks(2);
            // Frost walker II boots on a magma block: no hurt.
            wear({{"minecraft:diamond_helmet", none}, {"minecraft:diamond_chestplate", none}, {"minecraft:diamond_leggings", none},
                  {"minecraft:diamond_boots", {{"minecraft:frost_walker", 2}}}});
            command(1, "setblock 8 -62 8 magma_block");
            a = ticks(60);
            require(!std::ranges::any_of(a, [](const auto& x) { return x.starts_with("damage") || x.starts_with("slot") || x.starts_with("health"); }),
                    std::format("{}: frost walker on magma, neither a hurt nor wear", version));
            command(1, "setblock 8 -62 8 netherrack");
            // Its immunity is to hot floors only: lava hurts and wears.
            command(1, "setblock 8 -61 8 lava");
            a = ticks(3);
            command(1, "setblock 8 -61 8 water[level=0]");
            const auto out = ticks(5);
            a.insert(a.end(), out.begin(), out.end());
            command(1, "setblock 8 -61 8 air");
            require(has(a, "damage minecraft:lava") && has(a, "slot 8"), std::format("{}: frost walker in lava", version));
            heal();
            // Fire protection IV on all four: 20 points against the magma;
            // lava hurts 0.192 and never sets her burning.
            wear({{"minecraft:diamond_helmet", fp4}, {"minecraft:diamond_chestplate", fp4}, {"minecraft:diamond_leggings", fp4}, {"minecraft:diamond_boots", fp4}});
            command(1, "setblock 8 -62 8 magma_block");
            a = ticks(30);
            require(health_after(a, "damage minecraft:hot_floor") == 19.95800018310547f,
                    std::format("{}: magma, fire protection IV: {}", version, health_after(a, "damage minecraft:hot_floor")));
            command(1, "setblock 8 -62 8 netherrack");
            heal();
            // Water then: not burning, no extinguish sound.
            const auto extinguish = std::format("sound {}", v772 ? 616 : 603);
            command(1, "setblock 8 -61 8 lava");
            a = ticks(3);
            command(1, "setblock 8 -61 8 water[level=0]");
            const auto rest = ticks(10);
            a.insert(a.end(), rest.begin(), rest.end());
            command(1, "setblock 8 -61 8 air");
            const auto dry = ticks(30);
            a.insert(a.end(), dry.begin(), dry.end());
            require(has(a, "damage minecraft:lava") && health(a) == 19.808000564575195f && !has(a, "flags 1") && !has(a, extinguish),
                    std::format("{}: lava, fire protection IV: {}", version, health(a)));
            heal();
            // Fire protection I on a helmet alone: lava's burning hurts 12
            // times after it (14 without), each 0.92.
            wear({{"minecraft:iron_helmet", {{"minecraft:fire_protection", 1}}}, {"", none}, {"", none}, {"", none}});
            command(1, "setblock 8 -61 8 lava");
            a = ticks(3);
            command(1, "setblock 8 -61 8 air");
            const auto burning = ticks(300);
            require(has(a, "damage minecraft:lava") && count(burning, "damage minecraft:on_fire") == 12 && has(burning, "flags 0"),
                    std::format("{}: fire protection I: {} burning hurts", version, count(burning, "damage minecraft:on_fire")));
        }
        if (!ran) std::puts("  (no official server jar given: the protection checks did not run)");
    });

    suite.run("weapon enchantments add damage, knockback, fire and sweeps to players' attacks as both official servers do (client/ench_combat)", [] {
        // circuit-clienttest/ench-combat.js: alice (8.5 -60 8.5, facing east)
        // attacks bob (10.5 -60 8.5, facing west) with carol beside him
        // (10.5 -60 9.5), all three in survival on a floor of dirt;
        // enchantments need the official data packs (CIRCUIT_VANILLA_JAR_1_21_8,
        // _1_21_4).
        using I = server::Intent;
        bool ran = false;
        for (const auto& [version, variable] : {std::pair{"1.21.8", "CIRCUIT_VANILLA_JAR_1_21_8"}, std::pair{"1.21.4", "CIRCUIT_VANILLA_JAR_1_21_4"}}) {
            const char* jar = std::getenv(variable);
            if (!jar || !*jar) continue;
            ran = true;
            const auto& bundle = **contracts::BundleRegistry::find(std::format("java-{}", version));
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value() && d->game, d ? "no game data" : d.error());
            const auto enchanted = d->game->enchanted;
            GameFixture f(version, offline_config(), std::move(*d));
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const bool v772 = std::string_view(version) == "1.21.8";
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            auto carol = f.join(3, "carol");
            std::map<std::int32_t, std::string> names;
            std::map<std::string, std::int32_t> ids;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}, std::pair{carol, "carol"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) {
                        const auto e = pk->fields.i32().value();
                        names[e] = name;
                        ids[name] = e;
                    }
            }
            // Since the last call, as text: health, damage events (the type,
            // the causing and direct entity), velocities, sounds, animations,
            // particles (the id and count) and the shared flags.
            std::map<std::uint64_t, std::size_t> seen;
            std::int32_t last_teleport = 0; // alice's
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        out.push_back(std::format("health {}", in.f32().value()));
                    } else if (pk->id == id("player_position")) {
                        if (&r == alice.get()) last_teleport = in.varint().value();
                    } else if (pk->id == id("damage_event")) {
                        const auto e = in.varint().value(), type = in.varint().value(), cause = in.varint().value(), direct = in.varint().value();
                        out.push_back(std::format("damage {} {} by {} {}", who(e), type, cause ? who(cause - 1) : "none", direct ? who(direct - 1) : "none"));
                    } else if (pk->id == id("set_entity_motion")) {
                        const auto e = in.varint().value();
                        const auto x = in.i16().value(), y = in.i16().value(), z = in.i16().value();
                        out.push_back(std::format("velocity {} {} {} {}", who(e), x, y, z));
                    } else if (pk->id == id("sound")) {
                        out.push_back(std::format("sound {}", in.varint().value() - 1));
                    } else if (pk->id == id("animate")) {
                        const auto e = in.varint().value();
                        out.push_back(std::format("animation {} {}", who(e), in.u8().value()));
                    } else if (pk->id == id("level_particles")) {
                        (void)in.boolean();
                        (void)in.boolean();
                        for (int k = 0; k < 3; ++k) (void)in.f64();
                        for (int k = 0; k < 4; ++k) (void)in.f32();
                        const auto n = in.i32().value();
                        out.push_back(std::format("particle {} count {}", in.varint().value(), n));
                    } else if (pk->id == id("set_entity_data")) {
                        const auto e = in.varint().value();
                        if (in.u8().value() == 0 && in.varint().value() == 0) out.push_back(std::format("flags {} {}", who(e), in.u8().value()));
                    }
                }
                return out;
            };
            struct Seen {
                std::vector<std::string> alice, bob, carol;
            };
            const auto ticks = [&](int n) {
                Seen all;
                for (int k = 0; k < n; ++k) {
                    f.game->tick();
                    for (auto [r, into] : {std::pair{alice.get(), &all.alice}, std::pair{bob.get(), &all.bob}, std::pair{carol.get(), &all.carol}}) {
                        auto v = packets(*r);
                        into->insert(into->end(), v.begin(), v.end());
                    }
                }
                return all;
            };
            const auto has = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::find(v, x) != v.end(); };
            const auto count = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::count(v, x); };
            const auto in_order = [](const std::vector<std::string>& v, std::initializer_list<std::string> want) {
                auto at = v.begin();
                for (const auto& w : want) {
                    at = std::find(at, v.end(), w);
                    if (at == v.end()) return false;
                    ++at;
                }
                return true;
            };
            const auto text = [](const std::vector<std::string>& v) {
                std::string t;
                for (const auto& x : v) t += x + "; ";
                return t;
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const auto attack = [&] {
                f.game->submit(1, I{server::intent::Attack{ids.at("bob")}});
                f.game->submit(1, I{server::intent::Swing{0}});
            };
            // A swing and an attack in one tick: the charge of no ticks.
            const auto weak = [&] {
                f.game->submit(1, I{server::intent::Swing{0}});
                attack();
            };
            // A stack set in creative mode, enchanted.
            const auto put = [&](std::uint64_t who, std::int16_t slot, std::string_view name, std::vector<std::pair<std::string, std::int32_t>> levels) {
                command(who, "gamemode creative");
                (void)ticks(1);
                if (name.empty()) {
                    f.game->submit(who, I{server::intent::CreativeSlot{slot, 0, 0}});
                } else {
                    auto patch = levels.empty() ? std::optional<contracts::ComponentPatch>(contracts::ComponentPatch{}) : enchanted(levels);
                    require(patch.has_value(), "enchanted");
                    f.game->submit(who, I{server::intent::CreativeSlot{slot, 1, tables.item_id(name).value(), 0, *patch}});
                }
                (void)ticks(1);
                command(who, "gamemode survival");
            };
            const auto heal = [&] {
                command(1, "effect give bob minecraft:instant_health 1 3");
                command(1, "effect give carol minecraft:instant_health 1 3");
                (void)ticks(30);
            };
            const auto sound = [&](std::int32_t v772_id) { return std::format("sound {}", v772 ? v772_id : v772_id - 40); };
            const auto hurt = sound(1175), strong = sound(1168), weak_sound = sound(1170), nodamage = sound(1167), sweep = sound(1169),
                       knockback = sound(1166), crit = sound(1165);
            const std::string hit_bob = "damage bob 34 by alice alice", hit_carol = "damage carol 34 by alice alice", burnt_bob = "damage bob 31 by none none",
                              burnt_carol = "damage carol 31 by none none", magic = "animation bob 5";
            const auto sweep_particle = std::format("particle {} count 0", v772 ? 64 : 63);
            for (const auto* c : {"gamerule naturalRegeneration false", "gamerule doFireTick false", "difficulty normal", "fill 4 -61 4 16 -61 12 dirt",
                                  "tp alice 8.5 -60 8.5 -90 0", "gamemode survival"})
                command(1, c);
            command(2, "tp bob 10.5 -60 8.5 90 0");
            command(2, "gamemode survival");
            command(3, "tp carol 10.5 -60 9.5 90 0");
            command(3, "gamemode survival");
            (void)ticks(2);
            f.game->submit(1, I{server::intent::ConfirmTeleport{last_teleport}});
            f.game->submit(1, I{server::intent::Move{std::nullopt, std::pair{-90.0f, 0.0f}, true}});
            f.game->submit(2, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            f.game->submit(3, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            (void)ticks(30);

            // Sharpness V on an iron sword: 9 to bob, the sweep's 1 and the
            // sharpness to carol, the enchanted hit's animation (5) to all
            // after the sweep's particle, before the damage indicators (4).
            put(1, 36, "minecraft:iron_sword", {{"minecraft:sharpness", 5}});
            heal();
            attack();
            auto t = ticks(2);
            require(in_order(t.alice, {hit_bob, hurt, hit_carol, hurt, sweep, sweep_particle, magic, "particle 6 count 4"})
                        && in_order(t.bob, {"velocity bob 3200 2886 0", magic, "health 11"}) && has(t.carol, magic) && has(t.carol, "health 16"),
                    std::format("{}: sharpness V: {} / {}", version, text(t.alice), text(t.carol)));
            // A swing and a hit at once: the sharpness times the charge
            // (0.04), the animation after the weak sound.
            heal();
            weak();
            t = ticks(2);
            require(in_order(t.alice, {hit_bob, hurt, weak_sound, magic}) && has(t.bob, "health 18.67232"), std::format("{}: weak: {}", version, text(t.bob)));
            // Falling: a critical hit of 6 * 1.5 + 3, the enchanted hit's
            // animation after the critical one's.
            heal();
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -59.58, 8.5}, std::nullopt, false}});
            (void)ticks(1);
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -59.7, 8.5}, std::nullopt, false}});
            attack();
            t = ticks(2);
            require(in_order(t.alice, {hit_bob, crit, "animation bob 4", magic, "particle 6 count 6"}) && has(t.bob, "health 8"),
                    std::format("{}: critical: {} / {}", version, text(t.alice), text(t.bob)));
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -60.0, 8.5}, std::nullopt, true}});
            // bob in creative mode: the nodamage sound alone.
            heal();
            command(2, "gamemode creative");
            (void)ticks(2);
            attack();
            t = ticks(2);
            require(has(t.alice, nodamage) && !has(t.alice, magic) && !has(t.carol, hit_carol), std::format("{}: creative: {}", version, text(t.alice)));
            command(2, "gamemode survival");
            // A stick of sharpness V: 4; under weakness (no attack damage) 3.
            put(1, 36, "minecraft:stick", {{"minecraft:sharpness", 5}});
            heal();
            attack();
            t = ticks(2);
            require(in_order(t.alice, {hit_bob, strong, magic}) && has(t.bob, "health 16"), std::format("{}: a stick: {}", version, text(t.bob)));
            heal();
            command(1, "effect give alice minecraft:weakness 30 0");
            (void)ticks(30);
            attack();
            t = ticks(2);
            require(in_order(t.alice, {hit_bob, strong, magic}) && has(t.bob, "health 17"), std::format("{}: weakness: {}", version, text(t.bob)));
            command(1, "effect clear alice minecraft:weakness");
            // Smite V: nothing against a player, no enchanted hit.
            put(1, 36, "minecraft:iron_sword", {{"minecraft:smite", 5}});
            heal();
            attack();
            t = ticks(2);
            require(!has(t.alice, magic) && has(t.bob, "health 14") && has(t.carol, "health 19"), std::format("{}: smite: {}", version, text(t.alice)));

            // Knockback II: 1 more along her facing, also on a weak hit; on a
            // weak one sprinting, the sprint ends (no knockback sound).
            put(1, 36, "minecraft:iron_sword", {{"minecraft:knockback", 2}});
            heal();
            attack();
            t = ticks(2);
            require(has(t.bob, "velocity bob 9600 3200 0") && !has(t.alice, knockback) && has(t.alice, sweep), std::format("{}: knockback II: {}", version, text(t.bob)));
            heal();
            weak();
            t = ticks(2);
            require(has(t.bob, "velocity bob 9600 3200 0") && has(t.bob, "health 18.79232"), std::format("{}: weak knockback: {}", version, text(t.bob)));
            heal();
            f.game->submit(1, I{server::intent::Sprint{true}});
            (void)ticks(2);
            weak();
            t = ticks(2);
            require(has(t.bob, "velocity bob 9600 3200 0") && has(t.bob, "flags alice 0") && !has(t.alice, knockback),
                    std::format("{}: weak sprinting knockback: {}", version, text(t.bob)));
            heal();
            f.game->submit(1, I{server::intent::Sprint{true}});
            (void)ticks(2);
            attack();
            t = ticks(2);
            require(in_order(t.alice, {knockback, hit_bob, strong}) && has(t.bob, "velocity bob 13600 3200 0") && !has(t.alice, hit_carol),
                    std::format("{}: sprinting knockback: {}", version, text(t.bob)));
            f.game->submit(1, I{server::intent::Sprint{false}});
            // Against a netherite chestplate (knockback resistance 0.1).
            put(2, 6, "minecraft:netherite_chestplate", {});
            heal();
            attack();
            t = ticks(2);
            require(has(t.bob, "velocity bob 8640 3200 0"), std::format("{}: knockback resistance: {}", version, text(t.bob)));
            put(2, 6, "", {});

            // Fire aspect: bob and carol (swept) set on fire, 3 burning hurts
            // each with I and 7 with II, also on a weak hit; against fire
            // protection IV (a burning time of 0.4) 2.
            put(1, 36, "minecraft:iron_sword", {{"minecraft:fire_aspect", 1}});
            heal();
            attack();
            auto after = ticks(200);
            // The flag after the next tick's health (the official servers take
            // the attack after the target's tick).
            require(in_order(after.alice, {"flags bob 1", "flags carol 1"}) && in_order(after.bob, {"health 14", "flags bob 1"})
                        && count(after.alice, burnt_bob) == 3 && count(after.alice, burnt_carol) == 3 && has(after.alice, "flags bob 0"),
                    std::format("{}: fire aspect I: {} / {}", version, count(after.alice, burnt_bob), count(after.alice, burnt_carol)));
            put(1, 36, "minecraft:iron_sword", {{"minecraft:fire_aspect", 2}});
            heal();
            attack();
            after = ticks(200);
            require(count(after.alice, burnt_bob) == 7 && count(after.alice, burnt_carol) == 7, std::format("{}: fire aspect II", version));
            heal();
            weak();
            after = ticks(200);
            require(count(after.alice, burnt_bob) == 7 && count(after.alice, burnt_carol) == 0, std::format("{}: fire aspect II, weak", version));
            put(2, 6, "minecraft:iron_chestplate", {{"minecraft:fire_protection", 4}});
            heal();
            attack();
            after = ticks(200);
            require(count(after.alice, burnt_bob) == 2 && count(after.alice, burnt_carol) == 7 && has(after.bob, "health 13.36"),
                    std::format("{}: against fire protection IV: {}", version, count(after.alice, burnt_bob)));
            put(2, 6, "", {});
            // bob in creative mode: no fire.
            heal();
            command(2, "gamemode creative");
            (void)ticks(2);
            attack();
            after = ticks(40);
            require(!has(after.alice, "flags bob 1") && has(after.alice, nodamage), std::format("{}: creative, no fire", version));
            command(2, "gamemode survival");
            // Fire aspect I on bob burning from II: the longer burning kept
            // (5 burning hurts after the second hit, its first within the
            // hurt cooldown).
            put(1, 37, "minecraft:iron_sword", {{"minecraft:fire_aspect", 1}});
            heal();
            attack();
            int waited = 0;
            while (waited < 40 && count(ticks(1).alice, burnt_bob) == 0) ++waited;
            require(waited < 40, std::format("{}: the first burning hurt", version));
            f.game->submit(1, I{server::intent::HeldSlot{1}});
            (void)ticks(15);
            attack();
            after = ticks(200);
            require(count(after.alice, hit_bob) == 1 && count(after.alice, burnt_bob) == 5, std::format("{}: fire aspect I on II: {}", version, count(after.alice, burnt_bob)));
            f.game->submit(1, I{server::intent::HeldSlot{0}});
            // A fire aspect hit (II) in the tick after bob touched lava holds
            // back no burning flag already due: the flag comes when it would
            // without the hit (the attack's fire taken after his fire tick).
            const auto flag_after_lava = [&](bool hit) {
                heal();
                (void)ticks(10);
                command(1, "setblock 10 -60 8 lava");
                (void)ticks(1);
                if (hit) attack();
                int k = 0;
                bool swept_carol = false; // the hit landed (a sweep comes only then)
                for (; k < 5; ++k) {
                    const auto t = ticks(1);
                    swept_carol = swept_carol || has(t.alice, hit_carol);
                    if (has(t.alice, "flags bob 1")) break;
                }
                require(swept_carol == hit, std::format("{}: the hit after lava {}", version, hit ? "landed" : "absent"));
                command(1, "fill 10 -60 8 10 -60 9 water[level=0]");
                (void)ticks(3);
                command(1, "fill 10 -60 8 10 -60 9 air");
                (void)ticks(5);
                return k;
            };
            const auto unhit = flag_after_lava(false), hit = flag_after_lava(true);
            require(unhit < 5 && hit == unhit, std::format("{}: the flag after lava {} ticks on, with a hit {}", version, unhit, hit));

            // Sweeping edge: carol swept for 1 and its ratio of 6 (I 4, III
            // 5.5); III with sharpness V 8.5; at a charge of 0.96 (haste 229,
            // a swing and a hit at once) the whole times the charge.
            put(1, 36, "minecraft:iron_sword", {{"minecraft:sweeping_edge", 1}});
            heal();
            attack();
            t = ticks(2);
            require(has(t.carol, "health 16") && has(t.bob, "health 14"), std::format("{}: sweeping edge I: {}", version, text(t.carol)));
            put(1, 36, "minecraft:iron_sword", {{"minecraft:sweeping_edge", 3}});
            heal();
            attack();
            t = ticks(2);
            require(has(t.carol, "health 14.5"), std::format("{}: sweeping edge III: {}", version, text(t.carol)));
            put(1, 36, "minecraft:iron_sword", {{"minecraft:sharpness", 5}, {"minecraft:sweeping_edge", 3}});
            heal();
            attack();
            t = ticks(2);
            require(has(t.carol, "health 11.5") && has(t.bob, "health 11") && has(t.carol, magic), std::format("{}: with sharpness V: {}", version, text(t.carol)));
            heal();
            command(1, "effect give alice minecraft:haste 30 229");
            (void)ticks(30);
            weak();
            t = ticks(2);
            require(has(t.alice, sweep) && has(t.bob, "health 11.496321") && has(t.carol, "health 12.110952"),
                    std::format("{}: at a charge of 0.96: {} / {}", version, text(t.bob), text(t.carol)));
            // A hit's fire is saved with a player who leaves before their fire
            // tick takes it (the hit and the leave in one tick): bob is shown
            // burning on joining again.
            command(1, "effect clear alice minecraft:haste");
            put(1, 36, "minecraft:iron_sword", {{"minecraft:fire_aspect", 2}});
            heal();
            attack();
            f.game->leave(2);
            (void)ticks(2);
            auto again = f.join(2, "bob");
            (void)ticks(3);
            std::int32_t again_id = -1;
            bool shown = false;
            {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                const auto data = f.wire.ids().id(State::play, Bound::clientbound, "set_entity_data");
                std::lock_guard lock(again->mutex);
                for (const auto& bytes : again->packets) {
                    auto pk = protocol::open_packet(bytes);
                    if (!pk) continue;
                    if (pk->id == login) {
                        again_id = pk->fields.i32().value();
                    } else if (pk->id == data && pk->fields.varint().value() == again_id && pk->fields.u8().value() == 0
                               && pk->fields.varint().value() == 0 && (pk->fields.u8().value() & 1) != 0) {
                        shown = true;
                    }
                }
            }
            require(again_id >= 0 && shown, std::format("{}: bob shown burning after leaving as he was hit", version));
        }
        if (!ran) std::puts("  (no official server jar given: the weapon enchantment checks did not run)");
    });

    suite.run("thorns hurt, knock back and wear as both official servers do (client/ench_thorns)", [] {
        // circuit-clienttest/ench-thorns.js: alice (8.5 -60 8.5, facing east)
        // attacks bob (10.5 -60 8.5, facing west), carol watching 3 blocks
        // south of alice or swept beside bob; thorns VII always acts (a
        // chance of 1.05). The enchantments need the official data packs
        // (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        using I = server::Intent;
        bool ran = false;
        for (const auto& [version, variable] : {std::pair{"1.21.8", "CIRCUIT_VANILLA_JAR_1_21_8"}, std::pair{"1.21.4", "CIRCUIT_VANILLA_JAR_1_21_4"}}) {
            const char* jar = std::getenv(variable);
            if (!jar || !*jar) continue;
            ran = true;
            const auto& bundle = **contracts::BundleRegistry::find(std::format("java-{}", version));
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value() && d->game, d ? "no game data" : d.error());
            const auto enchanted = d->game->enchanted;
            GameFixture f(version, offline_config(), std::move(*d));
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const bool v772 = std::string_view(version) == "1.21.8";
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            auto carol = f.join(3, "carol");
            std::map<std::int32_t, std::string> names;
            std::map<std::string, std::int32_t> ids;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}, std::pair{carol, "carol"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) {
                        const auto e = pk->fields.i32().value();
                        names[e] = name;
                        ids[name] = e;
                    }
            }
            // Since the last call, as text: health, damage events, velocities,
            // sounds, animations, particles, equipment others see and deaths
            // (the death screen with its message's key when it is thorns').
            std::map<std::uint64_t, std::size_t> seen;
            std::int32_t last_teleport = 0; // alice's
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        out.push_back(std::format("health {}", in.f32().value()));
                    } else if (pk->id == id("player_position")) {
                        if (&r == alice.get()) last_teleport = in.varint().value();
                    } else if (pk->id == id("damage_event")) {
                        const auto e = in.varint().value(), type = in.varint().value(), cause = in.varint().value(), direct = in.varint().value();
                        out.push_back(std::format("damage {} {} by {} {}", who(e), type, cause ? who(cause - 1) : "none", direct ? who(direct - 1) : "none"));
                    } else if (pk->id == id("set_entity_motion")) {
                        const auto e = in.varint().value();
                        const auto x = in.i16().value(), y = in.i16().value(), z = in.i16().value();
                        out.push_back(std::format("velocity {} {} {} {}", who(e), x, y, z));
                    } else if (pk->id == id("sound")) {
                        out.push_back(std::format("sound {}", in.varint().value() - 1));
                    } else if (pk->id == id("animate")) {
                        const auto e = in.varint().value();
                        out.push_back(std::format("animation {} {}", who(e), in.u8().value()));
                    } else if (pk->id == id("set_equipment")) {
                        out.push_back(std::format("equipment {}", who(in.varint().value())));
                    } else if (pk->id == id("player_combat_kill")) {
                        const std::string raw(reinterpret_cast<const char*>(r.packets[at].data()), r.packets[at].size());
                        out.push_back(raw.find("death.attack.thorns") != std::string::npos ? "death screen death.attack.thorns" : "death screen");
                    } else if (pk->id == id("level_particles")) {
                        (void)in.boolean();
                        (void)in.boolean();
                        for (int k = 0; k < 3; ++k) (void)in.f64();
                        for (int k = 0; k < 4; ++k) (void)in.f32();
                        const auto n = in.i32().value();
                        out.push_back(std::format("particle {} count {}", in.varint().value(), n));
                    } else if (pk->id == id("set_entity_data")) {
                        const auto e = in.varint().value();
                        if (in.u8().value() == 0 && in.varint().value() == 0) out.push_back(std::format("flags {} {}", who(e), in.u8().value()));
                    }
                }
                return out;
            };
            struct Seen {
                std::vector<std::string> alice, bob, carol;
            };
            const auto ticks = [&](int n) {
                Seen all;
                for (int k = 0; k < n; ++k) {
                    f.game->tick();
                    for (auto [r, into] : {std::pair{alice.get(), &all.alice}, std::pair{bob.get(), &all.bob}, std::pair{carol.get(), &all.carol}}) {
                        auto v = packets(*r);
                        into->insert(into->end(), v.begin(), v.end());
                    }
                }
                return all;
            };
            const auto has = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::find(v, x) != v.end(); };
            const auto count = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::count(v, x); };
            // In this order, a health of any value written "health".
            const auto in_order = [](const std::vector<std::string>& v, std::initializer_list<std::string> want) {
                auto at = v.begin();
                for (const auto& w : want) {
                    at = std::find_if(at, v.end(), [&](const std::string& x) { return x == w || (w == "health" && x.starts_with("health ")); });
                    if (at == v.end()) return false;
                    ++at;
                }
                return true;
            };
            const auto healths = [](const std::vector<std::string>& v) {
                std::vector<float> out;
                for (const auto& x : v)
                    if (x.starts_with("health ")) out.push_back(std::stof(x.substr(7)));
                return out;
            };
            const auto text = [](const std::vector<std::string>& v) {
                std::string t;
                for (const auto& x : v) t += x + "; ";
                return t;
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const auto attack = [&] {
                f.game->submit(1, I{server::intent::Attack{ids.at("bob")}});
                f.game->submit(1, I{server::intent::Swing{0}});
            };
            const auto put = [&](std::uint64_t who, std::int16_t slot, std::string_view name, std::vector<std::pair<std::string, std::int32_t>> levels) {
                command(who, "gamemode creative");
                (void)ticks(1);
                if (name.empty()) {
                    f.game->submit(who, I{server::intent::CreativeSlot{slot, 0, 0}});
                } else {
                    auto patch = levels.empty() ? std::optional<contracts::ComponentPatch>(contracts::ComponentPatch{}) : enchanted(levels);
                    require(patch.has_value(), "enchanted");
                    f.game->submit(who, I{server::intent::CreativeSlot{slot, 1, tables.item_id(name).value(), 0, *patch}});
                }
                (void)ticks(1);
                command(who, "gamemode survival");
            };
            const auto wear = [&](std::uint64_t peer, std::size_t slot) {
                const auto inv = f.game->engine().view().player(contracts::PlayerKey{peer, peer}).value();
                return inv ? inv->slots[slot].damage : -1;
            };
            const auto heal = [&] {
                command(1, "effect give alice minecraft:instant_health 1 3");
                command(1, "effect give bob minecraft:instant_health 1 3");
                (void)ticks(30);
            };
            const auto sound = [&](std::int32_t v772_id, std::int32_t v769_id) { return std::format("sound {}", v772 ? v772_id : v769_id); };
            const auto hurt = sound(1175, 1135), death = sound(1174, 1134), strong = sound(1168, 1128), nodamage = sound(1167, 1127),
                       sweep = sound(1169, 1129), thorns = sound(1462, 1421);
            const std::string hit_bob = "damage bob 34 by alice alice", hit_carol = "damage carol 34 by alice alice", thorned = "damage alice 42 by bob bob",
                              knocked = "velocity alice -3200 2886 0", jump = "velocity alice 0 3359 0";
            const auto sweep_particle = std::format("particle {} count 0", v772 ? 64 : 63);
            for (const auto* c : {"gamerule naturalRegeneration false", "fill 4 -61 4 16 -61 12 dirt", "tp alice 8.5 -60 8.5 -90 0", "gamemode survival"})
                command(1, c);
            command(2, "tp bob 10.5 -60 8.5 90 0");
            command(2, "gamemode survival");
            command(3, "tp carol 8.5 -60 11.5 0 0");
            command(3, "gamemode survival");
            (void)ticks(2);
            f.game->submit(1, I{server::intent::ConfirmTeleport{last_teleport}});
            f.game->submit(1, I{server::intent::Move{std::nullopt, std::pair{-90.0f, 0.0f}, true}});
            f.game->submit(2, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            f.game->submit(3, I{server::intent::Move{std::nullopt, std::pair{0.0f, 0.0f}, true}});
            (void)ticks(30);
            // A hit without thorns: bob's velocity put back, told the first time.
            attack();
            (void)ticks(3);

            // Thorns VII on bob's chestplate: alice hurt for 1 to 5 by bob
            // (the thorns damage type), knocked 0.4 away from him (to the
            // others twice), after the attack's sound; 1.21.8 has the others
            // hear her hurt, then everyone the thorns sound, 1.21.4 the others
            // the thorns sound for the hurt; her health with the next tick's
            // wear, before bob's piece others see; the piece worn by 1 and 2.
            put(2, 6, "minecraft:iron_chestplate", {{"minecraft:thorns", 7}});
            heal();
            attack();
            auto t = ticks(3);
            const auto lost = healths(t.alice);
            require(in_order(t.alice, {hit_bob, hurt, strong, thorned, knocked, "health", "equipment bob"}) && lost.size() == 1 && lost[0] >= 15.0f
                        && lost[0] <= 19.0f && count(t.alice, knocked) == 1 && count(t.bob, knocked) == 2 && count(t.carol, knocked) == 2
                        && wear(2, 6) == 3,
                    std::format("{}: thorns: {} / {}", version, text(t.alice), text(t.bob)));
            require(v772 ? in_order(t.bob, {thorned, hurt, thorns}) && has(t.alice, thorns)
                         : in_order(t.bob, {thorned, thorns}) && !has(t.alice, thorns) && count(t.bob, hurt) == 0,
                    std::format("{}: the thorns sound: {} / {}", version, text(t.alice), text(t.bob)));
            // The knockback lifts her: her move up is not a jump; one from the
            // ground again is.
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -59.6, 8.5}, std::nullopt, false}});
            t = ticks(2);
            require(!has(t.bob, jump), std::format("{}: no jump by the knockback: {}", version, text(t.bob)));
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -60.0, 8.5}, std::nullopt, true}});
            (void)ticks(1);
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -59.6, 8.5}, std::nullopt, false}});
            t = ticks(2);
            require(has(t.bob, jump), std::format("{}: a jump after landing: {}", version, text(t.bob)));
            f.game->submit(1, I{server::intent::Move{server::Vec3{8.5, -60.0, 8.5}, std::nullopt, true}});
            (void)ticks(3);
            // Under resistance V: the hurt, no health lost.
            command(1, "effect give alice minecraft:resistance 600 4");
            heal();
            attack();
            t = ticks(3);
            require(has(t.alice, thorned) && healths(t.alice).empty() && wear(2, 6) == 6, std::format("{}: resistance V: {}", version, text(t.alice)));
            // A harder hit within bob's hurt cooldown: no thorns hurt (alice in
            // hers), the piece still worn by 2 more.
            heal();
            attack();
            (void)ticks(6);
            command(1, "effect give alice minecraft:strength 30 1");
            attack();
            t = ticks(3);
            require(!has(t.alice, thorned) && has(t.alice, strong) && wear(2, 6) == 12, std::format("{}: within the cooldown: {} ({})", version, text(t.alice), wear(2, 6)));
            command(1, "effect clear alice minecraft:strength");
            // An iron sword: thorns after the sweep's sound and particle, before
            // the damage indicators (bob lost 5.28: 2); the chestplate worn by
            // the hit's 1 and thorns' 2.
            put(1, 36, "minecraft:iron_sword", {});
            heal();
            attack();
            t = ticks(3);
            require(in_order(t.alice, {hit_bob, hurt, sweep, sweep_particle, thorned, "particle 6 count 2"}) && wear(2, 6) == 15,
                    std::format("{}: the sword: {}", version, text(t.alice)));
            put(1, 36, "", {});
            // bob in creative mode: no hurt, no thorns.
            heal();
            command(2, "gamemode creative");
            (void)ticks(2);
            attack();
            t = ticks(3);
            require(has(t.alice, nodamage) && !has(t.alice, thorned) && wear(2, 6) == 15, std::format("{}: bob in creative mode: {}", version, text(t.alice)));
            command(2, "gamemode survival");
            // alice in diamond armor: her pieces worn by thorns' hurt; bob sees
            // her pieces before his health.
            for (const auto& [slot, piece] : {std::pair{5, "minecraft:diamond_helmet"}, std::pair{6, "minecraft:diamond_chestplate"},
                                              std::pair{7, "minecraft:diamond_leggings"}, std::pair{8, "minecraft:diamond_boots"}})
                put(1, static_cast<std::int16_t>(slot), piece, {});
            heal();
            attack();
            t = ticks(3);
            require(has(t.alice, thorned) && wear(1, 5) == 1 && wear(1, 8) == 1 && in_order(t.bob, {"equipment alice", "health"}),
                    std::format("{}: alice in diamond armor: {}", version, text(t.bob)));
            for (const std::int16_t slot : {5, 6, 7, 8}) put(1, slot, "", {});
            // Thorns on a sword bob holds (no chestplate): alice hurt, the
            // sword worn by 2.
            put(2, 6, "", {});
            put(2, 36, "minecraft:iron_sword", {{"minecraft:thorns", 7}});
            heal();
            attack();
            t = ticks(3);
            require(has(t.alice, thorned) && wear(2, 36) == 2, std::format("{}: thorns held: {}", version, text(t.alice)));
            put(2, 36, "", {});
            // Two pieces of thorns: one hurt (the second within alice's
            // cooldown), both worn by 1 and 2.
            put(2, 6, "minecraft:iron_chestplate", {{"minecraft:thorns", 7}});
            put(2, 7, "minecraft:iron_leggings", {{"minecraft:thorns", 7}});
            heal();
            attack();
            t = ticks(3);
            require(count(t.alice, thorned) == 1 && wear(2, 6) == 3 && wear(2, 7) == 3, std::format("{}: two pieces: {}", version, text(t.alice)));
            put(2, 7, "", {});
            // alice in creative mode: no hurt, the piece still worn by 2.
            heal();
            command(1, "gamemode creative");
            (void)ticks(2);
            attack();
            t = ticks(3);
            require(!has(t.alice, thorned) && has(t.alice, hit_bob) && wear(2, 6) == 6, std::format("{}: alice in creative mode: {} ({})", version, text(t.alice), wear(2, 6)));
            command(1, "gamemode survival");
            // carol beside bob, swept, in a chestplate of thorns: alice hurt by
            // carol right after carol's hurt, before the sweep's sound.
            put(2, 6, "", {});
            put(3, 6, "minecraft:iron_chestplate", {{"minecraft:thorns", 7}});
            put(1, 36, "minecraft:iron_sword", {});
            command(3, "tp carol 10.5 -60 9.5 90 0");
            heal();
            attack();
            t = ticks(3);
            // Each player's velocities together: bob is told alice's twice
            // (her knockback from carol changed it), then carol's twice.
            const std::string from_carol = "velocity alice -2862 2886 -1431",
                              swept = v772 ? "velocity carol 4631 3200 715" : "velocity carol 4462 3200 1431";
            require(in_order(t.alice, {hit_bob, hurt, hit_carol, hurt, "damage alice 42 by carol carol", sweep}) && wear(3, 6) == 3
                        && in_order(t.bob, {from_carol, from_carol, swept, swept}) && count(t.bob, from_carol) == 2 && count(t.bob, swept) == 2,
                    std::format("{}: carol swept: {} / {}", version, text(t.alice), text(t.bob)));
            put(3, 6, "", {});
            put(1, 36, "", {});
            command(3, "tp carol 8.5 -60 11.5 0 0");
            // Fire aspect on a hit within bob's hurt cooldown sets him burning.
            command(1, "effect clear alice minecraft:resistance");
            put(1, 37, "minecraft:iron_sword", {{"minecraft:fire_aspect", 2}});
            heal();
            attack();
            (void)ticks(3);
            f.game->submit(1, I{server::intent::HeldSlot{1}});
            (void)ticks(3);
            attack();
            t = ticks(200);
            require(count(t.alice, "damage bob 31 by none none") == 7, std::format("{}: fire aspect within the cooldown: {}", version, count(t.alice, "damage bob 31 by none none")));
            f.game->submit(1, I{server::intent::HeldSlot{0}});
            put(1, 37, "", {});
            // Poisoned to 1 health, alice is killed by thorns: the death sound
            // (1.21.8 then the thorns sound), her swing after her death still
            // shown.
            put(2, 6, "minecraft:iron_chestplate", {{"minecraft:thorns", 7}});
            heal();
            command(1, "effect give alice minecraft:instant_damage 1 1");
            (void)ticks(2);
            command(1, "effect give alice minecraft:poison 30 0");
            t = ticks(300);
            const auto poisoned = healths(t.alice);
            require(!poisoned.empty() && poisoned.back() == 1.0f, std::format("{}: poisoned to 1: {}", version, poisoned.empty() ? -1.0f : poisoned.back()));
            command(1, "effect clear alice minecraft:poison");
            (void)ticks(2);
            attack();
            t = ticks(3);
            require(has(t.alice, "death screen death.attack.thorns") && in_order(t.bob, {thorned, death}) && has(t.bob, "animation alice 0")
                        && (v772 ? in_order(t.bob, {thorned, death, thorns}) : !has(t.bob, thorns)),
                    std::format("{}: killed by thorns: {}", version, text(t.bob)));
        }
        if (!ran) std::puts("  (no official server jar given: the thorns checks did not run)");
    });

    suite.run("unbreaking spares players' items' wear as both official servers do (client/ench_unbreaking)", [] {
        // circuit-clienttest/ench-unbreaking.js: alice (8.5 -60 8.5, facing
        // east) and bob (10.5 -60 8.5, facing west). Unbreaking III removes
        // each point of wear with a chance of 0.3 for armor and 0.75 for
        // other items (Circuit's own draws): the totals within 4 standard
        // deviations of their means, a use that wore nothing telling the
        // holder no slot. The enchantments need the official data packs
        // (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        using I = server::Intent;
        bool ran = false;
        for (const auto& [version, variable] : {std::pair{"1.21.8", "CIRCUIT_VANILLA_JAR_1_21_8"}, std::pair{"1.21.4", "CIRCUIT_VANILLA_JAR_1_21_4"}}) {
            const char* jar = std::getenv(variable);
            if (!jar || !*jar) continue;
            ran = true;
            const auto& bundle = **contracts::BundleRegistry::find(std::format("java-{}", version));
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value() && d->game, d ? "no game data" : d.error());
            const auto enchanted = d->game->enchanted;
            GameFixture f(version, offline_config(), std::move(*d));
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::int32_t alice_id = 0, bob_id = 0, last_teleport = 0;
            for (const auto& [r, into] : {std::pair{alice.get(), &alice_id}, std::pair{bob.get(), &bob_id}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) *into = pk->fields.i32().value();
            }
            // The slots told to alice since the last call (window 0).
            std::size_t seen = 0;
            const auto slots_told = [&] {
                const auto slot_id = f.wire.ids().id(State::play, Bound::clientbound, "container_set_slot");
                const auto position = f.wire.ids().id(State::play, Bound::clientbound, "player_position");
                std::vector<std::int32_t> out;
                std::lock_guard lock(alice->mutex);
                for (; seen < alice->packets.size(); ++seen) {
                    auto pk = protocol::open_packet(alice->packets[seen]);
                    if (!pk) continue;
                    if (pk->id == position) {
                        last_teleport = pk->fields.varint().value();
                    } else if (pk->id == slot_id) {
                        if (pk->fields.varint().value() != 0) continue;
                        (void)pk->fields.varint();
                        out.push_back(pk->fields.i16().value());
                    }
                }
                return out;
            };
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const auto put = [&](std::uint64_t who, std::int16_t slot, std::string_view name, std::vector<std::pair<std::string, std::int32_t>> levels) {
                command(who, "gamemode creative");
                ticks(1);
                if (name.empty()) {
                    f.game->submit(who, I{server::intent::CreativeSlot{slot, 0, 0}});
                } else {
                    auto patch = levels.empty() ? std::optional<contracts::ComponentPatch>(contracts::ComponentPatch{}) : enchanted(levels);
                    require(patch.has_value(), "enchanted");
                    f.game->submit(who, I{server::intent::CreativeSlot{slot, 1, tables.item_id(name).value(), 0, *patch}});
                }
                ticks(1);
                command(who, "gamemode survival");
                ticks(2);
            };
            const auto wear = [&](std::uint64_t peer, std::size_t slot) {
                const auto inv = f.game->engine().view().player(contracts::PlayerKey{peer, peer}).value();
                return inv ? static_cast<std::int32_t>(inv->slots[slot].damage) : -1;
            };
            // Within 4 standard deviations of the binomial mean of `n` points kept with `keep`.
            const auto within = [](std::int32_t total, int n, double keep) {
                const double mean = n * keep, sd = std::sqrt(n * keep * (1.0 - keep));
                return std::abs(total - mean) <= 4.0 * sd;
            };
            const std::vector<std::pair<std::string, std::int32_t>> unbreaking{{"minecraft:unbreaking", 3}};
            for (const auto* c : {"gamerule naturalRegeneration false", "fill 4 -61 4 16 -61 12 stone", "tp alice 8.5 -60 8.5 -90 0", "gamemode survival"})
                command(1, c);
            command(2, "tp bob 10.5 -60 8.5 90 0");
            command(2, "gamemode survival");
            ticks(2);
            (void)slots_told();
            f.game->submit(1, I{server::intent::ConfirmTeleport{last_teleport}});
            f.game->submit(1, I{server::intent::Move{std::nullopt, std::pair{-90.0f, 0.0f}, true}});
            f.game->submit(2, I{server::intent::Move{std::nullopt, std::pair{90.0f, 0.0f}, true}});
            ticks(30);
            // Repeats `use` n times, `gap` ticks apart: the uses that wore the
            // item in `slot` of `peer` and the slots told to alice of it.
            struct Worn {
                std::int32_t total{}, uses_worn{}, told{};
                std::vector<std::int32_t> each; // each use's wear
            };
            const auto repeat = [&](int n, int gap, std::uint64_t peer, std::size_t slot, const std::function<void(int)>& use) {
                Worn w;
                const auto start = wear(peer, slot);
                (void)slots_told();
                for (int k = 0; k < n; ++k) {
                    const auto before = wear(peer, slot);
                    use(k);
                    ticks(gap);
                    w.each.push_back(wear(peer, slot) - before);
                    if (wear(peer, slot) != before) ++w.uses_worn;
                    if (peer == 1)
                        for (const auto s : slots_told()) w.told += s == static_cast<std::int32_t>(slot) ? 1 : 0;
                }
                w.total = wear(peer, slot) - start;
                return w;
            };
            // An iron sword's 100 hits on bob, under resistance V: each a point
            // kept with 0.25.
            command(2, "effect give bob minecraft:resistance 6000 4 true");
            put(1, 36, "minecraft:iron_sword", unbreaking);
            auto w = repeat(100, 11, 1, 36, [&](int) { f.game->submit(1, I{server::intent::Attack{bob_id}}); });
            require(within(w.total, 100, 0.25) && w.uses_worn == w.total && w.told == w.uses_worn && w.uses_worn < 100,
                    std::format("{}: the sword worn by {} in {} hits, {} slots told", version, w.total, w.uses_worn, w.told));
            // Uses wearing more than a point draw for each point: an iron
            // axe's 40 hits (2 points each), some worn by 1.
            const auto between = [](const Worn& worn, std::int32_t low, std::int32_t high) {
                return std::ranges::all_of(worn.each, [&](std::int32_t d) { return d >= low && d <= high; });
            };
            const auto in_part = [](const Worn& worn, std::int32_t points) {
                return std::ranges::count_if(worn.each, [&](std::int32_t d) { return d > 0 && d < points; });
            };
            put(1, 36, "minecraft:iron_axe", unbreaking);
            w = repeat(40, 25, 1, 36, [&](int) { f.game->submit(1, I{server::intent::Attack{bob_id}}); });
            require(within(w.total, 80, 0.25) && between(w, 0, 2) && in_part(w, 2) > 0 && w.told == w.uses_worn,
                    std::format("{}: the axe worn by {} in 40 hits, {} by 1, {} slots told", version, w.total, in_part(w, 2), w.told));
            // Iron armor's 60 hurts by bob's fist, alice regenerating: each
            // point kept with 0.7, piece by piece.
            put(1, 36, "", {});
            const std::array<const char*, 4> pieces{"minecraft:iron_helmet", "minecraft:iron_chestplate", "minecraft:iron_leggings", "minecraft:iron_boots"};
            for (std::int16_t k = 0; k < 4; ++k) put(1, static_cast<std::int16_t>(5 + k), pieces[static_cast<std::size_t>(k)], unbreaking);
            command(1, "effect give alice minecraft:regeneration 6000 4 true");
            std::array<std::int32_t, 4> before{};
            for (std::size_t k = 0; k < 4; ++k) before[k] = wear(1, 5 + k);
            w = repeat(60, 11, 1, 6, [&](int) { f.game->submit(2, I{server::intent::Attack{alice_id}}); });
            for (std::size_t k = 0; k < 4; ++k)
                require(within(wear(1, 5 + k) - before[k], 60, 0.7) && wear(1, 5 + k) - before[k] < 60,
                        std::format("{}: armor slot {} worn by {} in 60 hurts", version, 5 + k, wear(1, 5 + k) - before[k]));
            require(w.told == w.uses_worn, std::format("{}: the chestplate's slot told {} times, worn in {} hurts", version, w.told, w.uses_worn));
            command(1, "effect clear alice minecraft:regeneration");
            for (std::int16_t k = 0; k < 4; ++k) put(1, static_cast<std::int16_t>(5 + k), "", {});
            // alice's shield raised against bob's iron sword: 30 blocks of 7
            // points each, the blocks worn in part.
            std::int32_t seq = 100;
            put(1, 45, "minecraft:shield", unbreaking);
            put(2, 36, "minecraft:iron_sword", {});
            f.game->submit(1, I{server::intent::Use{1, ++seq, -90.0f, 0.0f}});
            ticks(12);
            w = repeat(30, 15, 1, 45, [&](int) { f.game->submit(2, I{server::intent::Attack{alice_id}}); });
            require(within(w.total, 210, 0.25) && between(w, 0, 7) && in_part(w, 7) > 0 && w.told == w.uses_worn,
                    std::format("{}: the shield worn by {} in 30 blocks, {} in part, {} slots told", version, w.total, in_part(w, 7), w.told));
            f.game->submit(1, I{server::intent::Dig{5, {}, 0, 0}});
            put(2, 36, "", {});
            put(1, 45, "", {});
            // bob's chestplate of thorns VII hit 39 times by alice's fist
            // (alice under resistance V, bob regenerating): the hurt's point
            // and thorns' 2, each kept with the armor's 0.7.
            command(2, "effect clear bob minecraft:resistance");
            command(2, "effect give bob minecraft:regeneration 6000 4 true");
            command(1, "effect give alice minecraft:resistance 6000 4 true");
            put(2, 6, "minecraft:iron_chestplate", {{"minecraft:thorns", 7}, {"minecraft:unbreaking", 3}});
            w = repeat(39, 11, 2, 6, [&](int) { f.game->submit(1, I{server::intent::Attack{bob_id}}); });
            // Some hit worn by at most 1: impossible were thorns' 2 not spared.
            require(within(w.total, 117, 0.7) && w.total < 117 && between(w, 0, 3) && in_part(w, 3) > 0
                        && std::ranges::any_of(w.each, [](std::int32_t d) { return d <= 1; }),
                    std::format("{}: the thorns chestplate worn by {} in 39 hits, {} in part", version, w.total, in_part(w, 3)));
            command(1, "effect clear alice minecraft:resistance");
            put(2, 6, "", {});
            // Flint and steel's 40 fires, and an iron shovel of efficiency V's
            // 40 dirt blocks broken at once: each a point kept with 0.25.
            put(1, 36, "minecraft:flint_and_steel", unbreaking);
            // Its sound (item.flintandsteel.use, block category) to bob for
            // each fire, to alice, who used it, never.
            const auto flint_sounds = [&](Recorder& r, std::size_t from) {
                const auto sound = f.wire.ids().id(State::play, Bound::clientbound, "sound");
                int n = 0;
                std::lock_guard lock(r.mutex);
                for (auto k = from; k < r.packets.size(); ++k)
                    if (auto pk = protocol::open_packet(r.packets[k]); pk && pk->id == sound && pk->fields.varint().value() == (version == std::string_view{"1.21.8"} ? 559 : 546)
                                                                      && pk->fields.varint().value() == 4)
                        ++n;
                return n;
            };
            const auto packets_of = [](Recorder& r) {
                std::lock_guard lock(r.mutex);
                return r.packets.size();
            };
            const auto alice_from = packets_of(*alice), bob_from = packets_of(*bob);
            w = repeat(40, 3, 1, 36, [&](int) {
                command(1, "setblock 8 -60 6 air");
                f.game->submit(1, I{server::intent::UseOn{0, {8, -61, 6}, 1, 0.5f, 1.0f, 0.5f, false, ++seq}});
            });
            require(within(w.total, 40, 0.25) && w.total > 0 && w.total < 40 && w.told == w.uses_worn,
                    std::format("{}: flint and steel worn by {} in 40 uses, {} slots told", version, w.total, w.told));
            require(flint_sounds(*bob, bob_from) == 40 && flint_sounds(*alice, alice_from) == 0,
                    std::format("{}: flint and steel's sound {} times to bob, {} to alice", version, flint_sounds(*bob, bob_from), flint_sounds(*alice, alice_from)));
            put(1, 36, "minecraft:iron_shovel", {{"minecraft:efficiency", 5}, {"minecraft:unbreaking", 3}});
            w = repeat(40, 3, 1, 36, [&](int) {
                command(1, "setblock 8 -60 6 dirt");
                f.game->submit(1, I{server::intent::Dig{0, {8, -60, 6}, 1, ++seq}});
            });
            require(within(w.total, 40, 0.25) && w.total > 0 && w.total < 40 && w.told == w.uses_worn,
                    std::format("{}: the shovel worn by {} in 40 breaks, {} slots told", version, w.total, w.told));
        }
        if (!ran) std::puts("  (no official server jar given: the unbreaking checks did not run)");
    });

    suite.run("mending repairs players' items with the orbs they take as both official servers do (client/ench_mending)", [] {
        // circuit-clienttest/ench-mending.js: orbs summoned at alice's feet
        // (0.5 -60 0.5), bob 10 blocks east. The enchantments need the
        // official data packs (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        using I = server::Intent;
        bool ran = false;
        for (const auto& [version, variable] : {std::pair{"1.21.8", "CIRCUIT_VANILLA_JAR_1_21_8"}, std::pair{"1.21.4", "CIRCUIT_VANILLA_JAR_1_21_4"}}) {
            const char* jar = std::getenv(variable);
            if (!jar || !*jar) continue;
            ran = true;
            const auto& bundle = **contracts::BundleRegistry::find(std::format("java-{}", version));
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value() && d->game, d ? "no game data" : d.error());
            const auto enchanted = d->game->enchanted;
            GameFixture f(version, offline_config(), std::move(*d));
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            const bool v772 = version == std::string_view{"1.21.8"};
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const std::vector<std::pair<std::string, std::int32_t>> mending{{"minecraft:mending", 1}};
            const auto put = [&](std::int16_t slot, std::string_view name, std::int32_t damage) {
                command(1, "gamemode creative");
                ticks(1);
                if (name.empty()) {
                    f.game->submit(1, I{server::intent::CreativeSlot{slot, 0, 0}});
                } else {
                    auto patch = enchanted(mending);
                    require(patch.has_value(), "enchanted");
                    f.game->submit(1, I{server::intent::CreativeSlot{slot, 1, tables.item_id(name).value(), damage, *patch}});
                }
                ticks(1);
                command(1, "gamemode survival");
                ticks(2);
            };
            const auto damage = [&](std::size_t slot) {
                const auto inv = f.game->engine().view().player(contracts::PlayerKey{1, 1}).value();
                return inv ? static_cast<std::int32_t>(inv->slots[slot].damage) : -1;
            };
            const auto total = [&] {
                const auto inv = f.game->engine().view().player(contracts::PlayerKey{1, 1}).value();
                return inv ? inv->xp_total : -1;
            };
            // The kinds of the packets alice is sent from `from` on: s a slot
            // of window 0, x her experience, t a pickup, r a removal, o an
            // orb's spawn (with its value on 1.21.4) and v an orb's value
            // (with the value).
            std::set<std::int32_t> orbs; // the orbs' entity ids seen
            const auto kinds = [&](std::size_t from) {
                const auto id = [&](const char* name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto slot = id("container_set_slot"), experience = id("set_experience"), take = id("take_item_entity"),
                           removal = id("remove_entities"), data = id("set_entity_data");
                const auto orb = v772 ? id("add_entity") : id("add_experience_orb");
                std::string out;
                std::lock_guard lock(alice->mutex);
                for (auto k = from; k < alice->packets.size(); ++k) {
                    auto pk = protocol::open_packet(alice->packets[k]);
                    if (!pk) continue;
                    if (pk->id == slot && pk->fields.varint().value() == 0) {
                        out += 's';
                    } else if (pk->id == experience) {
                        out += 'x';
                    } else if (pk->id == take) {
                        out += 't';
                    } else if (pk->id == removal) {
                        out += 'r';
                    } else if (pk->id == orb) {
                        orbs.insert(pk->fields.varint().value());
                        out += 'o';
                        if (!v772) {
                            for (int c = 0; c < 3; ++c) (void)pk->fields.f64();
                            out += std::to_string(pk->fields.i16().value());
                        }
                    } else if (pk->id == data && orbs.contains(pk->fields.varint().value())) {
                        const auto index = pk->fields.u8().value();
                        const auto type = pk->fields.varint().value();
                        out += 'v';
                        if (index == 8 && type == 1) out += std::to_string(pk->fields.varint().value());
                    }
                }
                return out;
            };
            const auto mark = [&] {
                std::lock_guard lock(alice->mutex);
                return alice->packets.size();
            };
            const auto orb = [&](std::int32_t value, std::string_view extra = "") {
                command(1, std::format("summon experience_orb 0.5 -60 0.5 {{Value:{},Motion:[0.0d,0.0d,0.0d]{}}}", value, extra));
            };
            for (const auto* c : {"gamerule doMobSpawning false", "tp alice 0.5 -60 0.5 0 0", "gamemode survival"}) command(1, c);
            command(2, "tp bob 10.5 -60 0.5 90 0");
            ticks(30);
            const auto origin = total();

            // An iron pickaxe of mending, damage 100: an orb of 7 repairs 14,
            // all its points used; alice is told the pickup, the removal and
            // the slot, no experience.
            put(36, "minecraft:iron_pickaxe", 100);
            auto from = mark();
            orb(7);
            ticks(6);
            const auto first = kinds(from);
            require(damage(36) == 86 && total() == origin && first.ends_with("trs") && first.find('x') == std::string::npos,
                    std::format("{}: 14 repaired: damage {}, total {}, packets {}", version, damage(36), total(), first));
            // Damage 3 and an orb of 2: whole, 1 to the bar, told before the slot.
            put(36, "minecraft:iron_pickaxe", 3);
            from = mark();
            orb(2);
            ticks(6);
            const auto second = kinds(from);
            require(damage(36) == 0 && total() == origin + 1 && second.ends_with("trxs"),
                    std::format("{}: 3 repaired for 1 point: damage {}, total {}, packets {}", version, damage(36), total(), second));
            // A whole pickaxe: all to the bar.
            orb(5);
            ticks(6);
            require(damage(36) == 0 && total() == origin + 6, std::format("{}: a whole pickaxe, total {}", version, total()));

            // The choice: 60 orbs of 1 over a damaged pickaxe, shield and
            // helmet (each within 4 standard deviations of 20 picks), never the
            // whole boots or a pickaxe in a slot not selected, no point to the bar.
            put(36, "minecraft:iron_pickaxe", 100);
            put(45, "minecraft:shield", 100);
            put(5, "minecraft:iron_helmet", 100);
            put(8, "minecraft:iron_boots", 0);
            put(37, "minecraft:iron_pickaxe", 100);
            const auto before = total();
            // The equip sounds alice hears from `from` on: iron armor's (72)
            // and the shield's (70).
            const auto equip_sounds = [&](std::size_t from, std::int32_t sound) {
                const auto id = f.wire.ids().id(State::play, Bound::clientbound, "sound");
                int n = 0;
                std::lock_guard lock(alice->mutex);
                for (auto k = from; k < alice->packets.size(); ++k)
                    if (auto pk = protocol::open_packet(alice->packets[k]); pk && pk->id == id && pk->fields.varint().value() == sound + 1) ++n;
                return n;
            };
            from = mark();
            orb(1, ",Count:60");
            ticks(140);
            const int pickaxe = (100 - damage(36)) / 2, shield = (100 - damage(45)) / 2, helmet = (100 - damage(5)) / 2;
            const auto fair = [](int n) { return std::abs(n - 20) <= 15; };
            require(pickaxe + shield + helmet == 60 && fair(pickaxe) && fair(shield) && fair(helmet) && damage(8) == 0 && damage(37) == 100
                        && total() == before && equip_sounds(from, 72) == 0 && equip_sounds(from, 70) == 0,
                    std::format("{}: picks {} {} {}, boots {}, slot 37 {}, total {} (was {}), equip sounds {} {}", version, pickaxe, shield, helmet,
                                damage(8), damage(37), total(), before, equip_sounds(from, 72), equip_sounds(from, 70)));
            // Set in creative mode in place of the same item of another
            // damage: the equip sound (the boots whole for ones of damage 100,
            // the shield repaired for one of damage 100).
            from = mark();
            put(8, "minecraft:iron_boots", 100);
            put(45, "minecraft:shield", 100);
            require(equip_sounds(from, 72) == 1 && equip_sounds(from, 70) == 1,
                    std::format("{}: equip sounds for the boots {} and the shield {}", version, equip_sounds(from, 72), equip_sounds(from, 70)));
            for (const std::int16_t s : {45, 5, 8, 37}) put(s, "", 0);

            // Orbs of -5 and of 0: shown (1.21.8 with the value twice, the
            // second a tick on, and none for 0), taken, nothing changed, also
            // with levels and points.
            command(1, "xp set alice 3 levels");
            command(1, "xp add alice 5 points");
            put(36, "minecraft:iron_pickaxe", 50);
            const auto kept = total();
            // The kinds tick by tick ('|' after each), the last empty ticks left out.
            const auto by_tick = [&](int n) {
                std::string out;
                for (int k = 0; k < n; ++k) {
                    const auto at = mark();
                    ticks(1);
                    out += kinds(at) + '|';
                }
                while (out.ends_with("||")) out.pop_back();
                if (out.ends_with('|')) out.pop_back();
                return out;
            };
            orb(-5);
            const auto negative = by_tick(6);
            orb(0);
            const auto zero = by_tick(6);
            require(damage(36) == 50 && total() == kept && negative == (v772 ? "ov-5|v-5tr" : "o-5|tr") && zero == (v772 ? "o|tr" : "o0|tr"),
                    std::format("{}: orbs of -5 and 0: damage {}, total {} (was {}), packets {} and {}", version, damage(36), total(), kept, negative, zero));
        }
        if (!ran) std::puts("  (no official server jar given: the mending checks did not run)");
    });

    suite.run("the curses of binding and vanishing act as both official servers show them (client/ench_curses)", [] {
        // circuit-clienttest/ench-curses.js and ench-curses-table.js; the
        // enchantments need the official data packs (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        using I = server::Intent;
        bool ran = false;
        for (const auto& [version, variable] : {std::pair{"1.21.8", "CIRCUIT_VANILLA_JAR_1_21_8"}, std::pair{"1.21.4", "CIRCUIT_VANILLA_JAR_1_21_4"}}) {
            const char* jar = std::getenv(variable);
            if (!jar || !*jar) continue;
            ran = true;
            const auto& bundle = **contracts::BundleRegistry::find(std::format("java-{}", version));
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value() && d->game, d ? "no game data" : d.error());
            const auto enchanted = d->game->enchanted;
            GameFixture f(version, offline_config(), std::move(*d));
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const auto item = [](std::string_view name) { return gameplay::items::by_name(name).value(); };
            // Stacks set in creative mode: a name, a curse or none, a count.
            const auto put = [&](std::vector<std::tuple<std::int16_t, std::string_view, std::string_view, std::int32_t>> stacks,
                                 std::string_view mode = "survival") {
                command(1, "gamemode creative");
                ticks(1);
                for (const auto& [slot, name, curse, count] : stacks) {
                    if (name.empty()) {
                        f.game->submit(1, I{server::intent::CreativeSlot{slot, 0, 0}});
                        continue;
                    }
                    contracts::ComponentPatch patch;
                    if (!curse.empty()) {
                        auto made = enchanted({{std::string(curse), 1}});
                        require(made.has_value(), "enchanted");
                        patch = *made;
                    }
                    f.game->submit(1, I{server::intent::CreativeSlot{slot, count, tables.item_id(name).value(), 0, patch}});
                }
                ticks(1);
                command(1, std::format("gamemode {}", mode));
                ticks(2);
            };
            const auto inventory = [&] {
                const auto inv = f.game->engine().view().player(contracts::PlayerKey{1, 1}).value();
                return inv ? *inv : gameplay::PlayerInventory{};
            };
            const auto items_out = [&] {
                std::vector<std::uint16_t> out;
                const auto dump = f.game->engine().dump();
                for (const auto& e : dump.overworld().entities)
                    if (e.kind == gameplay::EntityKind::item) out.push_back(e.stack.item);
                return out;
            };
            const auto click = [&](std::int16_t slot, std::int8_t button, std::int32_t mode) {
                f.game->submit(1, window_click(0, slot, button, mode));
                ticks(1);
            };
            // The players' entity ids, and since `from` the packets a client
            // got, as text: "event <who> <n>", "spawn", "slot <window> <slot>",
            // "health", "attributes <who>", "open <window>", "close", "cursor",
            // "content <window> [<slots not empty>] carried <0 or 1>" (plain
            // stacks alone read; "?" past one with components).
            std::map<std::string, std::int32_t> ids;
            std::map<std::int32_t, std::string> names;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) {
                        const auto e = pk->fields.i32().value();
                        names[e] = name;
                        ids[name] = e;
                    }
            }
            const auto mark = [&](Recorder& r) {
                std::lock_guard lock(r.mutex);
                return r.packets.size();
            };
            const auto since = [&](Recorder& r, std::size_t from) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto k = from; k < r.packets.size(); ++k) {
                    auto pk = protocol::open_packet(r.packets[k]);
                    if (!pk) continue;
                    if (pk->id == id("entity_event")) {
                        const auto e = pk->fields.i32().value();
                        out.push_back(std::format("event {} {}", names.contains(e) ? names[e] : "?", pk->fields.u8().value()));
                    } else if (pk->id == id("add_entity")) {
                        out.push_back("spawn");
                    } else if (pk->id == id("container_set_slot")) {
                        const auto window = pk->fields.varint().value();
                        (void)pk->fields.varint();
                        out.push_back(std::format("slot {} {}", window, pk->fields.i16().value()));
                    } else if (pk->id == id("set_health")) {
                        out.push_back("health");
                    } else if (pk->id == id("update_attributes")) {
                        const auto e = pk->fields.varint().value();
                        out.push_back(std::format("attributes {}", names.contains(e) ? names[e] : "?"));
                    } else if (pk->id == id("open_screen")) {
                        out.push_back(std::format("open {}", pk->fields.varint().value()));
                    } else if (pk->id == id("container_close")) {
                        out.push_back("close");
                    } else if (pk->id == id("set_cursor_item")) {
                        out.push_back("cursor");
                    } else if (pk->id == id("container_set_content")) {
                        const auto window = pk->fields.varint().value();
                        (void)pk->fields.varint();
                        const auto n = pk->fields.varint().value();
                        std::string full;
                        bool plain = true;
                        const auto present = [&] {
                            if (pk->fields.varint().value() == 0) return false;
                            (void)pk->fields.varint();
                            plain = pk->fields.varint().value() == 0 && pk->fields.varint().value() == 0;
                            return true;
                        };
                        for (std::int32_t k = 0; k < n && plain; ++k)
                            if (present()) full += std::format("{}{}", full.empty() ? "" : ",", k);
                        const auto carried = plain ? (present() ? "1" : "0") : "?";
                        out.push_back(std::format("content {} [{}] carried {}", window, plain ? full : "?", carried));
                    }
                }
                return out;
            };
            for (const auto* c : {"gamerule doMobSpawning false", "tp alice 0.5 -60 0.5 0 0", "gamemode survival"}) command(1, c);
            command(2, "tp bob 10.5 -60 0.5 90 0");
            ticks(30);

            // Binding: no click takes the bound helmet off in survival, nor a
            // use swaps it; in creative mode a click does.
            put({{5, "minecraft:iron_helmet", "minecraft:binding_curse", 1}, {36, "minecraft:diamond_helmet", "", 1}});
            const auto bound = inventory().slots[5];
            for (const auto& [slot, button, mode] : std::vector<std::tuple<std::int16_t, std::int8_t, std::int32_t>>{
                     {5, 0, 0}, {5, 0, 1}, {5, 1, 2}, {5, 40, 2}, {5, 0, 4}, {5, 1, 4}})
                click(slot, button, mode);
            require(inventory().slots[5] == bound && inventory().cursor.empty() && inventory().slots[45].empty() && items_out().empty(),
                    std::format("{}: the bound helmet stays through every click", version));
            f.game->submit(1, I{server::intent::HeldSlot{0}});
            f.game->submit(1, I{server::intent::Use{0, 1, 0.0f, 0.0f}});
            ticks(2);
            require(inventory().slots[5] == bound && inventory().slots[36].item == item("minecraft:diamond_helmet"),
                    std::format("{}: a helmet used over it changes nothing", version));
            command(1, "gamemode creative");
            ticks(2);
            click(5, 0, 0);
            require(inventory().slots[5].empty() && inventory().cursor == bound, std::format("{}: in creative mode a click takes it", version));
            click(5, 0, 0);
            // Each creative set of the own grid's slots sends the result again
            // (ench-curses.js: four empty slots set empty, four results).
            auto at = mark(*alice);
            f.game->submit(1, I{server::intent::CreativeSlot{1, 0, 0}});
            f.game->submit(1, I{server::intent::CreativeSlot{2, 0, 0}});
            f.game->submit(1, I{server::intent::CreativeSlot{9, 0, 0}});
            ticks(1);
            auto seen = since(*alice, at);
            std::erase_if(seen, [](const auto& x) { return !x.starts_with("slot"); });
            require(seen == std::vector<std::string>{"slot 0 0", "slot 0 0"}, std::format("{}: the result sent again for each grid slot set ({})", version, [&] {
                        std::string t;
                        for (const auto& x : seen) t += x + "; ";
                        return t;
                    }()));
            command(1, "gamemode survival");
            ticks(2);

            // A bound helmet of damage 164 broken by bob's fist: its break
            // event (49) to both before the slot is told, and the slot freed.
            {
                command(1, "gamemode creative");
                ticks(1);
                auto patch = enchanted({{"minecraft:binding_curse", 1}});
                require(patch.has_value(), "enchanted");
                f.game->submit(1, I{server::intent::CreativeSlot{5, 1, tables.item_id("minecraft:iron_helmet").value(), 164, *patch}});
                ticks(1);
                command(1, "gamemode survival");
                command(2, "gamemode survival");
                command(2, "tp bob 2.0 -60 0.5 90 0");
                ticks(10);
            }
            const auto at_alice = mark(*alice), at_bob = mark(*bob);
            f.game->submit(2, I{server::intent::Attack{ids.at("alice")}});
            ticks(3);
            const auto alice_saw = since(*alice, at_alice), bob_saw = since(*bob, at_bob);
            const auto first = [](const std::vector<std::string>& v, std::string_view x) { return std::ranges::find(v, x) - v.begin(); };
            require(inventory().slots[5].empty() && first(alice_saw, "event alice 49") < first(alice_saw, "slot 0 5")
                        && first(alice_saw, "slot 0 5") < first(alice_saw, "health") && first(alice_saw, "health") < first(alice_saw, "attributes alice")
                        && first(alice_saw, "attributes alice") < static_cast<std::ptrdiff_t>(alice_saw.size())
                        && std::ranges::find(bob_saw, "event alice 49") != bob_saw.end(),
                    std::format("{}: the helmet broke, its event, then its slot, the health and the attributes", version));
            command(2, "tp bob 10.5 -60 0.5 90 0");
            ticks(2);

            // Vanishing at death: the vanishing stacks of the inventory are
            // gone, the rest drops; the grid's and the carried stack stay
            // until the body goes 20 ticks later, then are thrown, vanishing
            // or not, the carried stack first.
            command(1, "tp alice 40.5 -60 8.5 0 0");
            ticks(2);
            put({{5, "minecraft:iron_helmet", "minecraft:vanishing_curse", 1}, {36, "minecraft:iron_sword", "minecraft:vanishing_curse", 1},
                 {37, "minecraft:stone", "", 5}, {38, "minecraft:redstone", "minecraft:vanishing_curse", 3},
                 {39, "minecraft:feather", "minecraft:vanishing_curse", 1}});
            click(38, 0, 0);
            click(1, 0, 0);
            click(39, 0, 0);
            require(inventory().slots[1].item == item("minecraft:redstone") && inventory().cursor.item == item("minecraft:feather"),
                    std::format("{}: redstone in the grid, a feather carried", version));
            command(2, "tp bob 60.5 -60 8.5 90 0");
            ticks(5);
            at = mark(*bob);
            command(1, "kill");
            ticks(3);
            seen = since(*bob, at);
            require(std::ranges::find(seen, "spawn") < std::ranges::find(seen, "event alice 3") && std::ranges::find(seen, "event alice 3") != seen.end(),
                    std::format("{}: the others see the drops appear before the death animation", version));
            require(items_out() == std::vector<std::uint16_t>{gameplay::items::stone}, std::format("{}: only the stone dropped at the death ({})", version, items_out().size()));
            require(inventory().slots[1].item == item("minecraft:redstone") && inventory().cursor.item == item("minecraft:feather")
                        && inventory().slots[5].empty() && inventory().slots[36].empty(),
                    std::format("{}: the grid and the carried stack stay, the vanishing stacks gone", version));
            at = mark(*bob);
            const auto quiet = mark(*alice);
            ticks(20);
            require(items_out() == std::vector<std::uint16_t>{gameplay::items::stone, item("minecraft:feather"), item("minecraft:redstone")},
                    std::format("{}: the body gone, the feather then the redstone thrown ({})", version, items_out().size()));
            seen = since(*bob, at);
            require(std::ranges::find(seen, "event alice 60") < std::ranges::find(seen, "spawn") && std::ranges::count(seen, "spawn") == 2,
                    std::format("{}: the body's last event before the throw", version));
            seen = since(*alice, quiet);
            require(std::ranges::none_of(seen, [](const auto& x) { return x.starts_with("slot") || x == "cursor" || x.starts_with("content"); }),
                    std::format("{}: the dead player told nothing of the throw", version));
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            ticks(3);

            // A respawn before the body goes throws the grid and the carried
            // stack then; the respawn's window shows them gone.
            command(1, "tp alice 40.5 -60 16.5 0 0");
            ticks(2);
            put({{38, "minecraft:clay_ball", "", 2}, {39, "minecraft:feather", "", 1}});
            click(38, 0, 0);
            click(1, 0, 0);
            click(39, 0, 0);
            command(1, "kill");
            ticks(3);
            at = mark(*alice);
            const auto thrown = items_out().size();
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            ticks(1);
            seen = since(*alice, at);
            const auto content = std::ranges::find_if(seen, [](const auto& x) { return x.starts_with("content 0"); });
            require(content != seen.end() && *content == "content 0 [] carried 0",
                    std::format("{}: the respawn's window without the grid and the carried stack ({})", version, content != seen.end() ? *content : "none"));
            at = mark(*alice);
            ticks(3);
            seen = since(*alice, at);
            require(items_out().size() == thrown + 2 && items_out()[thrown] == item("minecraft:feather") && items_out()[thrown + 1] == item("minecraft:clay_ball")
                        && std::ranges::none_of(seen, [](const auto& x) { return x.starts_with("slot 0 1") || x == "cursor"; }),
                    std::format("{}: thrown at the respawn, the feather first, nothing told", version));

            // A crafting table open at death: no close, the death's slots told
            // in its window; its grid and the carried stack thrown when the
            // body goes, or at once by a close from the dead player's client.
            for (const bool closes : {false, true}) {
                const auto z = closes ? 32 : 24;
                command(1, std::format("tp alice 20.5 -60 {}.5 0 0", z));
                command(1, std::format("setblock 20 -60 {} crafting_table", z + 2));
                ticks(3);
                put({{36, "minecraft:clay_ball", "", 2}, {37, "minecraft:feather", "", 1}, {38, "minecraft:stone", "", 3}});
                at = mark(*alice);
                f.game->submit(1, I{server::intent::UseOn{0, {20, -60, z + 2}, 1, 0.5f, 1.0f, 0.5f, false, 900 + z}});
                ticks(2);
                seen = since(*alice, at);
                const auto opened = std::ranges::find_if(seen, [](const auto& x) { return x.starts_with("open "); });
                require(opened != seen.end(), std::format("{}: the table opened", version));
                const auto window = std::stoi(opened->substr(5));
                for (const auto& [slot, button] : std::vector<std::pair<std::int16_t, std::int8_t>>{{37, 0}, {1, 0}, {38, 0}}) {
                    f.game->submit(1, window_click(window, slot, button, 0));
                    ticks(1);
                }
                require(inventory().table[1].item == item("minecraft:clay_ball") && inventory().cursor.item == item("minecraft:feather"),
                        std::format("{}: clay in the table's grid, a feather carried", version));
                const auto before = items_out().size();
                at = mark(*alice);
                command(1, "kill");
                ticks(3);
                seen = since(*alice, at);
                require(std::ranges::find(seen, std::format("slot {} 39", window)) != seen.end() && std::ranges::find(seen, "close") == seen.end()
                            && items_out().size() == before + 1 && inventory().table[1].item == item("minecraft:clay_ball"),
                        std::format("{}: the stone's slot told in the table's window, no close, the grid kept", version));
                if (closes) {
                    f.game->submit(1, I{server::intent::ContainerClose{window}});
                    ticks(1);
                } else {
                    ticks(20);
                }
                require(items_out().size() == before + 3 && items_out()[before + 1] == item("minecraft:feather")
                            && items_out()[before + 2] == item("minecraft:clay_ball"),
                        std::format("{}: the feather then the table's clay thrown ({})", version, closes ? "closed by the client" : "the body gone"));
                f.game->submit(1, I{server::intent::ClientCommand{0}});
                ticks(3);
            }
            (void)bob;
        }
        if (!ran) std::puts("  (no official server jar given: the curse checks did not run)");
    });

    suite.run("players hear the level-up sound and are told their experience after /xp as both official servers do (client/xp_death)", [] {
        // circuit-clienttest/xp-levelup.js: alice gains levels by /xp, bob
        // listens 3 blocks away.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            GameFixture f(version, offline_config());
            const bool v772 = std::string_view(version) == "1.21.8";
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            // Since the last call: the level-up sounds (volume) and, for
            // alice, the experience packets (level, total).
            std::map<std::uint64_t, std::size_t> seen;
            const auto heard = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("sound")) {
                        const auto sound = in.varint().value() - 1, category = in.varint().value();
                        const auto x = in.i32().value(), y = in.i32().value(), z = in.i32().value();
                        const auto volume = in.f32().value();
                        const auto pitch = in.f32().value();
                        if (sound != (v772 ? 1180 : 1140)) continue;
                        // Where: alice's place (0.5 -60 0.5, in eighths) or not.
                        const bool hers = x == 4 && y == -480 && z == 4;
                        out.push_back(std::format("levelup category {} volume {}{}{}", category, volume, pitch == 1.0f ? "" : " pitch?",
                                                  hers ? "" : " elsewhere"));
                    } else if (pk->id == id("set_experience")) {
                        (void)in.f32();
                        const auto level = in.varint().value();
                        out.push_back(std::format("experience {} {}", level, in.varint().value()));
                    }
                }
                return out;
            };
            const auto command = [&](std::string text) {
                f.game->submit(1, I{server::intent::Command{std::move(text)}});
                f.game->tick();
                f.game->tick();
            };
            f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
            f.game->submit(2, I{server::intent::ConfirmTeleport{1}});
            command("gamemode survival");
            // alice at 0.5 -60 0.5, bob 3 blocks east.
            command("tp alice 0.5 -60 0.5");
            f.game->submit(1, I{server::intent::ConfirmTeleport{2}});
            f.game->submit(2, I{server::intent::Command{"tp bob 3.5 -60 0.5"}});
            f.game->tick();
            f.game->submit(2, I{server::intent::ConfirmTeleport{2}});
            f.game->tick();
            (void)heard(*alice), (void)heard(*bob);
            // Every /xp change is told, changed or not.
            command("xp set alice 0 levels");
            command("xp add alice 0 points");
            require(heard(*alice) == std::vector<std::string>{"experience 0 0", "experience 0 0"}, std::format("{}: unchanged, told", version));
            using V = std::vector<std::string>;
            // Levels added to 3, then to 6 (passing 5): nothing; to 10: the
            // sound for both, a 30th of 0.75 a level.
            command("xp add alice 3 levels");
            command("xp add alice 3 levels");
            require(heard(*bob).empty(), "no multiple of 5 reached");
            command("xp add alice 4 levels");
            require(heard(*alice) == V{"experience 3 0", "experience 6 0", "levelup category 7 volume 0.25", "experience 10 0"}, std::format("{}: level 10", version));
            require(heard(*bob) == V{"levelup category 7 volume 0.25"}, "bob hears it");
            // Within 100 ticks, not again (100 ticks after it, level 15: none;
            // 101, level 20: 0.5); a set, never.
            for (int k = 0; k < 98; ++k) f.game->tick();
            f.game->submit(1, I{server::intent::Command{"xp add alice 5 levels"}});
            f.game->tick();
            require(heard(*bob).empty(), "not again within 100 ticks");
            f.game->submit(1, I{server::intent::Command{"xp add alice 5 levels"}});
            f.game->tick();
            require(heard(*bob) == V{"levelup category 7 volume 0.5"}, std::format("{}: 101 ticks after", version));
            for (int k = 0; k < 100; ++k) f.game->tick();
            command("xp set alice 25 levels");
            require(heard(*bob).empty(), "a set to 25: none");
            // Points: the first multiple of 5 passed (55 points to 5; 200 through 5 and 10 to 11).
            command("xp set alice 0 levels");
            command("xp add alice 55 points");
            require(heard(*bob) == V{"levelup category 7 volume 0.125"}, std::format("{}: points to 5", version));
            for (int k = 0; k < 101; ++k) f.game->tick();
            command("xp set alice 0 levels");
            command("xp set alice 0 points");
            command("xp add alice 200 points");
            require(heard(*bob) == V{"levelup category 7 volume 0.125"}, std::format("{}: points through 5 and 10", version));
            for (int k = 0; k < 101; ++k) f.game->tick();
            // Up to 30 and beyond: 0.75; taken away: nothing.
            command("xp add alice 19 levels");
            require(heard(*bob) == V{"levelup category 7 volume 0.75"}, std::format("{}: level 30", version));
            for (int k = 0; k < 101; ++k) f.game->tick();
            command("xp add alice 10 levels");
            require(heard(*bob) == V{"levelup category 7 volume 0.75"}, "level 40");
            for (int k = 0; k < 101; ++k) f.game->tick();
            command("xp add alice -5 levels");
            require(heard(*bob).empty(), "levels taken: none");
        }
    });

    suite.run("players lose their experience at death as both official servers show it (client/xp_death)", [] {
        // circuit-clienttest/xp-death.js: alice, in survival with experience
        // and stone, dies by /kill; bob, a spectator, watches. Without keepInventory the
        // stone, then 7 points a level (at most 100) as orbs drop where she
        // stood, and the experience she is told comes only with the respawn,
        // after the first spawn position and after the health; with it,
        // nothing drops and she keeps her levels.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            GameFixture f(version, offline_config());
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            const contracts::PlayerKey key{1, 1};
            const auto stone = content().item_id("minecraft:stone").value();
            // Alice's packets since the last call, by name: the death, the
            // respawn, the position, the spawn position, the health, the
            // experience (progress, level, total) and her own window's slots.
            std::size_t seen = 0;
            const auto packets = [&] {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                std::vector<std::string> out;
                std::lock_guard lock(alice->mutex);
                for (; seen < alice->packets.size(); ++seen) {
                    auto pk = protocol::open_packet(alice->packets[seen]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("player_combat_kill")) out.push_back("death");
                    else if (pk->id == id("respawn")) out.push_back("respawn");
                    else if (pk->id == id("player_position")) out.push_back("position");
                    else if (pk->id == id("set_default_spawn_position")) out.push_back("spawn");
                    else if (pk->id == id("set_health")) out.push_back(std::format("health {}", in.f32().value()));
                    else if (pk->id == id("container_set_slot")) {
                        if (in.varint().value() != 0) continue;
                        (void)in.varint();
                        out.push_back(std::format("slot {}", in.i16().value()));
                    } else if (pk->id == id("set_experience")) {
                        const auto progress = in.f32().value();
                        const auto level = in.varint().value();
                        out.push_back(std::format("experience {} {} {}", progress, level, in.varint().value()));
                    }
                }
                return out;
            };
            const auto experience = [&] {
                const auto d = f.game->engine().dump();
                const auto p = std::ranges::find_if(d.players, [&](const auto& inv) { return inv.player == key; });
                return p == d.players.end() ? std::tuple{0, 0} : std::tuple{p->xp_level, p->xp_total};
            };
            // What is in the world: items by count, orbs by value, in order.
            const auto entities = [&] {
                std::vector<std::string> out;
                const auto d = f.game->engine().dump(); // kept: overworld() refers into it
                for (const auto& e : d.overworld().entities)
                    out.push_back(e.kind == gameplay::EntityKind::experience_orb ? std::format("orb {}", e.experience) : std::format("item {}", e.stack.count));
                return out;
            };
            f.game->submit(1, I{server::intent::ConfirmTeleport{1}});
            f.game->submit(1, I{server::intent::CreativeSlot{36, 16, stone}});
            f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
            f.game->submit(2, I{server::intent::Command{"gamemode spectator"}}); // beside her, not collecting
            f.game->tick();
            // 20 blocks from the spawn, where she respawns.
            f.game->submit(1, I{server::intent::Command{"tp alice 20.5 -60 0.5"}});
            f.game->tick();
            f.game->submit(1, I{server::intent::ConfirmTeleport{2}});
            f.game->submit(2, I{server::intent::Command{"tp bob 20.5 -60 3.5"}});
            f.game->submit(1, I{server::intent::Command{"xp add alice 3 levels"}});
            f.game->submit(1, I{server::intent::Command{"xp add alice 5 points"}});
            f.game->tick();
            f.game->tick();
            require(experience() == std::tuple{3, 5}, "given");
            (void)packets();
            // The death: the stone, then 21 points as 17, 3 and 1; nothing
            // told of the experience while dead.
            f.game->submit(1, I{server::intent::Command{"kill"}});
            for (int k = 0; k < 3; ++k) f.game->tick();
            require(entities() == std::vector<std::string>{"item 16", "orb 17", "orb 3", "orb 1"},
                    std::format("{}: the stone, then the orbs: {}", version, [&] { std::string t; for (const auto& e : entities()) t += e + ", "; return t; }()));
            require(experience() == std::tuple{0, 0}, "the experience gone");
            auto seen_now = packets();
            require(std::ranges::count(seen_now, std::string("death")) == 1
                        && std::ranges::none_of(seen_now, [](const auto& s) { return s.starts_with("experience"); }),
                    std::format("{}: no experience while dead", version));
            // The slot the drops empty, then the health of 0.
            {
                const auto slot = std::ranges::find(seen_now, std::string("slot 36"));
                const auto health = std::ranges::find(seen_now, std::string("health 0"));
                require(slot != seen_now.end() && health != seen_now.end() && slot < health,
                        std::format("{}: the emptied slot, then the health", version));
            }
            // The respawn: the experience after the first spawn position and
            // again after the health.
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            f.game->tick();
            seen_now = packets();
            const std::vector<std::string> respawned{"respawn", "position", "spawn", "experience 0 0 0", "spawn", "health 20", "experience 0 0 0"};
            require(seen_now == respawned, std::format("{}: the respawn's experience, twice: {}", version, [&] { std::string t; for (const auto& e : seen_now) t += e + ", "; return t; }()));
            for (int k = 0; k < 3; ++k) f.game->tick();
            require(packets().empty(), "no more experience");
            // Points alone drop nothing.
            const auto before = entities().size();
            f.game->submit(1, I{server::intent::ConfirmTeleport{3}});
            f.game->submit(1, I{server::intent::Command{"xp add alice 5 points"}});
            f.game->tick();
            require(experience() == std::tuple{0, 5}, "points given");
            (void)packets();
            f.game->submit(1, I{server::intent::Command{"kill"}});
            for (int k = 0; k < 3; ++k) f.game->tick();
            require(entities().size() == before && experience() == std::tuple{0, 0}, std::format("{}: points alone lost", version));
            seen_now = packets();
            require(std::ranges::count(seen_now, std::string("death")) == 1
                        && std::ranges::none_of(seen_now, [](const auto& s) { return s.starts_with("experience"); }),
                    std::format("{}: points alone, nothing told while dead", version));
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            f.game->tick();
            require(packets() == respawned, std::format("{}: points alone, the respawn", version));
            // A creative player's experience drops too; the orbs come at the
            // feet, below the items (thrown from 1.32 above them).
            f.game->submit(1, I{server::intent::ConfirmTeleport{4}});
            f.game->submit(1, I{server::intent::Command{"gamemode creative"}});
            f.game->submit(1, I{server::intent::CreativeSlot{36, 16, stone}});
            f.game->submit(1, I{server::intent::Command{"xp add alice 3 levels"}});
            f.game->tick();
            f.game->tick();
            {
                const auto from = f.game->engine().dump().overworld().entities.size();
                f.game->submit(1, I{server::intent::Command{"kill"}});
                f.game->tick();
                const auto d = f.game->engine().dump();
                const auto& all = d.overworld().entities;
                std::vector<std::string> kinds;
                double lowest_item = 1e9, highest_orb = -1e9;
                for (std::size_t k = from; k < all.size(); ++k) {
                    const bool orb = all[k].kind == gameplay::EntityKind::experience_orb;
                    kinds.push_back(orb ? std::format("orb {}", all[k].experience) : std::format("item {}", all[k].stack.count));
                    if (orb) highest_orb = std::max(highest_orb, all[k].position.y);
                    else lowest_item = std::min(lowest_item, all[k].position.y);
                }
                require(kinds == std::vector<std::string>{"item 16", "orb 17", "orb 3", "orb 1"}, std::format("{}: a creative player's drops", version));
                require(highest_orb < lowest_item - 0.5, std::format("{}: the orbs at the feet ({} against {})", version, highest_orb, lowest_item));
            }
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            f.game->tick();
            seen_now = packets();
            // (she respawns where she died, and takes an orb at once)
            require(!std::ranges::search(seen_now, respawned).empty(),
                    std::format("{}: the creative player's experience gone: {}", version, [&] { std::string t; for (const auto& e : seen_now) t += e + ", "; return t; }()));
            f.game->submit(1, I{server::intent::ConfirmTeleport{5}});
            f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
            for (int k = 0; k < 6; ++k) f.game->tick(); // the rest of her orbs taken
            // With keepInventory (20 blocks the other way): nothing drops at
            // the death, the levels stay and are told at the respawn; the
            // stone carried is thrown when the body goes, here at the respawn
            // before it (client/ench_curses).
            f.game->submit(1, I{server::intent::Command{"tp alice -19.5 -60 0.5"}});
            f.game->tick();
            f.game->submit(1, I{server::intent::ConfirmTeleport{6}});
            f.game->submit(1, I{server::intent::Command{"gamerule keepInventory true"}});
            f.game->submit(1, I{server::intent::Command{"gamemode creative"}});
            f.game->submit(1, I{server::intent::CreativeSlot{36, 16, stone}});
            f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
            f.game->submit(1, I{server::intent::Command{"xp set alice 0 levels"}});
            f.game->submit(1, I{server::intent::Command{"xp set alice 0 points"}});
            f.game->tick();
            f.game->submit(1, I{server::intent::Command{"xp add alice 3 levels"}});
            f.game->tick();
            f.game->submit(1, window_click(0, 36, 0, 0));
            f.game->tick();
            {
                const auto d = f.game->engine().dump();
                const auto p = std::ranges::find_if(d.players, [&](const auto& inv) { return inv.player == key; });
                require(p != d.players.end() && p->cursor == contracts::ItemStack{static_cast<std::uint16_t>(stone), 16, 0} && p->slots[36].empty(),
                        "the stone carried");
            }
            const auto before_keep = entities().size();
            (void)packets();
            f.game->submit(1, I{server::intent::Command{"kill"}});
            for (int k = 0; k < 3; ++k) f.game->tick();
            {
                const auto d = f.game->engine().dump();
                const auto p = std::ranges::find_if(d.players, [&](const auto& inv) { return inv.player == key; });
                require(entities().size() == before_keep && p != d.players.end()
                            && p->cursor == contracts::ItemStack{static_cast<std::uint16_t>(stone), 16, 0} && p->xp_level == 3,
                        std::format("{}: keepInventory keeps the levels, the stone still carried", version));
            }
            require(std::ranges::none_of(packets(), [](const auto& s) { return s.starts_with("experience"); }), "nothing told while dead");
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            f.game->tick();
            seen_now = packets();
            {
                // The carried stack thrown from where the body was at the
                // respawn (client/ench_curses: when the body goes, or then).
                const auto d = f.game->engine().dump();
                const auto p = std::ranges::find_if(d.players, [&](const auto& inv) { return inv.player == key; });
                require(entities().size() == before_keep + 1 && (p == d.players.end() || p->cursor.empty()),
                        std::format("{}: the carried stone thrown at the respawn", version));
            }
            // (the total keeps the 21 points she took from her orbs: /xp set leaves it)
            require(seen_now == std::vector<std::string>{"respawn", "position", "spawn", "experience 0 3 21", "spawn", "health 20", "experience 0 3 21"},
                    std::format("{}: keepInventory, the respawn: {}", version, [&] { std::string t; for (const auto& e : seen_now) t += e + ", "; return t; }()));
            (void)bob;
        }
    });

    suite.run("players' entity data, operator level and where they appear as both official servers show them", [] {
        // circuit-clienttest/player-data.js, both versions alike.
        using I = server::Intent;
        std::map<std::uint64_t, std::size_t> seen;
        // Since the last call, as text: "login <entity>", "recipes",
        // "status <entity> <event>", "commands", "abilities", "respawn",
        // "position <x> <y> <z>", "data <entity> <index>=<value> ...",
        // "health <health>", "attributes <entity>", "spawn <entity>".
        const auto packets = [&](GameFixture& f, Recorder& r) {
            const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
            std::vector<std::string> out;
            std::lock_guard lock(r.mutex);
            for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                auto pk = protocol::open_packet(r.packets[at]);
                if (!pk) continue;
                auto& in = pk->fields;
                if (pk->id == id("login")) out.push_back(std::format("login {}", in.i32().value()));
                else if (pk->id == id("update_recipes")) out.emplace_back("recipes");
                else if (pk->id == id("commands")) out.emplace_back("commands");
                else if (pk->id == id("player_abilities")) out.emplace_back("abilities");
                else if (pk->id == id("respawn")) out.emplace_back("respawn");
                else if (pk->id == id("entity_event")) {
                    const auto e = in.i32().value();
                    out.push_back(std::format("status {} {}", e, static_cast<int>(in.i8().value())));
                } else if (pk->id == id("player_position")) {
                    (void)in.varint();
                    const auto x = in.f64().value(), y = in.f64().value();
                    out.push_back(std::format("position {} {} {}", x, y, in.f64().value()));
                } else if (pk->id == id("set_entity_data")) {
                    std::string text = std::format("data {}", in.varint().value());
                    for (auto index = in.u8().value(); index != 0xFF; index = in.u8().value()) {
                        const auto type = in.varint().value();
                        if (type == 0) text += std::format(" {}={}", index, static_cast<int>(in.u8().value()));
                        else if (type == 3) text += std::format(" {}={}", index, in.f32().value());
                        else break; // the effects' entries are not used here
                    }
                    out.push_back(std::move(text));
                } else if (pk->id == id("set_health")) {
                    out.push_back(std::format("health {}", in.f32().value()));
                } else if (pk->id == id("update_attributes")) {
                    out.push_back(std::format("attributes {}", in.varint().value()));
                } else if (pk->id == id("add_entity")) {
                    out.push_back(std::format("spawn {}", in.varint().value()));
                } else if (pk->id == id("remove_entities")) {
                    for (auto n = in.varint().value(); n > 0; --n) out.push_back(std::format("removed {}", in.varint().value()));
                }
            }
            return out;
        };
        const auto index = [](const std::vector<std::string>& v, const std::string& s) { return std::ranges::find(v, s) - v.begin(); };
        {
            GameFixture f;
            auto alice = f.join(1, "alice");
            auto a = packets(f, *alice);
            const auto me = a.at(0).substr(6);
            // Joining: the operator level after the recipes and before the commands; the first data before the health, the attributes after.
            const auto at_status = index(a, std::format("status {} 28", me));
            require(a[0].starts_with("login ") && index(a, "recipes") < at_status && at_status + 1 == index(a, "commands"), "the operator level");
            const auto at_data = index(a, std::format("data {} 9=20", me));
            require(at_data < static_cast<std::ptrdiff_t>(a.size()) && index(a, "position 0.5 -60 0.5") < at_data && at_data < index(a, "health 20") &&
                        index(a, "health 20") < index(a, std::format("attributes {}", me)),
                    "the position, the first data, the health, the attributes");
            // Bob joins with his skin parts: Alice sees him appear with his data, then his first data again.
            const auto bob = std::make_shared<Recorder>(2);
            f.game->join({bob, {2, 2}, "bob", {}, 3, false, 127, 1});
            f.game->tick();
            a = packets(f, *alice);
            const auto spawn = std::ranges::find_if(a, [](const auto& t) { return t.starts_with("spawn "); });
            require(spawn != a.end(), "bob appears");
            const auto him = spawn->substr(6);
            // A new player's attributes come after both (client/attribute_join).
            const auto first_data = index(a, std::format("data {} 9=20 17=127", him));
            const auto second_data = std::find(a.begin() + first_data + 1, a.end(), std::format("data {} 9=20 17=127", him)) - a.begin();
            require(index(a, *spawn) < first_data && first_data < second_data && second_data < index(a, std::format("attributes {}", him)) &&
                        index(a, std::format("attributes {}", him)) < static_cast<std::ptrdiff_t>(a.size()),
                    "his appearance, his data twice, then his attributes");
            (void)packets(f, *bob);
            // The client's settings: the skin parts and the main hand, to her and to those who see her.
            f.game->submit(1, I{server::intent::ClientInformation{61, 0}});
            f.game->tick();
            require(packets(f, *alice) == std::vector<std::string>{std::format("data {} 17=61 18=0", me)}, "her settings");
            // Hurt: the health, then the data at the next entity update.
            f.game->submit(1, I{server::intent::Command{"gamemode survival"}});
            f.game->tick();
            (void)packets(f, *alice);
            f.game->submit(1, I{server::intent::Command{"effect give alice minecraft:instant_damage 1 0"}});
            f.game->tick();
            f.game->tick();
            a = packets(f, *alice);
            require(index(a, "health 14") < index(a, std::format("data {} 9=14", me)) && index(a, std::format("data {} 9=14", me)) < static_cast<std::ptrdiff_t>(a.size()),
                    "the health, then its data");
            // A respawn: the abilities, the respawn, the position, the operator level, the commands, the first data, the attributes, the health.
            f.game->submit(1, I{server::intent::Command{"kill"}});
            for (int k = 0; k < 3; ++k) f.game->tick();
            (void)packets(f, *alice);
            (void)packets(f, *bob);
            f.game->submit(1, I{server::intent::ClientCommand{0}});
            f.game->tick();
            a = packets(f, *alice);
            // An immediate respawn: her client drops bob before the respawn and sees him again after; he sees her body go and her come back.
            require(index(a, std::format("removed {}", him)) < index(a, "respawn") && index(a, "respawn") < index(a, std::format("spawn {}", him)) &&
                        index(a, std::format("spawn {}", him)) < static_cast<std::ptrdiff_t>(a.size()),
                    "she drops bob, respawns, sees him again");
            const auto b = packets(f, *bob);
            require(index(b, std::format("removed {}", me)) < index(b, std::format("spawn {}", me)) && index(b, std::format("spawn {}", me)) < static_cast<std::ptrdiff_t>(b.size()),
                    "bob sees her body go and her come back");
            std::vector<std::string> order;
            for (const auto& t : a)
                if (t == "abilities" || t == "respawn" || t.starts_with("position") || t == std::format("status {} 28", me) || t == "commands" ||
                    t.starts_with(std::format("data {} ", me)) || t == std::format("attributes {}", me) || t.starts_with("health"))
                    order.push_back(t.starts_with("position") ? "position" : t);
            require(order == std::vector<std::string>{"abilities", "respawn", "position", std::format("status {} 28", me), "commands",
                                                      std::format("data {} 9=20 17=61 18=0", me), std::format("attributes {}", me), "health 20"},
                    "the respawn's order");
        }
        {
            // With spawnRadius 10: four new players within 10 blocks of the spawn's center, on the floor, not all at the center.
            auto config = offline_config();
            config.initial_game_rules["spawnRadius"] = 10;
            GameFixture f(config);
            seen.clear(); // the peers' ids again
            int centered = 0;
            for (std::uint64_t k = 1; k <= 4; ++k) {
                auto peer = f.join(k, std::format("p{}", k));
                const auto a = packets(f, *peer);
                const auto position = std::ranges::find_if(a, [](const auto& t) { return t.starts_with("position "); });
                require(position != a.end(), "a position");
                double x = 0, y = 0, z = 0;
                std::sscanf(position->c_str(), "position %lf %lf %lf", &x, &y, &z);
                require(std::abs(x - 0.5) <= 10.0 && std::abs(z - 0.5) <= 10.0 && y == -60.0 && x - std::floor(x) == 0.5 && z - std::floor(z) == 0.5,
                        "within 10 blocks, on the floor, at a block's center: " + *position);
                centered += x == 0.5 && z == 0.5 ? 1 : 0;
            }
            require(centered < 4, "not all at the center");
        }
    });

    suite.run("players wear armor and hold items as both official servers show them", [] {
        // circuit-clienttest/armor.js: alice in survival, bob in creative beside
        // her; both versions alike but for the attributes' ids.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            GameFixture f(version, offline_config());
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::map<std::int32_t, std::string> names;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = name;
            }
            // Since the last call, as text; attributes by name with their modifiers.
            std::map<std::uint64_t, std::size_t> seen;
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                const auto stack = [&](protocol::Reader& in) {
                    const auto count = in.varint().value();
                    if (count == 0) return std::string("empty");
                    const auto item = tables.item_name(in.varint().value()).value_or("?").substr(10);
                    (void)in.varint(), (void)in.varint(); // no components
                    return std::format("{}*{}", item, count);
                };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("sound")) {
                        const auto sound = in.varint().value() - 1, category = in.varint().value();
                        (void)in.i32(), (void)in.i32(), (void)in.i32();
                        out.push_back(std::format("sound {} category {} volume {}", sound, category, in.f32().value()));
                    } else if (pk->id == id("set_equipment")) {
                        std::string line = std::format("equipment {}", who(in.varint().value()));
                        for (bool more = true; more;) {
                            const auto slot = in.u8().value();
                            more = (slot & 0x80) != 0;
                            line += std::format(" {}:{}", slot & 0x7F, stack(in));
                        }
                        out.push_back(line);
                    } else if (pk->id == id("update_attributes")) {
                        std::string line = std::format("attributes {}", who(in.varint().value()));
                        for (auto n = in.varint().value(); n > 0; --n) {
                            const auto attribute = in.varint().value();
                            line += std::format(" [{} {}", attribute, in.f64().value());
                            for (auto m = in.varint().value(); m > 0; --m) {
                                const auto modifier = in.string().value();
                                const auto amount = in.f64().value();
                                (void)in.u8();
                                line += std::format(" {{{} {}}}", modifier.substr(10), amount);
                            }
                            line += "]";
                        }
                        out.push_back(line);
                    } else if (pk->id == id("entity_event")) {
                        const auto e = in.i32().value();
                        const auto event = static_cast<int>(in.i8().value());
                        if (event < 24 || event > 28) out.push_back(std::format("status {} {}", who(e), event));
                    } else if (pk->id == id("animate")) {
                        const auto e = in.varint().value();
                        out.push_back(std::format("animation {} {}", who(e), static_cast<int>(in.u8().value())));
                    } else if (pk->id == id("container_set_content")) {
                        if (in.varint().value() == 0) out.emplace_back("whole window");
                    } else if (pk->id == id("add_entity")) {
                        out.push_back(std::format("spawned {}", who(in.varint().value())));
                    }
                }
                return out;
            };
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            std::int32_t sequence = 0;
            const auto use = [&](std::int32_t hand) { f.game->submit(1, I{server::intent::Use{hand, ++sequence, 0.0f, 0.0f}}); };
            const auto hold = [&](std::int16_t slot) { f.game->submit(1, I{server::intent::HeldSlot{slot}}); };
            const auto put = [&](std::int16_t slot, std::string_view name, std::int32_t count = 1) {
                f.game->submit(1, I{server::intent::CreativeSlot{slot, count, tables.item_id(name).value(), 0}});
            };
            const bool v772 = std::string_view(version) == "1.21.8";
            const auto armor = std::format("{}", 0), toughness = std::format("{}", 1), speed = std::format("{}", 4);
            (void)v772;
            command(1, "tp 8.5 -60 8.5");
            command(2, "tp 8.5 -60 11.5");
            put(36, "minecraft:iron_helmet");
            put(37, "minecraft:diamond_chestplate");
            put(38, "minecraft:iron_sword");
            put(39, "minecraft:leather_helmet");
            put(40, "minecraft:stone", 3);
            put(41, "minecraft:carved_pumpkin");
            ticks(2);
            command(1, "gamemode survival");
            ticks(3);
            (void)packets(*alice);
            (void)packets(*bob);
            // The iron helmet by use: its sound, the whole window, then (at the
            // next tick's start) the attributes; bob gets the slots that changed.
            use(0);
            ticks(1);
            auto a = packets(*alice);
            auto b = packets(*bob);
            const auto iron = "sound 72 category 7 volume 1";
            require(a == std::vector<std::string>{iron, "whole window"}, std::format("{}: the helmet put on", version));
            require(b == std::vector<std::string>{iron, "equipment alice 0:empty 5:iron_helmet*1"}, std::format("{}: what bob gets", version));
            ticks(1);
            const auto helmet = std::format("attributes alice [{} 0 {{armor.helmet 2}}] [{} 0 {{armor.helmet 0}}]", armor, toughness);
            a = packets(*alice);
            b = packets(*bob);
            require(a == std::vector<std::string>{helmet} && b == std::vector<std::string>{helmet},
                    std::format("{}: armor 2, toughness 0: {} / {}", version, a.empty() ? "" : a[0], b.empty() ? "" : b[0]));
            // The diamond chestplate, from another hotbar slot: the hand's change, then the use.
            hold(1);
            ticks(1);
            require(packets(*bob) == std::vector<std::string>{"equipment alice 0:diamond_chestplate*1"}, std::format("{}: held", version));
            use(0);
            ticks(2);
            (void)packets(*alice);
            b = packets(*bob);
            require(b.size() == 3 && b[0] == "sound 68 category 7 volume 1" && b[1] == "equipment alice 0:empty 4:diamond_chestplate*1"
                        && b[2] == std::format("attributes alice [{} 0 {{armor.helmet 2}} {{armor.chestplate 8}}] [{} 0 {{armor.helmet 0}} {{armor.chestplate 2}}]",
                                               armor, toughness),
                    std::format("{}: the chestplate", version));
            // A sword held: its attack speed (its attack damage is not sent).
            hold(2);
            ticks(2);
            const auto sword = std::format("attributes alice [{} 4 {{base_attack_speed {}}}]", speed, static_cast<double>(-2.4f));
            require(packets(*alice) == std::vector<std::string>{sword}, std::format("{}: the sword's attack speed", version));
            require(packets(*bob) == std::vector<std::string>{"equipment alice 0:iron_sword*1", sword}, std::format("{}: and bob's", version));
            // Swung: bob sees it, alice does not.
            f.game->submit(1, I{server::intent::Swing{0}});
            f.game->submit(1, I{server::intent::Swing{1}});
            ticks(1);
            require(packets(*alice).empty() && packets(*bob) == std::vector<std::string>{"animation alice 0", "animation alice 3"},
                    std::format("{}: the swings", version));
            // The swap key: event 55 for bob, not the slots; the sword's speed gone.
            f.game->submit(1, I{server::intent::Dig{6, {0, 0, 0}, 0, 0}});
            ticks(2);
            b = packets(*bob);
            require(b == std::vector<std::string>{"status alice 55", std::format("attributes alice [{} 4]", speed)}, std::format("{}: hands swapped", version));
            // The leather helmet by use, for the iron one: the iron helmet to the hand, the new modifier last.
            hold(3);
            ticks(1);
            (void)packets(*bob);
            use(0);
            ticks(2);
            b = packets(*bob);
            require(b.size() == 3 && b[0] == "sound 73 category 7 volume 1" && b[1] == "equipment alice 0:iron_helmet*1 5:leather_helmet*1"
                        && b[2] == std::format("attributes alice [{} 0 {{armor.chestplate 8}} {{armor.helmet 1}}] [{} 0 {{armor.chestplate 2}} {{armor.helmet 0}}]",
                                               armor, toughness),
                    std::format("{}: swapped: {}", version, b.size()));
            // A carved pumpkin is not put on by use.
            hold(5);
            ticks(1);
            (void)packets(*bob);
            use(0);
            ticks(2);
            require(packets(*bob).empty(), std::format("{}: the pumpkin stays in the hand", version));
            // One of a held stack thrown: its count for bob.
            hold(4);
            ticks(1);
            (void)packets(*bob);
            f.game->submit(1, I{server::intent::Dig{4, {0, 0, 0}, 0, 0}});
            ticks(1);
            b = packets(*bob);
            require(std::ranges::count(b, std::string("equipment alice 0:stone*2")) == 1, std::format("{}: one thrown", version));
            // Bob far away and back: alice appears with her attributes, then all she holds and wears.
            command(2, "tp 300 -60 300");
            ticks(5);
            command(2, "tp 8.5 -60 11.5");
            ticks(5);
            b = packets(*bob);
            const auto spawned = std::ranges::find(b, std::string("spawned alice"));
            const auto worn = std::ranges::find(b, std::string("equipment alice 0:stone*2 1:iron_sword*1 4:diamond_chestplate*1 5:leather_helmet*1"));
            const auto attributes = std::ranges::find_if(b, [](const std::string& l) { return l.starts_with("attributes alice"); });
            require(spawned != b.end() && attributes != b.end() && worn != b.end() && spawned < attributes && attributes < worn,
                    std::format("{}: alice appears", version));
        }
    });

    suite.run("dispensers and clicks put armor on as both official servers show it", [] {
        // circuit-clienttest/armor2.js and armor.js's clicks: alice in survival
        // before a dispenser facing her, bob in creative beside her.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            GameFixture f(version, offline_config());
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::map<std::int32_t, std::string> names;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = name;
            }
            std::int32_t window = -1;
            std::map<std::uint64_t, std::size_t> seen;
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::string("an entity"); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("sound")) {
                        const auto sound = in.varint().value() - 1, category = in.varint().value();
                        out.push_back(std::format("sound {} category {}", sound, category));
                    } else if (pk->id == id("level_event")) {
                        const auto event = in.i32().value();
                        (void)in.position();
                        out.push_back(std::format("level event {} data {}", event, in.i32().value()));
                    } else if (pk->id == id("set_equipment")) {
                        std::string line = std::format("equipment {}", who(in.varint().value()));
                        for (bool more = true; more;) {
                            const auto slot = in.u8().value();
                            more = (slot & 0x80) != 0;
                            const auto count = in.varint().value();
                            if (count == 0) {
                                line += std::format(" {}:empty", slot & 0x7F);
                            } else {
                                line += std::format(" {}:{}", slot & 0x7F, tables.item_name(in.varint().value()).value_or("?").substr(10));
                                (void)in.varint(), (void)in.varint();
                            }
                        }
                        out.push_back(line);
                    } else if (pk->id == id("add_entity")) {
                        out.push_back(std::format("spawned {}", who(in.varint().value())));
                    } else if (pk->id == id("open_screen")) {
                        window = in.varint().value();
                    }
                }
                return out;
            };
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            const auto put = [&](std::int16_t slot, std::string_view name) {
                f.game->submit(1, I{server::intent::CreativeSlot{slot, 1, tables.item_id(name).value(), 0}});
            };
            const auto click = [&](std::int32_t in, std::int16_t slot, std::int32_t mode) {
                f.game->submit(1, I{server::intent::ContainerClick{in, 0, slot, 0, mode, {}, std::nullopt}});
                ticks(1);
            };
            command(1, "tp 8.5 -60 8.5");
            command(2, "tp 8.5 -60 11.5");
            command(1, "setblock 8 -60 7 minecraft:dispenser[facing=south,triggered=false]");
            put(36, "minecraft:iron_helmet");
            put(37, "minecraft:iron_helmet");
            put(38, "minecraft:diamond_boots");
            ticks(3);
            (void)packets(*alice);
            // Two helmets into the dispenser through its window (in creative mode).
            f.game->submit(1, I{server::intent::UseOn{0, {8, -60, 7}, 1, 0.5f, 1.0f, 0.5f, false, 1}});
            ticks(1);
            (void)packets(*alice);
            require(window > 0, std::format("{}: the dispenser's window", version));
            click(window, 36, 1);
            click(window, 37, 1);
            f.game->submit(1, I{server::intent::ContainerClose{window}});
            command(1, "gamemode survival");
            ticks(3);
            (void)packets(*alice);
            (void)packets(*bob);
            const auto power = [&](bool on) {
                command(1, on ? "setblock 9 -60 7 minecraft:redstone_block" : "setblock 9 -60 7 minecraft:air");
                ticks(on ? 6 : 2);
            };
            // The helmet goes on: its sound, then the dispenser's sound and smoke (facing south: 3), then bob's equipment.
            power(true);
            auto a = packets(*alice);
            auto b = packets(*bob);
            const std::vector<std::string> put_on{"sound 72 category 7", "level event 1000 data 0", "level event 2000 data 3"};
            require(a.size() >= 3 && std::vector(a.begin(), a.begin() + 3) == put_on, std::format("{}: the helmet goes on", version));
            require(b.size() >= 4 && std::vector(b.begin(), b.begin() + 3) == put_on && b[3] == "equipment alice 5:iron_helmet",
                    std::format("{}: what bob gets", version));
            power(false);
            // The second, with a helmet worn: ejected, the item before the dispenser's sound and smoke.
            (void)packets(*bob);
            power(true);
            b = packets(*bob);
            require(b.size() >= 3 && b[0] == "spawned an entity" && b[1] == "level event 1000 data 0" && b[2] == "level event 2000 data 3",
                    std::format("{}: ejected", version));
            power(false);
            // Boots put on by clicks: their sound and the equipment; taken off: no sound.
            ticks(60);
            (void)packets(*alice);
            (void)packets(*bob);
            click(0, 38, 0);
            click(0, 8, 0);
            b = packets(*bob);
            require(b == std::vector<std::string>{"sound 68 category 7", "equipment alice 2:diamond_boots"}, std::format("{}: boots on", version));
            click(0, 8, 0);
            click(0, 20, 0);
            b = packets(*bob);
            require(b == std::vector<std::string>{"equipment alice 2:empty"}, std::format("{}: boots off, without a sound", version));
        }
    });

    suite.run("players eat and drink as both official servers show it", [] {
        // circuit-clienttest/eat.js: alice in survival, bob in creative beside
        // her; both versions alike but for the ids.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            const bool v772 = std::string_view(version) == "1.21.8";
            GameFixture f(version, offline_config());
            const auto& tables = *protocol::ContentTables::for_protocol(f.wire.protocol());
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::map<std::int32_t, std::string> names;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = name;
            }
            const auto eat = v772 ? 614 : 601, drink = v772 ? 613 : 600, burp = v772 ? 1173 : 1133, bad_omen = v772 ? 1724 : 1648;
            // Since the last call, as text; the sounds a use makes while it
            // lasts (category player) by name alone, their volume drawn at random.
            std::map<std::uint64_t, std::size_t> seen;
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("set_health")) {
                        const auto health = in.f32().value();
                        const auto food = in.varint().value();
                        out.push_back(std::format("health {} food {} saturation {}", health, food, in.f32().value()));
                    } else if (pk->id == id("sound")) {
                        const auto sound = in.varint().value() - 1, category = in.varint().value();
                        (void)in.i32(), (void)in.i32(), (void)in.i32();
                        if (category == 7 && (sound == eat || sound == drink)) out.push_back(std::format("use sound {}", sound));
                        else out.push_back(std::format("sound {} category {} volume {}", sound, category, in.f32().value()));
                    } else if (pk->id == id("entity_event")) {
                        const auto e = in.i32().value();
                        out.push_back(std::format("status {} {}", who(e), static_cast<int>(in.i8().value())));
                    } else if (pk->id == id("set_entity_data")) {
                        const auto e = in.varint().value();
                        for (auto index = in.u8().value(); index != 0xFF; index = in.u8().value()) {
                            const auto type = in.varint().value();
                            if (type == 0) {
                                const auto v = in.u8().value();
                                if (index == 8) out.push_back(std::format("using {} {}", who(e), static_cast<int>(v)));
                            } else if (type == 3) (void)in.f32();
                            else break;
                        }
                    } else if (pk->id == id("update_mob_effect")) {
                        (void)in.varint();
                        const auto effect = in.varint().value(), amplifier = in.varint().value(), duration = in.varint().value();
                        out.push_back(std::format("effect {} amp {} dur {} flags {}", effect, amplifier, duration, static_cast<int>(in.u8().value())));
                    } else if (pk->id == id("remove_mob_effect")) {
                        (void)in.varint();
                        out.push_back(std::format("removed {}", in.varint().value()));
                    } else if (pk->id == id("add_entity")) {
                        out.push_back(std::format("spawned {}", who(in.varint().value())));
                    } else if (pk->id == id("container_set_slot")) {
                        (void)in.varint(), (void)in.varint();
                        const auto slot = in.i16().value();
                        const auto count = in.varint().value();
                        out.push_back(count == 0 ? std::format("slot {} empty", slot)
                                                 : std::format("slot {} {}*{}", slot, tables.item_name(in.varint().value()).value_or("?").substr(10), count));
                    } else if (pk->id == id("cooldown")) {
                        const auto group = in.string().value();
                        out.push_back(std::format("cooldown {} {}", group, in.varint().value()));
                    } else if (pk->id == id("player_position")) {
                        if (out.empty() || out.back() != "teleported") out.emplace_back("teleported"); // a chorus fruit's tries: one line
                    }
                }
                return out;
            };
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            // Whether `lines` hold `wanted` in that order, others between them.
            const auto in_order = [](const std::vector<std::string>& lines, const std::vector<std::string>& wanted) {
                std::size_t k = 0;
                for (const auto& l : lines)
                    if (k < wanted.size() && l == wanted[k]) ++k;
                return k == wanted.size();
            };
            const auto join = [](const std::vector<std::string>& lines) {
                std::string out;
                for (const auto& l : lines) out += (out.empty() ? "" : " | ") + l;
                return out;
            };
            std::int32_t sequence = 0;
            const auto use = [&](std::int32_t hand) { f.game->submit(1, I{server::intent::Use{hand, ++sequence, 0.0f, 0.0f}}); };
            const auto hold = [&](std::int16_t slot) { f.game->submit(1, I{server::intent::HeldSlot{slot}}); };
            const auto put = [&](std::int16_t slot, std::string_view name, std::int32_t count, contracts::ComponentPatch c = {}) {
                f.game->submit(1, I{server::intent::CreativeSlot{slot, count, tables.item_id(name).value(), 0, std::move(c)}});
            };
            command(1, "gamerule naturalRegeneration false");
            command(1, "tp 8.5 -60 8.5");
            command(2, "tp 8.5 -60 11.5");
            put(36, "minecraft:cooked_beef", 2);
            put(37, "minecraft:milk_bucket", 1);
            put(38, "minecraft:potion", 1, f.wire.components().with_potion({}, 31, f.wire.component_context()).value());
            put(39, "minecraft:bread", 2);
            put(45, "minecraft:apple", 1);
            ticks(2);
            command(1, "gamemode survival");
            ticks(3);
            (void)packets(*alice);
            (void)packets(*bob);
            // At full hunger, beef is refused.
            use(0);
            ticks(40);
            require(packets(*alice).empty() && packets(*bob).empty(), std::format("{}: at full hunger nothing", version));
            // Made hungry: the food as the last health packet tells it.
            command(1, "effect give alice minecraft:hunger 3 255");
            ticks(62);
            command(1, "effect clear alice");
            ticks(1);
            auto a = packets(*alice);
            const auto told = [](const std::vector<std::string>& lines) {
                std::int32_t food = -1;
                for (const auto& l : lines)
                    if (l.starts_with("health 20 food ")) food = std::stoi(l.substr(15));
                return food;
            };
            const auto hungry = told(a);
            require(hungry > 0 && hungry < 12, std::format("{}: made hungry: {}", version, hungry));
            (void)packets(*bob);
            // Beef: the living flags at once; bob hears it every fourth tick
            // after the first fifth; at the 32nd tick the end.
            use(0);
            ticks(1);
            require(packets(*alice) == std::vector<std::string>{"using alice 1"} && packets(*bob) == std::vector<std::string>{"using alice 1"},
                    std::format("{}: the use starts", version));
            ticks(30);
            auto b = packets(*bob);
            require(packets(*alice).empty() && b == std::vector<std::string>(6, std::format("use sound {}", eat)), std::format("{}: six sounds so far", version));
            ticks(1);
            a = packets(*alice);
            b = packets(*bob);
            const auto food = hungry + 8;
            require(a == std::vector<std::string>{"status alice 9", std::format("sound {} category 6 volume 1", eat), std::format("sound {} category 7 volume 0.5", burp),
                                                  std::format("health 20 food {} saturation 12.8", food), "using alice 0", "slot 36 cooked_beef*1"},
                    std::format("{}: eaten: {}", version, join(a)));
            require(b == std::vector<std::string>{std::format("use sound {}", eat), std::format("sound {} category 6 volume 1", eat),
                                                  std::format("sound {} category 7 volume 0.5", burp), "using alice 0"},
                    std::format("{}: what bob hears", version));
            // Let go after 10 ticks: one sound, nothing eaten.
            use(0);
            ticks(10);
            f.game->submit(1, I{server::intent::Dig{5, {}, 0, 0}});
            ticks(2);
            require(packets(*alice) == std::vector<std::string>{"using alice 1", "using alice 0"}, std::format("{}: let go", version));
            require(packets(*bob) == std::vector<std::string>{"using alice 1", std::format("use sound {}", eat), "using alice 0"},
                    std::format("{}: one sound for bob", version));
            // Another hotbar slot ends it.
            hold(3);
            use(0);
            ticks(5);
            hold(0);
            ticks(1);
            require(packets(*alice) == std::vector<std::string>{"using alice 1", "using alice 0"}, std::format("{}: switched away", version));
            // The off hand: its flag, which stays after the use; the apple is eaten from it.
            use(1);
            ticks(1);
            require(packets(*bob) == std::vector<std::string>{"using alice 1", "using alice 0", "using alice 3"}, std::format("{}: the off hand", version));
            ticks(31);
            a = packets(*alice);
            require(a.size() == 7 && a[0] == "using alice 3" && a[1] == "status alice 9" && a[4].starts_with("health 20 food 18 ") && a[5] == "using alice 2"
                        && a[6] == "slot 45 empty",
                    std::format("{}: the apple eaten: {}", version, join(a)));
            // Milk: no sounds at the end, every effect gone, the bucket left.
            command(1, "effect give alice minecraft:speed 30 0");
            hold(1);
            ticks(1);
            (void)packets(*alice);
            (void)packets(*bob);
            use(0);
            ticks(32);
            a = packets(*alice);
            b = packets(*bob);
            require(a == std::vector<std::string>{"using alice 1", "status alice 9", "removed 0", "using alice 0", "slot 37 bucket*1"},
                    std::format("{}: milk: {}", version, join(a)));
            require(std::ranges::count(b, std::format("use sound {}", drink)) == 7 && b.back() == "using alice 0", std::format("{}: bob hears it drunk", version));
            // A potion: its effect, the bottle left.
            hold(2);
            use(0);
            ticks(32);
            a = packets(*alice);
            require(a == std::vector<std::string>{"using alice 1", "status alice 9", "effect 9 amp 0 dur 900 flags 14", "using alice 0", "slot 38 glass_bottle*1"},
                    std::format("{}: the potion: {}", version, join(a)));
            // In creative mode: eaten (its food given), the stack kept.
            command(1, "gamemode creative");
            hold(0);
            ticks(1);
            (void)packets(*alice);
            use(0);
            ticks(32);
            a = packets(*alice);
            require(a == std::vector<std::string>{"using alice 1", "status alice 9", std::format("sound {} category 6 volume 1", eat),
                                                  std::format("sound {} category 7 volume 0.5", burp), "health 20 food 20 saturation 20", "using alice 0"},
                    std::format("{}: creative: {}", version, join(a)));
            // Absorption ends with its absorption used up.
            command(1, "gamemode survival");
            command(1, "effect clear alice");
            ticks(1);
            command(1, "effect give alice minecraft:absorption 30 0");
            ticks(2);
            command(1, "effect give alice minecraft:instant_damage 1 0");
            ticks(3);
            a = packets(*alice);
            require(std::ranges::count(a, std::string("removed 21")) == 1, std::format("{}: absorption used up", version));
            // The bad omen is heard when it comes, by everyone nearby.
            (void)packets(*bob);
            command(1, "effect give alice minecraft:bad_omen 30 0");
            ticks(1);
            const auto omen = std::format("sound {} category 7 volume 1", bad_omen);
            require(std::ranges::count(packets(*alice), omen) == 1 && packets(*bob) == std::vector<std::string>{omen}, std::format("{}: bad omen", version));
            // A chorus fruit at full hunger, from the off hand: eaten, then the
            // teleport (to a place drawn at random), its event and sound,
            // and a second's cooldown, in which another is refused.
            command(1, "gamemode creative");
            ticks(1);
            put(45, "minecraft:chorus_fruit", 2);
            ticks(1);
            command(1, "gamemode survival");
            ticks(1);
            (void)packets(*alice);
            (void)packets(*bob);
            const auto chorus = v772 ? 323 : 321;
            use(1);
            ticks(32);
            use(1);
            ticks(2);
            a = packets(*alice);
            require(a == std::vector<std::string>{"using alice 3", "status alice 9", std::format("sound {} category 6 volume 1", eat),
                                                  std::format("sound {} category 7 volume 0.5", burp), "teleported", "status alice 46",
                                                  std::format("sound {} category 7 volume 1", chorus), "cooldown minecraft:chorus_fruit 20", "using alice 2",
                                                  "slot 45 chorus_fruit*1"},
                    std::format("{}: chorus fruit: {}", version, join(a)));
            ticks(18);
            a = packets(*alice);
            require(a.size() == 1 && a[0] == "cooldown minecraft:chorus_fruit 0", std::format("{}: the cooldown over: {}", version, join(a)));
            // A second hotbar: a suspicious stew, an ominous bottle, honey, a
            // potion of healing and two golden apples, all eaten at full hunger
            // or drunk.
            command(1, "gamemode creative");
            command(1, "effect clear alice");
            ticks(1);
            protocol::Writer named;
            named.varint(1).varint(15).varint(100); // night vision for 100 ticks
            const auto stew_type = *f.wire.components().type_id("suspicious_stew_effects");
            put(36, "minecraft:suspicious_stew", 1,
                contracts::ComponentPatch::make(f.wire.protocol(), {{stew_type, std::vector<std::byte>(named.bytes().begin(), named.bytes().end())}}, {}, {}));
            put(37, "minecraft:ominous_bottle", 1);
            put(38, "minecraft:honey_bottle", 1);
            put(39, "minecraft:potion", 1, f.wire.components().with_potion({}, 24, f.wire.component_context()).value());
            put(40, "minecraft:golden_apple", 2);
            ticks(1);
            command(1, "gamemode survival");
            hold(0);
            ticks(1);
            (void)packets(*alice);
            (void)packets(*bob);
            const auto drink_slot = [&](std::int16_t slot, int n) {
                hold(slot);
                use(0);
                ticks(n);
                return packets(*alice);
            };
            // The stew's effects come before the food's sounds.
            a = drink_slot(0, 32);
            require(a == std::vector<std::string>{"using alice 1", "status alice 9", "effect 15 amp 0 dur 100 flags 14", std::format("sound {} category 6 volume 1", eat),
                                                  std::format("sound {} category 7 volume 0.5", burp), "using alice 0", "slot 36 bowl*1"},
                    std::format("{}: the stew: {}", version, join(a)));
            // The ominous bottle: bad omen with its particles hidden, its sound, the bottle's; nothing left.
            a = drink_slot(1, 32);
            require(a == std::vector<std::string>{"using alice 1", "status alice 9", "effect 30 amp 0 dur 120000 flags 12", std::format("sound {} category 7 volume 1", bad_omen),
                                                  std::format("sound {} category 7 volume 1", v772 ? 1075 : 1035), "using alice 0", "slot 37 empty"},
                    std::format("{}: the ominous bottle: {}", version, join(a)));
            // Honey while poisoned: 40 ticks, its sound and a burp, poison ended, the bottle left.
            command(1, "effect give alice minecraft:poison 30 0");
            ticks(1);
            (void)packets(*alice);
            a = drink_slot(2, 40);
            const auto honey = v772 ? 754 : 729;
            require(in_order(a, {"using alice 1", "status alice 9", std::format("sound {} category 6 volume 1", honey), std::format("sound {} category 7 volume 0.5", burp),
                                 "removed 18", "using alice 0", "slot 38 glass_bottle*1"})
                        && std::ranges::count(a, std::string("status alice 9")) == 1,
                    std::format("{}: honey: {}", version, join(a)));
            // A potion of healing heals at once, with no effect sent.
            a = drink_slot(3, 32);
            require(in_order(a, {"using alice 1", "status alice 9", "using alice 0", "slot 39 glass_bottle*1"})
                        && std::ranges::none_of(a, [](const std::string& l) { return l.starts_with("effect "); })
                        && std::ranges::any_of(a, [](const std::string& l) { return l.starts_with("health "); }),
                    std::format("{}: healing: {}", version, join(a)));
            // A golden apple's effects come after the sounds, in its order.
            a = drink_slot(4, 32);
            require(in_order(a, {"status alice 9", std::format("sound {} category 6 volume 1", eat), std::format("sound {} category 7 volume 0.5", burp),
                                 "effect 9 amp 1 dur 100 flags 14", "effect 21 amp 0 dur 2400 flags 14", "using alice 0", "slot 40 golden_apple*1"}),
                    std::format("{}: the golden apple: {}", version, join(a)));
            // A player who appears while eating comes with the living flags.
            command(2, "tp 300 -60 300");
            ticks(5);
            use(0);
            ticks(2);
            (void)packets(*bob);
            command(2, "tp 8.5 -60 11.5");
            ticks(5);
            b = packets(*bob);
            require(in_order(b, {"spawned alice", "using alice 1"}), std::format("{}: alice appears eating: {}", version, join(b)));
            // Death ends the use.
            command(1, "kill");
            ticks(1);
            require(std::ranges::count(packets(*bob), std::string("using alice 0")) == 1, std::format("{}: death ends it", version));
        }
    });

    suite.run("spectators are seen by spectators only, look on, and sneak as both official servers show them", [] {
        // circuit-clienttest/spectators.js, both versions alike.
        using I = server::Intent;
        for (const auto* version : {"1.21.8", "1.21.4"}) {
            const bool v772 = std::string_view(version) == "1.21.8";
            GameFixture f(version, offline_config());
            auto alice = f.join(1, "alice");
            auto bob = f.join(2, "bob");
            std::map<std::int32_t, std::string> names;
            for (const auto& [r, name] : {std::pair{alice, "alice"}, std::pair{bob, "bob"}}) {
                const auto login = f.wire.ids().id(State::play, Bound::clientbound, "login");
                std::lock_guard lock(r->mutex);
                for (const auto& bytes : r->packets)
                    if (auto pk = protocol::open_packet(bytes); pk && pk->id == login) names[pk->fields.i32().value()] = name;
            }
            std::map<std::uint64_t, std::size_t> seen;
            // Since the last call, as text.
            const auto packets = [&](Recorder& r) {
                const auto id = [&](std::string_view name) { return f.wire.ids().id(State::play, Bound::clientbound, name); };
                const auto who = [&](std::int32_t e) { return names.contains(e) ? names[e] : std::format("#{}", e); };
                std::vector<std::string> out;
                std::lock_guard lock(r.mutex);
                for (auto& at = seen[r.peer_id]; at < r.packets.size(); ++at) {
                    auto pk = protocol::open_packet(r.packets[at]);
                    if (!pk) continue;
                    auto& in = pk->fields;
                    if (pk->id == id("player_abilities")) out.push_back(std::format("abilities {}", in.u8().value()));
                    else if (pk->id == id("game_event")) {
                        const auto event = in.u8().value();
                        if (event == 3) out.push_back(std::format("game mode {}", in.f32().value()));
                    } else if (pk->id == id("player_info_update")) {
                        const auto actions = in.u8().value();
                        out.push_back(std::format("list {}", actions));
                    } else if (pk->id == id("remove_entities")) {
                        for (auto n = in.varint().value(); n > 0; --n) out.push_back(std::format("removed {}", who(in.varint().value())));
                    } else if (pk->id == id("add_entity")) {
                        out.push_back(std::format("spawned {}", who(in.varint().value())));
                    } else if (pk->id == id("set_entity_data")) {
                        const auto e = in.varint().value();
                        for (auto index = in.u8().value(); index != 0xFF; index = in.u8().value()) {
                            const auto type = in.varint().value();
                            if (type == 0) {
                                const auto v = in.u8().value();
                                if (index == 0) out.push_back(std::format("flags {} {}", who(e), static_cast<int>(v)));
                            } else if (type == 21) out.push_back(std::format("pose {} {}", who(e), in.varint().value()));
                            else if (type == 3) (void)in.f32();
                            else break;
                        }
                    } else if (pk->id == id("set_camera")) {
                        out.push_back(std::format("camera {}", who(in.varint().value())));
                    } else if (pk->id == id("block_update")) {
                        const auto pos = in.position().value();
                        out.push_back(std::format("block {} {} {}", pos.x, pos.y, pos.z));
                    } else if (pk->id == id("open_screen")) {
                        (void)in.varint();
                        out.push_back(std::format("window {}", in.varint().value()));
                    } else if (pk->id == id("player_position")) {
                        out.emplace_back("position");
                    }
                }
                return out;
            };
            const auto ticks = [&](int n) {
                for (int k = 0; k < n; ++k) f.game->tick();
            };
            const auto command = [&](std::uint64_t who, std::string text) { f.game->submit(who, I{server::intent::Command{std::move(text)}}); };
            command(1, "tp 8.5 -60 8.5");
            command(2, "tp 8.5 -60 12.5");
            ticks(3);
            (void)packets(*alice);
            (void)packets(*bob);
            // Sneaking: the shift key flag and the crouching pose, to the player and those who see them (1.21.4 by the player command).
            const auto sneak = [&](std::uint64_t who, bool on) {
                f.game->submit(who, I{server::intent::Sneak{on}});
                ticks(1);
            };
            sneak(1, true);
            require(packets(*alice) == std::vector<std::string>{"flags alice 2", "pose alice 5"} && packets(*bob) == std::vector<std::string>{"flags alice 2", "pose alice 5"},
                    std::format("{}: alice crouches", version));
            sneak(1, false);
            require(packets(*bob) == std::vector<std::string>{"flags alice 0", "pose alice 0"}, std::format("{}: and stands", version));
            (void)packets(*alice);
            // Bob a spectator: the abilities, the game mode, the list, the abilities; alice no longer sees him; he is invisible.
            command(2, "gamemode spectator");
            ticks(1);
            auto b = packets(*bob);
            require(b.size() >= 5 && b[0] == "abilities 7" && b[1] == "game mode 3" && b[2] == "list 4" && b[3] == "abilities 7" &&
                        std::ranges::count(b, std::string("flags bob 32")) == 1,
                    std::format("{}: bob's view of it", version));
            require(packets(*alice) == std::vector<std::string>{"list 4", "removed bob"}, std::format("{}: alice's", version));
            // Alice a spectator too: she sees him; back in survival, not.
            command(1, "gamemode spectator");
            ticks(1);
            auto a = packets(*alice);
            require(std::ranges::count(a, std::string("spawned bob")) == 1 && std::ranges::count(a, std::string("flags bob 32")) == 1, std::format("{}: spectators see spectators", version));
            command(1, "gamemode survival");
            ticks(1);
            a = packets(*alice);
            require(std::ranges::count(a, std::string("removed bob")) == 1, std::format("{}: not after", version));
            (void)packets(*bob);
            // A spectator's dig is answered once; a chest's window opens without its lid.
            f.game->submit(2, I{server::intent::Dig{0, {8, -61, 12}, 1, 1}});
            f.game->submit(2, I{server::intent::Dig{2, {8, -61, 12}, 1, 2}});
            ticks(1);
            require(std::ranges::count(packets(*bob), std::string("block 8 -61 12")) == 1, std::format("{}: one correction", version));
            command(1, "setblock 9 -60 13 minecraft:chest[facing=north,type=single,waterlogged=false]");
            ticks(2);
            (void)packets(*bob);
            f.game->submit(2, I{server::intent::UseOn{0, {9, -60, 13}, 1, 0.5f, 1.0f, 0.5f, false, 3}});
            ticks(1);
            b = packets(*bob);
            require(!b.empty() && b[0] == "window 2", std::format("{}: the chest's window", version));
            // Attacking alice takes her view; sneaking gives it back.
            f.game->submit(2, I{server::intent::Attack{std::ranges::find_if(names, [](const auto& e) { return e.second == "alice"; })->first}});
            ticks(1);
            require(packets(*bob) == std::vector<std::string>{"camera alice", "position"}, std::format("{}: alice's view", version));
            sneak(2, true);
            b = packets(*bob);
            require(b.size() == 3 && b[0] == "flags bob 34" && b[1] == "camera bob" && b[2] == "position", std::format("{}: back to his", version));
            sneak(2, false);
            (void)packets(*bob);
            // The chest's lid did not rise for the spectator (no block event for the other player).
            {
                const auto event = f.wire.ids().id(State::play, Bound::clientbound, "block_event");
                std::lock_guard lock(alice->mutex);
                require(std::ranges::none_of(alice->packets, [&](const auto& bytes) {
                            const auto pk = protocol::open_packet(bytes);
                            return pk && pk->id == event;
                        }),
                        std::format("{}: no lid", version));
            }
            // The packets: the shift key from player_input on 1.21.8 and from the player command on 1.21.4;
            // interact's three forms (only the attack is taken); the spectator menu's teleport.
            const auto parse = [&](std::string_view name, const Writer& w) {
                protocol::Reader r(w.bytes());
                auto parsed = f.wire.parse_play(f.wire.ids().id(State::play, Bound::serverbound, name), r);
                require(parsed.has_value() && r.remaining() == 0, std::format("{}: {} parsed whole", version, name));
                return *parsed;
            };
            const auto sneaks = [](const std::optional<I>& i) -> std::optional<bool> {
                if (!i) return std::nullopt;
                if (const auto* s = std::get_if<server::intent::Sneak>(&*i)) return s->sneaking;
                return std::nullopt;
            };
            require(sneaks(parse("player_input", Writer().u8(0x20))) == (v772 ? std::optional(true) : std::nullopt),
                    std::format("{}: player_input's shift", version));
            require(sneaks(parse("player_command", Writer().varint(1).varint(0).varint(0))) == (v772 ? std::nullopt : std::optional(true)) &&
                        sneaks(parse("player_command", Writer().varint(1).varint(1).varint(0))) == (v772 ? std::nullopt : std::optional(false)),
                    std::format("{}: the player command's shift", version));
            const auto attack = parse("interact", Writer().varint(7).varint(1).boolean(false));
            require(attack && std::holds_alternative<server::intent::Attack>(*attack) && std::get<server::intent::Attack>(*attack).entity == 7,
                    std::format("{}: an attack", version));
            require(!parse("interact", Writer().varint(7).varint(0).varint(0).boolean(true)) &&
                        !parse("interact", Writer().varint(7).varint(2).f32(0.5f).f32(1.0f).f32(0.5f).varint(1).boolean(false)),
                    std::format("{}: interactions passed over, read whole", version));
            const auto teleport = parse("teleport_to_entity", Writer().uuid({1, 2}));
            require(teleport && std::holds_alternative<server::intent::TeleportToEntity>(*teleport), std::format("{}: the spectator menu's teleport", version));
            const auto swing = parse("swing", Writer().varint(1));
            require(swing && std::holds_alternative<server::intent::Swing>(*swing) && std::get<server::intent::Swing>(*swing).hand == 1,
                    std::format("{}: an arm's swing (client/armor)", version));
        }
    });

    suite.run("the air supply and frozen ticks are saved with the players", [] {
        const auto dir = temporary_directory();
        auto config = offline_config();
        config.world_directory = dir / "world";
        // A player's entity data entries 1 (air) and 7 (frozen ticks), when given.
        const auto breath = [](TestClient& c) {
            std::pair<std::optional<std::int32_t>, std::optional<std::int32_t>> got;
            const auto d = c.until(State::play, "set_entity_data");
            if (!d) return got;
            auto r = d->reader();
            (void)r.varint();
            for (auto index = r.u8().value(); index != 0xFF; index = r.u8().value()) {
                const auto type = r.varint().value();
                if (type == 0 || type == 8) (void)r.u8();
                else if (type == 3) (void)r.f32();
                else if (type == 1 || type == 21) {
                    const auto v = r.varint().value();
                    if (index == 1) got.first = v;
                    if (index == 7) got.second = v;
                } else {
                    break;
                }
            }
            return got;
        };
        {
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Diver");
            const auto at = confirm_spawn(c);
            const auto x = static_cast<int>(std::floor(at[0])), y = static_cast<int>(std::floor(at[1])), z = static_cast<int>(std::floor(at[2]));
            c.send(State::play, "chat_command", Writer().string("gamemode survival"));
            c.send(State::play, "chat_command", Writer().string(std::format("fill {} {} {} {} {} {} powder_snow", x, y, z, x, y + 1, z)));
            // Frozen to 40 or more, then under water for 10 ticks or more.
            std::int32_t frozen = 0, air = 300;
            for (int k = 0; k < 400 && frozen < 40; ++k)
                if (const auto [a, f] = breath(c); f) frozen = *f;
            c.send(State::play, "chat_command", Writer().string(std::format("fill {} {} {} {} {} {} water[level=0]", x, y, z, x, y + 1, z)));
            for (int k = 0; k < 400 && air > 290; ++k)
                if (const auto [a, f] = breath(c); a) air = *a;
            require(frozen >= 40 && air <= 290, std::format("frozen {} and air {} before leaving", frozen, air));
            s.stop();
        }
        {
            // Players format 8: shown at once on joining again.
            server::Server s(config);
            s.start();
            TestClient c(s.port());
            c.join("Diver");
            std::optional<std::int32_t> air, frozen;
            for (int k = 0; k < 20 && !(air && frozen); ++k) {
                const auto [a, f] = breath(c);
                if (a && !air) air = a;
                if (f && !frozen) frozen = f;
            }
            require(air && *air < 291 && frozen && *frozen > 0 && *frozen < 40,
                    std::format("air {} and frozen ticks {} after rejoining", air.value_or(-99), frozen.value_or(-99)));
            s.stop();
        }
        std::filesystem::remove_all(dir);
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
            // The player's first data: the health, then the absorption (client/player_data).
            require(d.u8().value() == 9 && d.varint().value() == 3 && d.f32().value() > 0.0f, "the health first");
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
        // furnace's, the crafter's, the brewing stand's, the lectern's and the
        // beacon's (whose table copies components, so it was never used); but wheat's,
        // which its rules use. The 49
        // wall torches, signs, hanging signs, banners and heads count with the
        // standing ones' tables (the banners' and the player head's are
        // refused: they copy components).
        for (const auto& e : {Expected{"CIRCUIT_VANILLA_JAR_1_21_8", "java-1.21.8", "1.21.8", 925, 887, 1407, 984},
                              Expected{"CIRCUIT_VANILLA_JAR_1_21_4", "java-1.21.4", "1.21.4", 917, 879, 1370, 948}}) {
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
