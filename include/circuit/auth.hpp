// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace circuit::auth {

enum class error { unavailable, rejected, invalid_request, upstream_failure };
enum class identity_assurance { offline_unverified, online_verified };

struct login_request {
    std::string claimed_name;
    std::string server_digest;
    std::span<const std::byte> transport_public_key;
};

struct authenticated_identity {
    std::string uuid;
    std::string name;
    identity_assurance assurance{identity_assurance::offline_unverified};
};

// The connection layer must await authentication before admitting a player.
// Implementations perform I/O away from chunk ownership threads. Only a real
// verification provider may return online_verified; a configured name is not proof.
class authenticator {
public:
    virtual ~authenticator() = default;
    virtual std::expected<authenticated_identity, error>
    authenticate(const login_request& request,
                 std::chrono::steady_clock::time_point deadline) = 0;
};

// Safe default until an actual provider is installed. This is NOT a Mojang
// authentication implementation and never grants an online or offline identity.
class deny_authenticator final : public authenticator {
public:
    std::expected<authenticated_identity, error>
    authenticate(const login_request&,
                 std::chrono::steady_clock::time_point) override {
        return std::unexpected(error::unavailable);
    }
};

// Player names accepted at login: 1-16 characters of [A-Za-z0-9_].
bool valid_player_name(std::string_view name) noexcept;

// UUID text of an offline player: the name-based (version 3, MD5) UUID of
// "OfflinePlayer:" + name, the convention clients and vanilla servers use.
std::string offline_uuid(std::string_view name);

// Offline provider. Identities are labelled offline_unverified and live in the
// offline UUID namespace; the claimed name is not proof of anything. Disabling
// online verification does not establish an exemption from Mojang/Microsoft
// terms.
class offline_authenticator final : public authenticator {
public:
    std::expected<authenticated_identity, error>
    authenticate(const login_request& request, std::chrono::steady_clock::time_point deadline) override;
};

} // namespace circuit::auth
