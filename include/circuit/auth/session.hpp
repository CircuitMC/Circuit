// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Server-side session verification (minecraft.wiki "Java Edition
// protocol/Encryption", Authentication/Server): after the key exchange the
// server asks the session service whether the named player joined with the
// login hash. Any failure denies the login.
namespace circuit::auth {

struct ProfileProperty {
    std::string name, value;
    std::optional<std::string> signature;
};

struct VerifiedProfile {
    std::string uuid; // dashed form
    std::string name;
    std::vector<ProfileProperty> properties;
};

enum class SessionError : std::uint8_t {
    not_joined,       // the service does not confirm the join (HTTP 204)
    unavailable,      // connection, TLS, timeout or HTTP failure
    invalid_response, // a malformed or unexpected answer
};
std::string_view describe(SessionError e) noexcept;

class SessionVerifier {
public:
    // `base` is an https URL such as https://sessionserver.mojang.com, or a
    // plain http URL on a loopback address (tests).
    static std::expected<SessionVerifier, std::string> create(std::string_view base);

    [[nodiscard]] std::expected<VerifiedProfile, SessionError> has_joined(std::string_view name, std::string_view server_hash,
                                                                          std::chrono::milliseconds timeout) const;
    [[nodiscard]] const std::string& host() const noexcept { return host_; }

private:
    bool tls_{};
    std::string host_, port_, path_;
};

// "1111...5555" (32 hex digits) as "11111111-2222-3333-4444-555555555555";
// nullopt when the input is not 32 hex digits.
std::optional<std::string> dashed_uuid(std::string_view hex);
std::string url_encode(std::string_view text);

} // namespace circuit::auth
