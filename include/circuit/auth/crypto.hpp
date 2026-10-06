// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Cryptography of protocol encryption (minecraft.wiki "Java Edition
// protocol/Encryption"), on OpenSSL: an RSA key exchange, AES-128/CFB8 stream
// ciphers keyed with the shared secret, and the login hash.
namespace circuit::auth {

using ByteVector = std::vector<std::byte>;

ByteVector random_bytes(std::size_t n);

class RsaKeyPair {
public:
    // A fresh key pair; the protocol uses 1024 bits.
    static RsaKeyPair generate(unsigned bits = 1024);
    RsaKeyPair(RsaKeyPair&&) noexcept;
    RsaKeyPair& operator=(RsaKeyPair&&) noexcept;
    ~RsaKeyPair();

    // SubjectPublicKeyInfo in DER, as sent in the encryption request.
    [[nodiscard]] const ByteVector& public_der() const noexcept { return public_der_; }
    // PKCS#1 v1.5 decryption; fails for malformed input.
    [[nodiscard]] std::expected<ByteVector, std::string> decrypt(std::span<const std::byte> data) const;

private:
    struct Key;
    explicit RsaKeyPair(std::unique_ptr<Key> key, ByteVector der);
    std::unique_ptr<Key> key_;
    ByteVector public_der_;
};

// PKCS#1 v1.5 encryption with a DER public key (the client's side; tests).
std::expected<ByteVector, std::string> rsa_encrypt(std::span<const std::byte> public_der, std::span<const std::byte> data);

// AES-128 in CFB8 mode with key and IV equal to the 16-byte shared secret.
// The stream state continues across calls.
class Cfb8 {
public:
    enum class Direction { encrypt, decrypt };
    Cfb8(std::span<const std::byte> secret, Direction direction);
    Cfb8(Cfb8&&) noexcept;
    Cfb8& operator=(Cfb8&&) noexcept;
    ~Cfb8();
    void apply(std::span<std::byte> data);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Minecraft's login hash: SHA-1 of the server id, the shared secret and the
// public key, printed as a signed two's-complement hexadecimal number.
std::string login_hash(std::string_view server_id, std::span<const std::byte> secret, std::span<const std::byte> public_der);
// The same digest formatting applied to arbitrary data (documented examples).
std::string minecraft_hex_digest(std::span<const std::byte> data);

} // namespace circuit::auth
