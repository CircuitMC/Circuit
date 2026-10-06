// SPDX-License-Identifier: AGPL-3.0-only
#include "java_bridge.hpp"
#include "log.hpp"
#include <circuit/gameplay/rules.hpp>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <circuit/protocol/nbt.hpp>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace circuit::server {
using plugin::Permission;
using plugin::Status;

namespace {
constexpr std::int32_t protocol_version = 1;
constexpr std::size_t max_frame = std::size_t{1} << 20;
constexpr std::size_t max_inbox = 4096;                // messages waiting for the game thread
constexpr std::size_t max_outbox_bytes = std::size_t{8} << 20;
constexpr std::size_t requests_per_tick = 1024;
constexpr auto connect_timeout = std::chrono::seconds(30);

enum : std::uint8_t {
    hello = 1, log_message = 2, request = 3, tick_done = 4, command_reply = 5, ready = 6,
    welcome = 101, response = 102, tick_message = 103, command_message = 104, shutdown_message = 105,
};
enum : std::uint8_t { op_time = 1, op_block_at = 2, op_items = 3, op_set_block = 4, op_use_block = 5, op_register = 6, op_subscribe = 7 };
enum : std::uint8_t { ok = 0, denied = 1, invalid = 2, unavailable = 3 };

// DataOutputStream encoding: big-endian numbers, modified UTF-8 with a 16-bit length.
class Out {
public:
    explicit Out(std::uint8_t type) { bytes_.push_back(static_cast<std::byte>(type)); }
    Out& u8(std::uint8_t v) {
        bytes_.push_back(static_cast<std::byte>(v));
        return *this;
    }
    Out& i32(std::int32_t v) { return be(static_cast<std::uint32_t>(v), 4); }
    Out& i64(std::int64_t v) { return be(static_cast<std::uint64_t>(v), 8); }
    Out& f64(double v) { return be(std::bit_cast<std::uint64_t>(v), 8); }
    Out& utf(std::string_view s) {
        auto m = protocol::nbt::to_modified_utf8(s);
        if (m.size() > 65535) m = protocol::nbt::to_modified_utf8(s.substr(0, 16000));
        be(m.size(), 2);
        for (const char c : m) bytes_.push_back(static_cast<std::byte>(c));
        return *this;
    }
    std::vector<std::byte> frame() const {
        std::vector<std::byte> out;
        const auto n = static_cast<std::uint32_t>(bytes_.size());
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::byte>((n >> (8 * i)) & 0xFF));
        out.insert(out.end(), bytes_.begin(), bytes_.end());
        return out;
    }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

private:
    Out& be(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bytes_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
        return *this;
    }
    std::vector<std::byte> bytes_;
};

class In {
public:
    explicit In(std::span<const std::byte> b) : b_(b) {}
    std::optional<std::uint64_t> be(int n) {
        if (b_.size() - at_ < static_cast<std::size_t>(n)) return std::nullopt;
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(b_[at_++]);
        return v;
    }
    std::optional<std::uint8_t> u8() {
        const auto v = be(1);
        return v ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(*v)) : std::nullopt;
    }
    std::optional<std::int32_t> i32() {
        const auto v = be(4);
        return v ? std::optional<std::int32_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(*v))) : std::nullopt;
    }
    std::optional<std::int64_t> i64() {
        const auto v = be(8);
        return v ? std::optional<std::int64_t>(static_cast<std::int64_t>(*v)) : std::nullopt;
    }
    std::optional<std::string> utf() {
        const auto n = be(2);
        if (!n || b_.size() - at_ < *n) return std::nullopt;
        auto s = protocol::nbt::from_modified_utf8(b_.subspan(at_, *n));
        at_ += *n;
        return s ? std::optional<std::string>(std::move(*s)) : std::nullopt;
    }
    [[nodiscard]] bool done() const noexcept { return at_ == b_.size(); }

private:
    std::span<const std::byte> b_;
    std::size_t at_{};
};

bool read_exact(int fd, std::byte* data, std::size_t n) {
    std::size_t done = 0;
    while (done < n) {
        const auto r = ::read(fd, data + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        done += static_cast<std::size_t>(r);
    }
    return true;
}
bool write_all(int fd, std::span<const std::byte> data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const auto r = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        done += static_cast<std::size_t>(r);
    }
    return true;
}
// One frame's payload; nullopt at end of stream, an empty vector for a bad size.
std::optional<std::vector<std::byte>> read_frame(int fd) {
    std::array<std::byte, 4> head{};
    if (!read_exact(fd, head.data(), 4)) return std::nullopt;
    std::uint32_t n = 0;
    for (const auto b : head) n = (n << 8) | std::to_integer<std::uint32_t>(b);
    if (n < 1 || n > max_frame) return std::vector<std::byte>{};
    std::vector<std::byte> payload(n);
    if (!read_exact(fd, payload.data(), n)) return std::nullopt;
    return payload;
}
} // namespace

struct JavaBridge::Impl {
    GameServices& game;
    const ServerConfig& config;
    const contracts::VersionBundle& bundle;
    plugin::Permissions permissions;
    CommandRegistrar registrar;

    std::filesystem::path directory;
    int listener{-1}, connection{-1};
    pid_t child{-1};
    std::thread reader, writer;

    std::mutex mutex;
    std::condition_variable outbox_ready;
    std::deque<std::vector<std::byte>> inbox;   // payloads from the JVM
    std::deque<std::vector<std::byte>> outbox;  // frames to the JVM
    std::size_t outbox_bytes{};
    bool closing{false};
    std::string error; // first failure; set by any thread under `mutex`

    bool is_subscribed{false};
    std::uint64_t last_sent{}, last_acknowledged{};
    bool any_sent{false};
    std::uint64_t next_invocation{1};
    std::map<std::uint64_t, std::string> invocations; // invocation -> player

    void fail(std::string why) {
        std::lock_guard lock(mutex);
        if (error.empty()) error = std::move(why);
        outbox_ready.notify_all();
    }
    bool broken() {
        std::lock_guard lock(mutex);
        return !error.empty();
    }

    void send(const Out& message) {
        auto frame = message.frame();
        std::lock_guard lock(mutex);
        if (!error.empty() || closing) return;
        if (outbox_bytes + frame.size() > max_outbox_bytes) {
            error = "the JVM does not read its messages (outbox full)";
            outbox_ready.notify_all();
            return;
        }
        outbox_bytes += frame.size();
        outbox.push_back(std::move(frame));
        outbox_ready.notify_all();
    }

    void start() {
        std::string templ = (std::filesystem::temp_directory_path() / "circuit-bridge-XXXXXX").string();
        if (!::mkdtemp(templ.data())) throw std::runtime_error("java bridge: cannot create a private directory");
        directory = templ; // mode 0700
        const auto path = (directory / "bridge.sock").string();
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) throw std::runtime_error("java bridge: socket path too long");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(listener, 1) != 0)
            throw std::runtime_error(std::format("java bridge: cannot listen on {}: {}", path, std::strerror(errno)));

        std::array<unsigned char, 32> raw{};
        if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) throw std::runtime_error("java bridge: no random token");
        std::string token;
        for (const auto b : raw) token += std::format("{:02x}", b);

        std::vector<std::string> args{config.java, "-cp", config.java_bridge, "org.circuitmc.bridge.Main", "--socket", path};
        for (const auto& c : config.java_plugin_classes) {
            args.emplace_back("--plugin-class");
            args.push_back(c);
        }
        if (!config.java_plugins.empty()) {
            args.emplace_back("--plugins");
            args.push_back(config.java_plugins.string());
        }
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        std::vector<std::string> env_storage;
        for (char** e = environ; *e; ++e)
            if (!std::string_view(*e).starts_with("CIRCUIT_BRIDGE_TOKEN=")) env_storage.emplace_back(*e);
        env_storage.push_back("CIRCUIT_BRIDGE_TOKEN=" + token);
        std::vector<char*> envp;
        for (auto& e : env_storage) envp.push_back(e.data());
        envp.push_back(nullptr);
        if (const int rc = ::posix_spawnp(&child, config.java.c_str(), nullptr, nullptr, argv.data(), envp.data()); rc != 0) {
            child = -1;
            throw std::runtime_error(std::format("java bridge: cannot start {}: {}", config.java, std::strerror(rc)));
        }

        // Accept the JVM's connection, checking that it runs as this user.
        pollfd p{listener, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(std::chrono::milliseconds(connect_timeout).count())) != 1)
            throw std::runtime_error("java bridge: the JVM did not connect in time");
        connection = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (connection < 0) throw std::runtime_error("java bridge: accept failed");
        ucred cred{};
        socklen_t length = sizeof cred;
        if (::getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &cred, &length) != 0 || cred.uid != ::getuid())
            throw std::runtime_error("java bridge: the peer runs as another user");
        timeval timeout{10, 0};
        ::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        const auto first = read_frame(connection);
        if (!first || first->empty()) throw std::runtime_error("java bridge: no handshake");
        In in(*first);
        const auto type = in.u8();
        const auto version = in.i32();
        const auto presented = in.utf();
        if (type != hello || !presented || !in.done())
            throw std::runtime_error("java bridge: malformed handshake");
        if (presented->size() != token.size() || CRYPTO_memcmp(presented->data(), token.data(), token.size()) != 0)
            throw std::runtime_error("java bridge: wrong token");
        if (version != protocol_version)
            throw std::runtime_error(std::format("java bridge: protocol version {} is not supported", version.value_or(-1)));
        timeval none{0, 0};
        ::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
        ::close(listener);
        listener = -1;

        reader = std::thread([this] { read_loop(); });
        writer = std::thread([this] { write_loop(); });
        send(Out(welcome).i32(protocol_version).utf(bundle.id).i32(static_cast<std::int32_t>(permissions)));
        log_info(std::format("java bridge started (pid {})", child));
    }

    void read_loop() {
        for (;;) {
            auto payload = read_frame(connection);
            if (!payload) {
                fail("the JVM closed the connection");
                return;
            }
            if (payload->empty()) {
                fail("the JVM sent a frame of invalid size");
                return;
            }
            std::lock_guard lock(mutex);
            if (inbox.size() >= max_inbox) {
                if (error.empty()) error = "the JVM sends more than the server handles (inbox full)";
                outbox_ready.notify_all();
                return;
            }
            inbox.push_back(std::move(*payload));
        }
    }

    void write_loop() {
        std::unique_lock lock(mutex);
        for (;;) {
            outbox_ready.wait(lock, [&] { return !outbox.empty() || closing || !error.empty(); });
            if (!error.empty()) return;
            if (outbox.empty()) return; // closing and drained
            auto frame = std::move(outbox.front());
            outbox.pop_front();
            outbox_bytes -= frame.size();
            lock.unlock();
            const bool sent = write_all(connection, frame);
            lock.lock();
            if (!sent) {
                if (error.empty()) error = "the connection to the JVM failed";
                return;
            }
        }
    }

    bool allows(Permission p) const { return plugin::allows(permissions, p); }

    void respond(std::int64_t id, std::uint8_t status, std::string_view message) {
        send(Out(response).i64(id).u8(status).utf(message));
    }

    void handle_request(In& in) {
        const auto id = in.i64();
        const auto op = in.u8();
        if (!id || !op) return fail("malformed request");
        const auto& engine = game.simulation();
        const auto& mapping = game.content_mapping();
        const auto pos = [&]() -> std::optional<contracts::BlockPos> {
            const auto x = in.i32(), y = in.i32(), z = in.i32();
            if (!x || !y || !z) return std::nullopt;
            return contracts::BlockPos{*x, *y, *z};
        };
        switch (*op) {
        case op_time:
            if (!allows(Permission::world_read)) return respond(*id, denied, "world.read is not granted");
            return send(Out(response).i64(*id).u8(ok).i64(static_cast<std::int64_t>(engine.time())));
        case op_block_at: {
            const auto p = pos();
            if (!p) return fail("malformed request");
            if (!allows(Permission::world_read)) return respond(*id, denied, "world.read is not granted");
            const auto s = engine.view().block(*p);
            if (!s) return respond(*id, s.error() == contracts::ErrorCode::unloaded_chunk ? unavailable : invalid, "no block there");
            return send(Out(response).i64(*id).u8(ok).utf(mapping.notation(*s)));
        }
        case op_items: {
            const auto cx = in.i32(), cz = in.i32();
            if (!cx || !cz) return fail("malformed request");
            if (!allows(Permission::world_read)) return respond(*id, denied, "world.read is not granted");
            Out out(response);
            out.i64(*id).u8(ok);
            const auto column = engine.column({*cx, *cz});
            out.i32(column ? static_cast<std::int32_t>(column->entities.size()) : 0);
            if (column)
                for (const auto& e : column->entities) {
                    const auto real = [](std::int64_t v) { return static_cast<double>(v) / contracts::fixed_one; };
                    out.i64(static_cast<std::int64_t>(e.id.value)).f64(real(e.position.x)).f64(real(e.position.y)).f64(real(e.position.z));
                    out.utf(mapping.item_name(e.stack.item)).i32(e.stack.count);
                }
            if (out.size() > max_frame) return respond(*id, unavailable, "too many items");
            return send(out);
        }
        case op_set_block:
        case op_use_block: {
            const auto p = pos();
            std::optional<std::string> state;
            if (*op == op_set_block && !(state = in.utf())) return fail("malformed request");
            if (!p) return fail("malformed request");
            if (!allows(Permission::world_write)) return respond(*id, denied, "world.write is not granted");
            if (!gameplay::in_world(bundle, *p)) return respond(*id, invalid, "outside the world");
            contracts::CommandBody body = contracts::UseBlock{*p};
            if (state) {
                const auto s = mapping.block(*state);
                if (!s) return respond(*id, invalid, s.error());
                body = contracts::SetBlock{*p, s->raw(), std::nullopt};
            }
            const auto admission = game.submit(std::move(body));
            if (!admission) return respond(*id, unavailable, "the command queue is full");
            return send(Out(response).i64(*id).u8(ok).i64(static_cast<std::int64_t>(*admission)));
        }
        case op_register: {
            const auto name = in.utf(), usage = in.utf();
            if (!name || !usage) return fail("malformed request");
            if (!allows(Permission::commands)) return respond(*id, denied, "commands is not granted");
            if (!registrar(*name, *usage)) return respond(*id, invalid, "the name is invalid or taken");
            return send(Out(response).i64(*id).u8(ok));
        }
        case op_subscribe:
            if (!allows(Permission::world_observe)) return respond(*id, denied, "world.observe is not granted");
            is_subscribed = true;
            return send(Out(response).i64(*id).u8(ok));
        default:
            return fail(std::format("unknown request operation {}", *op));
        }
    }

    void serve() {
        std::deque<std::vector<std::byte>> batch;
        {
            std::lock_guard lock(mutex);
            const auto n = std::min(inbox.size(), requests_per_tick);
            batch.insert(batch.end(), std::make_move_iterator(inbox.begin()), std::make_move_iterator(inbox.begin() + static_cast<std::ptrdiff_t>(n)));
            inbox.erase(inbox.begin(), inbox.begin() + static_cast<std::ptrdiff_t>(n));
        }
        for (auto& payload : batch) {
            if (broken()) return;
            In in(payload);
            const auto type = in.u8();
            if (type == log_message) {
                const auto level = in.u8();
                const auto text = in.utf();
                if (!level || !text) return fail("malformed log message");
                const auto line = "[java] " + *text;
                if (*level >= 2) log_error(line);
                else if (*level == 1) log_warn(line);
                else log_info(line);
            } else if (type == request) {
                handle_request(in);
            } else if (type == tick_done) {
                const auto t = in.i64();
                if (!t) return fail("malformed tick acknowledgement");
                last_acknowledged = std::max(last_acknowledged, static_cast<std::uint64_t>(*t));
            } else if (type == command_reply) {
                const auto invocation = in.i64();
                const auto text = in.utf();
                if (!invocation || !text) return fail("malformed command reply");
                if (const auto it = invocations.find(static_cast<std::uint64_t>(*invocation)); it != invocations.end()) {
                    if (!text->empty()) game.tell(it->second, *text);
                    invocations.erase(it);
                }
            } else if (type == ready) {
                const auto n = in.i32();
                std::string names;
                for (std::int32_t i = 0; n && i < *n && i < 1000; ++i)
                    if (const auto s = in.utf()) names += (names.empty() ? "" : ", ") + *s;
                log_info(std::format("java bridge ready with {} plugins: {}", n.value_or(0), names));
            } else {
                return fail(std::format("unexpected message type {}", type.value_or(0)));
            }
        }
        if (any_sent && last_sent > last_acknowledged + config.java_max_lag_ticks)
            fail(std::format("the JVM is {} ticks behind", last_sent - last_acknowledged));
        if (child > 0) {
            int status = 0;
            if (::waitpid(child, &status, WNOHANG) == child) {
                child = -1;
                fail("the JVM exited");
            }
        }
    }

    void tick(const plugin::TickReport& report) {
        if (broken()) return;
        Out out(tick_message);
        out.i64(static_cast<std::int64_t>(report.tick));
        const auto changes = is_subscribed ? report.blocks.size() : 0;
        // Very large ticks are cut to the frame size; the count says how many follow.
        std::size_t n = 0, bytes = 16;
        for (; n < changes; ++n) {
            bytes += 12 + 4 + report.blocks[n].before.size() + report.blocks[n].after.size();
            if (bytes > max_frame - 64) break;
        }
        out.i32(static_cast<std::int32_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = report.blocks[i];
            out.i32(c.pos.x).i32(c.pos.y).i32(c.pos.z).utf(c.before).utf(c.after);
        }
        send(out);
        last_sent = report.tick;
        any_sent = true;
    }

    void command(std::string_view player, std::string_view name, std::span<const std::string> arguments) {
        const auto invocation = next_invocation++;
        invocations.emplace(invocation, std::string(player));
        Out out(command_message);
        out.i64(static_cast<std::int64_t>(invocation)).utf(player).utf(name).i32(static_cast<std::int32_t>(std::min<std::size_t>(arguments.size(), 64)));
        for (std::size_t i = 0; i < arguments.size() && i < 64; ++i) out.utf(arguments[i]);
        send(out);
    }

    void stop() {
        // A failed JVM is killed at once; a healthy one is asked to stop and
        // gets two seconds to disable its plugins. The writer keeps sending
        // meanwhile; closing the socket afterwards ends both threads, even
        // when a write is blocked.
        const bool healthy = !broken();
        if (connection >= 0 && healthy) {
            std::lock_guard lock(mutex);
            if (!closing) {
                const auto frame = Out(shutdown_message).frame();
                outbox_bytes += frame.size();
                outbox.push_back(frame);
                closing = true;
                outbox_ready.notify_all();
            }
        }
        if (child > 0) {
            int status = 0;
            if (!healthy) ::kill(child, SIGKILL);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (::waitpid(child, &status, WNOHANG) == 0) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    ::kill(child, SIGKILL);
                    ::waitpid(child, &status, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            child = -1;
        }
        if (connection >= 0) ::shutdown(connection, SHUT_RDWR);
        {
            std::lock_guard lock(mutex);
            closing = true;
            if (error.empty()) error = "stopped";
            outbox_ready.notify_all();
        }
        if (reader.joinable()) reader.join();
        if (writer.joinable()) writer.join();
        if (connection >= 0) ::close(connection);
        if (listener >= 0) ::close(listener);
        connection = listener = -1;
        if (!directory.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
            directory.clear();
        }
    }
};

JavaBridge::JavaBridge(GameServices& game, const ServerConfig& config, const contracts::VersionBundle& bundle,
                       plugin::Permissions permissions, CommandRegistrar registrar)
    : impl_(std::make_unique<Impl>(game, config, bundle, permissions, std::move(registrar))) {}
JavaBridge::~JavaBridge() { impl_->stop(); }

void JavaBridge::start() {
    try {
        impl_->start();
    } catch (...) {
        impl_->stop();
        throw;
    }
}
void JavaBridge::serve() { impl_->serve(); }
void JavaBridge::tick(const plugin::TickReport& report) { impl_->tick(report); }
void JavaBridge::command(std::string_view player, std::string_view name, std::span<const std::string> arguments) {
    impl_->command(player, name, arguments);
}
bool JavaBridge::subscribed() const noexcept { return impl_->is_subscribed; }
bool JavaBridge::failed() const noexcept { return impl_->broken(); }
std::string JavaBridge::failure() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}
void JavaBridge::stop() { impl_->stop(); }

} // namespace circuit::server
