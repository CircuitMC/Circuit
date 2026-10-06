// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/auth.hpp>
#include <format>
#include <openssl/evp.h>
#include <stdexcept>

namespace circuit::auth {

bool valid_player_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 16) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

std::string offline_uuid(std::string_view name) {
    const std::string input = "OfflinePlayer:" + std::string(name);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(input.data(), input.size(), digest, &length, EVP_md5(), nullptr) != 1 || length != 16)
        throw std::runtime_error("auth: MD5 unavailable");
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0F) | 0x30); // version 3
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3F) | 0x80); // RFC 4122 variant
    std::string hex;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) hex += '-';
        hex += std::format("{:02x}", digest[i]);
    }
    return hex;
}

std::expected<authenticated_identity, error> offline_authenticator::authenticate(const login_request& request,
                                                                              std::chrono::steady_clock::time_point) {
    if (!valid_player_name(request.claimed_name)) return std::unexpected(error::invalid_request);
    return authenticated_identity{offline_uuid(request.claimed_name), request.claimed_name,
                                  identity_assurance::offline_unverified};
}

} // namespace circuit::auth
