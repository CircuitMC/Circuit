// SPDX-License-Identifier: AGPL-3.0-only
#include "extensions.hpp"
#include "log.hpp"
#include "session.hpp"
#include <circuit/contracts/version.hpp>
#include <circuit/runtime/tick_clock.hpp>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

namespace circuit::server {

struct Server::Impl {
    ServerConfig config;
    const contracts::VersionBundle* bundle{};
    std::unique_ptr<Wire> wire;
    const protocol::ContentTables* content{};
    const evidence::ContentMapping* mapping{};
    const std::vector<RegistryEntries>* registries{};
    std::unique_ptr<Game> game;
    auth::offline_authenticator offline;
    std::optional<auth::RsaKeyPair> keys;
    std::optional<auth::SessionVerifier> verifier;
    asio::thread_pool auth_pool{2};
    std::unique_ptr<Shared> shared;

    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    asio::ip::tcp::acceptor acceptor{io};
    std::vector<std::thread> network;
    std::thread game_thread;
    std::atomic<bool> game_running{false};
    bool started{false};

    explicit Impl(ServerConfig c) : config(std::move(c)) {
        const auto selected = contracts::BundleRegistry::select_for_simulation(config.game_version);
        if (!selected) throw std::invalid_argument(std::format("server: {} has no executable rules", config.game_version));
        bundle = *selected;
        // The play protocol of the bundle's game version.
        const std::int32_t protocol_number = bundle->protocol;
        content = protocol::ContentTables::for_protocol(protocol_number);
        registries = synced_registries(protocol_number);
        mapping = evidence::ContentMapping::for_game_version(bundle->game_version);
        if (!content || !registries || !mapping)
            throw std::invalid_argument(std::format("server: no play protocol for {}", config.game_version));
        wire = std::make_unique<Wire>(protocol_number);
        if (config.view_distance < 2 || config.view_distance > 32) throw std::invalid_argument("server: view distance must be 2-32");
        if (config.simulation_distance < 1 || config.simulation_distance > static_cast<std::int32_t>(bundle->limits.max_ticket_radius))
            throw std::invalid_argument("server: simulation distance outside the ticket limit");
        // The port is bound before the world is opened: a server that cannot
        // listen fails without touching its world.
        const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(config.bind_address), config.port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();
        game = std::make_unique<Game>(config, *bundle, *wire, *content, *mapping, *registries);
        shared = std::make_unique<Shared>(config, *wire, *registries, *registries_with_data(protocol_number),
                                          bundle->game_version, *game, offline);
        if (config.online_mode) {
            auto v = auth::SessionVerifier::create(config.session_server);
            if (!v) throw std::invalid_argument("server: " + v.error());
            verifier = std::move(*v);
            keys = auth::RsaKeyPair::generate(1024);
            shared->keys = &*keys;
            shared->verifier = &*verifier;
            shared->auth_pool = &auth_pool;
        }
    }

    void accept() {
        acceptor.async_accept(asio::make_strand(io), [this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
            if (ec) return; // the acceptor was closed
            if (shared->stopping) return;
            std::size_t open = 0;
            {
                std::lock_guard lock(shared->sessions_mutex);
                open = shared->sessions.size();
            }
            if (open >= config.max_connections) {
                asio::error_code ignored;
                socket.close(ignored); // over the connection limit: refuse before reading anything
                accept();
                return;
            }
            asio::error_code nodelay;
            socket.set_option(asio::ip::tcp::no_delay(true), nodelay);
            std::make_shared<Session>(std::move(socket), *shared)->start();
            accept();
        });
    }

    void run_game() {
        runtime::TickClock clock(std::chrono::steady_clock::now());
        while (game_running) {
            const auto now = std::chrono::steady_clock::now();
            if (!clock.due(now)) {
                std::this_thread::sleep_until(std::min(clock.deadline(), now + std::chrono::milliseconds(5)));
                continue;
            }
            try {
                game->tick();
            } catch (const std::exception& e) {
                // A failed tick leaves the engine unusable: stop accepting and disconnect everyone.
                log_error(std::format("tick failed: {}", e.what()));
                shared->stopping = true;
                game->shutdown("The server stopped after an internal error");
                game_running = false;
                return;
            }
            clock.ticked();
        }
    }
};

Server::Server(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Server::~Server() { stop(); }

void Server::start() {
    auto& i = *impl_;
    if (i.started) return;
    i.work.emplace(i.io.get_executor());
    i.accept();
    for (unsigned t = 0; t < std::max(1u, i.config.network_threads); ++t) i.network.emplace_back([&i] { i.io.run(); });
    i.game_running = true;
    i.game_thread = std::thread([&i] { i.run_game(); });
    i.started = true;
    log_info(std::format("Circuit listening on {}:{} ({}, protocol {}, {} mode)", i.config.bind_address, port(),
                         i.config.game_version, i.wire->protocol(), i.config.online_mode ? "online" : "offline"));
}

void Server::stop() {
    auto& i = *impl_;
    if (!i.started) return;
    i.started = false;
    i.shared->stopping = true;
    asio::post(i.io, [&i] {
        asio::error_code ec;
        i.acceptor.close(ec);
    });
    i.game_running = false;
    if (i.game_thread.joinable()) i.game_thread.join();
    // The game thread has stopped: kick everyone from here, then let the
    // sessions write their reasons before the network threads stop.
    i.game->shutdown("The server is stopping");
    std::vector<std::shared_ptr<Session>> open;
    {
        std::lock_guard lock(i.shared->sessions_mutex);
        for (auto& [id, weak] : i.shared->sessions)
            if (auto s = weak.lock()) open.push_back(std::move(s));
    }
    for (auto& s : open) s->kick("The server is stopping");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(i.shared->sessions_mutex);
            if (i.shared->sessions.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (auto& s : open) s->close();
    i.work.reset();
    i.io.stop();
    for (auto& t : i.network) t.join();
    i.network.clear();
    i.auth_pool.join();
    log_info("Circuit stopped");
}

std::uint16_t Server::port() const {
    asio::error_code ec;
    const auto ep = impl_->acceptor.local_endpoint(ec);
    return ec ? impl_->config.port : ep.port();
}
std::size_t Server::players() const { return impl_->game->players(); }
bool Server::running() const { return impl_->game_running.load(); }
std::uint64_t Server::ticks() const { return impl_->game->ticks(); }

namespace {
template <class T>
bool parse_number(std::string_view text, T& out, T low, T high) {
    try {
        std::size_t used = 0;
        const auto v = std::stoll(std::string(text), &used);
        if (used != text.size() || v < static_cast<long long>(low) || v > static_cast<long long>(high)) return false;
        out = static_cast<T>(v);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace

std::expected<ServerConfig, std::string> parse_config(std::string_view text) {
    ServerConfig c;
    std::istringstream in{std::string(text)};
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) return std::unexpected(std::format("line {}: expected key=value", number));
        const auto key = line.substr(first, eq - first);
        const auto value = line.substr(eq + 1);
        bool ok = true;
        if (key == "bind-address") c.bind_address = value;
        else if (key == "port") ok = parse_number<std::uint16_t>(value, c.port, 0, 65535);
        else if (key == "game-version") c.game_version = value;
        else if (key == "network-threads") ok = parse_number<unsigned>(value, c.network_threads, 1, 64);
        else if (key == "workers") ok = parse_number<unsigned>(value, c.workers, 1, 64);
        else if (key == "max-players") ok = parse_number<std::int32_t>(value, c.max_players, 1, 10000);
        else if (key == "max-connections") ok = parse_number<std::size_t>(value, c.max_connections, 1, 100000);
        else if (key == "view-distance") ok = parse_number<std::int32_t>(value, c.view_distance, 2, 32);
        else if (key == "simulation-distance") ok = parse_number<std::int32_t>(value, c.simulation_distance, 1, 32);
        else if (key == "compression-threshold") ok = parse_number<std::int32_t>(value, c.compression_threshold, -1, 1 << 20);
        else if (key == "online-mode") {
            if (value == "true") c.online_mode = true;
            else if (value == "false") c.online_mode = false;
            else ok = false;
        } else if (key == "allow-flight") {
            if (value == "true") c.allow_flight = true;
            else if (value == "false") c.allow_flight = false;
            else ok = false;
        } else if (key == "difficulty") {
            if (value == "peaceful") c.difficulty = 0;
            else if (value == "easy") c.difficulty = 1;
            else if (value == "normal") c.difficulty = 2;
            else if (value == "hard") c.difficulty = 3;
            else ok = false;
        } else if (key == "motd") c.motd = value;
        else if (key == "session-server") c.session_server = value;
        else if (key == "auth-timeout-ms") {
            std::int64_t ms = 0;
            ok = parse_number<std::int64_t>(value, ms, 100, 60000);
            c.auth_timeout = std::chrono::milliseconds(ms);
        }
        else if (key == "seed") ok = parse_number<std::uint64_t>(value, c.seed, 0, INT64_MAX);
        else if (key == "max-queued-bytes") ok = parse_number<std::size_t>(value, c.max_queued_bytes, 65536, 1u << 30);
        else if (key == "floor-y") ok = parse_number<std::int32_t>(value, c.floor_y, -2048, 2047);
        else if (key == "world-directory") c.world_directory = value;
        else if (key == "autosave-ticks") ok = parse_number<std::uint32_t>(value, c.autosave_ticks, 20, 1728000);
        else if (key == "log-sync-ticks") ok = parse_number<std::uint32_t>(value, c.log_sync_ticks, 1, 1200);
        else if (key == "plugin") c.plugins.emplace_back(value);
        else if (key.starts_with("plugin-permissions.") && key.size() > 19) {
            ok = parse_permissions(value).has_value();
            c.plugin_permissions[key.substr(19)] = value;
        }
        else if (key == "java-bridge") c.java_bridge = value;
        else if (key == "java") c.java = value;
        else if (key == "java-plugin-class") c.java_plugin_classes.push_back(value);
        else if (key == "java-plugins") c.java_plugins = value;
        else if (key == "java-permissions") {
            ok = parse_permissions(value).has_value();
            c.java_permissions = value;
        }
        else if (key == "java-max-lag-ticks") ok = parse_number<std::uint32_t>(value, c.java_max_lag_ticks, 20, 72000);
        else if (key == "login-timeout" || key == "keep-alive-timeout") {
            std::int64_t seconds = 0;
            ok = parse_number<std::int64_t>(value, seconds, 1, 600);
            (key == "login-timeout" ? c.login_timeout : c.keep_alive_timeout) = std::chrono::seconds(seconds);
        } else return std::unexpected(std::format("line {}: unknown key `{}`", number, key));
        if (!ok) return std::unexpected(std::format("line {}: invalid value for `{}`", number, key));
    }
    return c;
}

std::expected<ServerConfig, std::string> load_config(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot read {}", path.string()));
    std::ostringstream text;
    text << in.rdbuf();
    return parse_config(text.str());
}

} // namespace circuit::server
