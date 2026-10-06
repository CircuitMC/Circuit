// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/auth/crypto.hpp>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <stdexcept>

namespace circuit::auth {
namespace {
std::string openssl_error() {
    const auto code = ERR_get_error();
    char buf[256];
    ERR_error_string_n(code, buf, sizeof buf);
    return buf;
}
const unsigned char* bytes(std::span<const std::byte> s) { return reinterpret_cast<const unsigned char*>(s.data()); }
} // namespace

ByteVector random_bytes(std::size_t n) {
    ByteVector out(n);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) != 1)
        throw std::runtime_error("auth: random generator failed: " + openssl_error());
    return out;
}

struct RsaKeyPair::Key {
    EVP_PKEY* pkey{};
    ~Key() { EVP_PKEY_free(pkey); }
};

RsaKeyPair::RsaKeyPair(std::unique_ptr<Key> key, ByteVector der) : key_(std::move(key)), public_der_(std::move(der)) {}
RsaKeyPair::RsaKeyPair(RsaKeyPair&&) noexcept = default;
RsaKeyPair& RsaKeyPair::operator=(RsaKeyPair&&) noexcept = default;
RsaKeyPair::~RsaKeyPair() = default;

RsaKeyPair RsaKeyPair::generate(unsigned bits) {
    auto key = std::make_unique<Key>();
    key->pkey = EVP_RSA_gen(bits);
    if (!key->pkey) throw std::runtime_error("auth: RSA key generation failed: " + openssl_error());
    unsigned char* der = nullptr;
    const int length = i2d_PUBKEY(key->pkey, &der);
    if (length <= 0) throw std::runtime_error("auth: public key encoding failed");
    ByteVector out(reinterpret_cast<std::byte*>(der), reinterpret_cast<std::byte*>(der) + length);
    OPENSSL_free(der);
    return RsaKeyPair(std::move(key), std::move(out));
}

std::expected<ByteVector, std::string> RsaKeyPair::decrypt(std::span<const std::byte> data) const {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(key_->pkey, nullptr);
    if (!ctx) return std::unexpected("context");
    std::expected<ByteVector, std::string> result = std::unexpected("RSA decryption failed");
    std::size_t length = 0;
    if (EVP_PKEY_decrypt_init(ctx) == 1 && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1
        && EVP_PKEY_decrypt(ctx, nullptr, &length, bytes(data), data.size()) == 1) {
        ByteVector out(length);
        if (EVP_PKEY_decrypt(ctx, reinterpret_cast<unsigned char*>(out.data()), &length, bytes(data), data.size()) == 1) {
            out.resize(length);
            result = std::move(out);
        }
    }
    EVP_PKEY_CTX_free(ctx);
    ERR_clear_error();
    return result;
}

std::expected<ByteVector, std::string> rsa_encrypt(std::span<const std::byte> public_der, std::span<const std::byte> data) {
    const unsigned char* p = bytes(public_der);
    EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &p, static_cast<long>(public_der.size()));
    if (!pkey) return std::unexpected("invalid public key");
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    std::expected<ByteVector, std::string> result = std::unexpected("RSA encryption failed");
    std::size_t length = 0;
    if (ctx && EVP_PKEY_encrypt_init(ctx) == 1 && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1
        && EVP_PKEY_encrypt(ctx, nullptr, &length, bytes(data), data.size()) == 1) {
        ByteVector out(length);
        if (EVP_PKEY_encrypt(ctx, reinterpret_cast<unsigned char*>(out.data()), &length, bytes(data), data.size()) == 1) {
            out.resize(length);
            result = std::move(out);
        }
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return result;
}

struct Cfb8::State {
    EVP_CIPHER_CTX* ctx{};
    ~State() { EVP_CIPHER_CTX_free(ctx); }
};

Cfb8::Cfb8(std::span<const std::byte> secret, Direction direction) : state_(std::make_unique<State>()) {
    if (secret.size() != 16) throw std::invalid_argument("auth: the shared secret must be 16 bytes");
    state_->ctx = EVP_CIPHER_CTX_new();
    if (!state_->ctx
        || EVP_CipherInit_ex(state_->ctx, EVP_aes_128_cfb8(), nullptr, bytes(secret), bytes(secret),
                             direction == Direction::encrypt ? 1 : 0)
               != 1)
        throw std::runtime_error("auth: cipher setup failed: " + openssl_error());
}
Cfb8::Cfb8(Cfb8&&) noexcept = default;
Cfb8& Cfb8::operator=(Cfb8&&) noexcept = default;
Cfb8::~Cfb8() = default;

void Cfb8::apply(std::span<std::byte> data) {
    if (data.empty()) return;
    int out = 0;
    auto* p = reinterpret_cast<unsigned char*>(data.data());
    if (EVP_CipherUpdate(state_->ctx, p, &out, p, static_cast<int>(data.size())) != 1 || out != static_cast<int>(data.size()))
        throw std::runtime_error("auth: cipher update failed");
}

std::string minecraft_hex_digest(std::span<const std::byte> data) {
    unsigned char digest[20];
    unsigned int length = 0;
    EVP_Digest(bytes(data), data.size(), digest, &length, EVP_sha1(), nullptr);
    const bool negative = (digest[0] & 0x80) != 0;
    if (negative) { // two's complement negation of the 160-bit value
        bool carry = true;
        for (int i = 19; i >= 0; --i) {
            digest[i] = static_cast<unsigned char>(~digest[i]);
            if (carry) {
                carry = digest[i] == 0xFF;
                digest[i] = static_cast<unsigned char>(digest[i] + 1);
            }
        }
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const auto b : digest) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    const auto first = out.find_first_not_of('0');
    out = first == std::string::npos ? "0" : out.substr(first);
    return negative ? "-" + out : out;
}

std::string login_hash(std::string_view server_id, std::span<const std::byte> secret, std::span<const std::byte> public_der) {
    ByteVector data;
    const auto id = std::as_bytes(std::span(server_id.data(), server_id.size()));
    data.insert(data.end(), id.begin(), id.end());
    data.insert(data.end(), secret.begin(), secret.end());
    data.insert(data.end(), public_der.begin(), public_der.end());
    return minecraft_hex_digest(data);
}

} // namespace circuit::auth
