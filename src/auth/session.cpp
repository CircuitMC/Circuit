// SPDX-License-Identifier: AGPL-3.0-only
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <circuit/auth/json.hpp>
#include <circuit/auth/session.hpp>
#include <format>

namespace circuit::auth {
namespace {
constexpr std::size_t max_response = 1 << 20;

struct Response {
    int status{};
    std::string body;
};

std::expected<Response, std::string> parse_response(std::string_view raw) {
    const auto head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos) return std::unexpected("no header end");
    const auto head = raw.substr(0, head_end);
    auto body = std::string(raw.substr(head_end + 4));
    if (!head.starts_with("HTTP/1.")) return std::unexpected("not HTTP/1.x");
    const auto sp = head.find(' ');
    if (sp == std::string_view::npos || head.size() < sp + 4) return std::unexpected("no status");
    Response r;
    const auto [end, ec] = std::from_chars(head.data() + sp + 1, head.data() + sp + 4, r.status);
    if (ec != std::errc{}) return std::unexpected("bad status");
    std::string lower(head);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        std::size_t at = 0;
        for (;;) {
            const auto line_end = body.find("\r\n", at);
            if (line_end == std::string::npos) return std::unexpected("bad chunk");
            std::size_t size = 0;
            const auto [e, err] = std::from_chars(body.data() + at, body.data() + line_end, size, 16);
            if (err != std::errc{} && e == body.data() + at) return std::unexpected("bad chunk size");
            at = line_end + 2;
            if (size == 0) break;
            if (at + size > body.size()) return std::unexpected("short chunk");
            decoded.append(body, at, size);
            at += size + 2;
        }
        body = std::move(decoded);
    }
    r.body = std::move(body);
    return r;
}

// GET over plain TCP or TLS, bounded by `timeout` for the whole exchange.
template <class Stream>
std::expected<std::string, std::string> exchange(asio::io_context& io, Stream& stream, const std::string& request,
                                                 std::chrono::milliseconds timeout) {
    std::string response;
    std::array<char, 8192> buf{};
    std::string error;
    bool done = false;
    std::function<void()> read_more = [&] {
        stream.async_read_some(asio::buffer(buf), [&](const asio::error_code& ec, std::size_t n) {
            response.append(buf.data(), n);
            if (response.size() > max_response) {
                error = "response too large";
                done = true;
                return;
            }
            if (ec) {
                done = true; // end of stream or a closed TLS session ends the response
                return;
            }
            read_more();
        });
    };
    asio::async_write(stream, asio::buffer(request), [&](const asio::error_code& ec, std::size_t) {
        if (ec) {
            error = ec.message();
            done = true;
            return;
        }
        read_more();
    });
    io.restart();
    io.run_for(timeout);
    if (!done) return std::unexpected("timed out");
    if (!error.empty()) return std::unexpected(error);
    return response;
}
} // namespace

std::string_view describe(SessionError e) noexcept {
    switch (e) {
    case SessionError::not_joined: return "the session service did not confirm the login";
    case SessionError::unavailable: return "the session service is unavailable";
    case SessionError::invalid_response: return "the session service sent an invalid answer";
    }
    return "unknown";
}

std::optional<std::string> dashed_uuid(std::string_view hex) {
    if (hex.size() != 32) return std::nullopt;
    for (const char c : hex)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    std::string out;
    for (std::size_t i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) out += '-';
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(hex[i])));
    }
    return out;
}

std::string url_encode(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else out += std::format("%{:02X}", u);
    }
    return out;
}

std::expected<SessionVerifier, std::string> SessionVerifier::create(std::string_view base) {
    SessionVerifier v;
    std::string_view rest;
    if (base.starts_with("https://")) {
        v.tls_ = true;
        rest = base.substr(8);
    } else if (base.starts_with("http://")) {
        rest = base.substr(7);
    } else {
        return std::unexpected("the session server URL must start with https://");
    }
    const auto slash = rest.find('/');
    auto authority = rest.substr(0, slash);
    v.path_ = slash == std::string_view::npos ? "" : std::string(rest.substr(slash));
    while (v.path_.ends_with('/')) v.path_.pop_back();
    const auto colon = authority.rfind(':');
    v.host_ = std::string(authority.substr(0, colon));
    v.port_ = colon == std::string_view::npos ? (v.tls_ ? "443" : "80") : std::string(authority.substr(colon + 1));
    if (v.host_.empty()) return std::unexpected("the session server URL has no host");
    if (!v.tls_ && v.host_ != "127.0.0.1" && v.host_ != "localhost" && v.host_ != "::1")
        return std::unexpected("plain http is only accepted for loopback session servers");
    return v;
}

std::expected<VerifiedProfile, SessionError> SessionVerifier::has_joined(std::string_view name, std::string_view server_hash,
                                                                         std::chrono::milliseconds timeout) const {
    const auto target = std::format("{}/session/minecraft/hasJoined?username={}&serverId={}", path_, url_encode(name),
                                    url_encode(server_hash));
    const auto request = std::format("GET {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: Circuit\r\nAccept: application/json\r\n"
                                     "Connection: close\r\n\r\n",
                                     target, host_);
    std::expected<std::string, std::string> raw = std::unexpected("not attempted");
    try {
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        asio::error_code ec;
        const auto endpoints = resolver.resolve(host_, port_, ec);
        if (ec) return std::unexpected(SessionError::unavailable);
        if (tls_) {
            asio::ssl::context ctx(asio::ssl::context::tls_client);
            ctx.set_default_verify_paths();
            ctx.set_verify_mode(asio::ssl::verify_peer);
            asio::ssl::stream<asio::ip::tcp::socket> stream(io, ctx);
            SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str());
            stream.set_verify_callback(asio::ssl::host_name_verification(host_));
            bool ready = false;
            std::string error;
            asio::async_connect(stream.lowest_layer(), endpoints, [&](const asio::error_code& e, const auto&) {
                if (e) {
                    error = e.message();
                    ready = true;
                    return;
                }
                stream.async_handshake(asio::ssl::stream_base::client, [&](const asio::error_code& h) {
                    if (h) error = h.message();
                    ready = true;
                });
            });
            io.run_for(timeout);
            if (!ready || !error.empty()) return std::unexpected(SessionError::unavailable);
            raw = exchange(io, stream, request, timeout);
        } else {
            asio::ip::tcp::socket socket(io);
            bool ready = false;
            std::string error;
            asio::async_connect(socket, endpoints, [&](const asio::error_code& e, const auto&) {
                if (e) error = e.message();
                ready = true;
            });
            io.run_for(timeout);
            if (!ready || !error.empty()) return std::unexpected(SessionError::unavailable);
            raw = exchange(io, socket, request, timeout);
        }
    } catch (const std::exception&) {
        return std::unexpected(SessionError::unavailable);
    }
    if (!raw) return std::unexpected(SessionError::unavailable);
    const auto response = parse_response(*raw);
    if (!response) return std::unexpected(SessionError::invalid_response);
    if (response->status == 204) return std::unexpected(SessionError::not_joined);
    if (response->status != 200) return std::unexpected(SessionError::unavailable);
    const auto json = Json::parse(response->body);
    if (!json || json->kind() != Json::Kind::object) return std::unexpected(SessionError::invalid_response);
    const auto* id = json->string("id");
    const auto* profile_name = json->string("name");
    if (!id || !profile_name) return std::unexpected(SessionError::invalid_response);
    auto uuid = dashed_uuid(*id);
    if (!uuid) return std::unexpected(SessionError::invalid_response);
    VerifiedProfile profile{std::move(*uuid), *profile_name, {}};
    if (const auto* props = json->find("properties"); props && props->kind() == Json::Kind::array) {
        for (const auto& p : props->items()) {
            const auto* n = p.string("name");
            const auto* v = p.string("value");
            if (!n || !v) return std::unexpected(SessionError::invalid_response);
            ProfileProperty prop{*n, *v, std::nullopt};
            if (const auto* s = p.string("signature")) prop.signature = *s;
            profile.properties.push_back(std::move(prop));
        }
    }
    return profile;
}

} // namespace circuit::auth
