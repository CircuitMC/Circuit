// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/protocol/codec.hpp>
#include <bit>
#include <cmath>
#include <format>

namespace circuit::protocol {

std::string Uuid::text() const {
    const auto h = std::format("{:016x}{:016x}", high, low);
    return std::format("{}-{}-{}-{}-{}", h.substr(0, 8), h.substr(8, 4), h.substr(12, 4), h.substr(16, 4), h.substr(20));
}

std::int64_t pack_position(WirePosition p) noexcept {
    const auto x = static_cast<std::uint64_t>(static_cast<std::int64_t>(p.x)) & 0x3FFFFFFULL;
    const auto z = static_cast<std::uint64_t>(static_cast<std::int64_t>(p.z)) & 0x3FFFFFFULL;
    const auto y = static_cast<std::uint64_t>(static_cast<std::int64_t>(p.y)) & 0xFFFULL;
    return static_cast<std::int64_t>((x << 38) | (z << 12) | y);
}

WirePosition unpack_position(std::int64_t v) noexcept {
    // Arithmetic shifts sign-extend each field (C++20 defines them on signed values).
    const auto x = v >> 38;
    const auto y = static_cast<std::int64_t>(static_cast<std::uint64_t>(v) << 52) >> 52;
    const auto z = static_cast<std::int64_t>(static_cast<std::uint64_t>(v) << 26) >> 38;
    return {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)};
}

bool packable(WirePosition p) noexcept {
    constexpr std::int32_t h26 = 1 << 25, h12 = 1 << 11;
    return p.x >= -h26 && p.x < h26 && p.z >= -h26 && p.z < h26 && p.y >= -h12 && p.y < h12;
}

std::expected<std::size_t, CodecError> utf16_units(std::string_view s) noexcept {
    std::size_t units = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n;
        std::uint32_t cp;
        if (c < 0x80) { n = 1; cp = c; }
        else if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
        else return std::unexpected(CodecError::malformed);
        if (i + n > s.size()) return std::unexpected(CodecError::malformed);
        for (std::size_t k = 1; k < n; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return std::unexpected(CodecError::malformed);
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong forms, surrogates and values beyond U+10FFFF.
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF
            || (cp >= 0xD800 && cp <= 0xDFFF))
            return std::unexpected(CodecError::malformed);
        units += cp > 0xFFFF ? 2 : 1;
        i += n;
    }
    return units;
}

bool valid_identifier(std::string_view id) noexcept {
    if (id.empty() || id.size() > max_string_units) return false;
    const auto colon = id.find(':');
    const auto ns = colon == std::string_view::npos ? std::string_view{} : id.substr(0, colon);
    const auto path = colon == std::string_view::npos ? id : id.substr(colon + 1);
    const auto base = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'; };
    for (char c : ns)
        if (!base(c)) return false;
    if (path.empty()) return false;
    for (char c : path)
        if (!base(c) && c != '/') return false;
    return true;
}

std::size_t varint_size(std::int32_t v) noexcept {
    auto u = static_cast<std::uint32_t>(v);
    std::size_t n = 1;
    while (u >= 0x80) {
        u >>= 7;
        ++n;
    }
    return n;
}

Writer& Writer::u8(std::uint8_t v) {
    out_.push_back(static_cast<std::byte>(v));
    return *this;
}
Writer& Writer::u16(std::uint16_t v) { return u8(static_cast<std::uint8_t>(v >> 8)).u8(static_cast<std::uint8_t>(v)); }
Writer& Writer::i32(std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    for (int s = 24; s >= 0; s -= 8) u8(static_cast<std::uint8_t>(u >> s));
    return *this;
}
Writer& Writer::i64(std::int64_t v) {
    const auto u = static_cast<std::uint64_t>(v);
    for (int s = 56; s >= 0; s -= 8) u8(static_cast<std::uint8_t>(u >> s));
    return *this;
}
Writer& Writer::f32(float v) { return i32(std::bit_cast<std::int32_t>(v)); }
Writer& Writer::f64(double v) { return i64(std::bit_cast<std::int64_t>(v)); }
Writer& Writer::varint(std::int32_t v) {
    auto u = static_cast<std::uint32_t>(v);
    while (u >= 0x80) {
        u8(static_cast<std::uint8_t>((u & 0x7F) | 0x80));
        u >>= 7;
    }
    return u8(static_cast<std::uint8_t>(u));
}
Writer& Writer::varlong(std::int64_t v) {
    auto u = static_cast<std::uint64_t>(v);
    while (u >= 0x80) {
        u8(static_cast<std::uint8_t>((u & 0x7F) | 0x80));
        u >>= 7;
    }
    return u8(static_cast<std::uint8_t>(u));
}
Writer& Writer::string(std::string_view v, std::size_t max_units) {
    const auto units = utf16_units(v);
    if (!units || *units > max_units) throw std::length_error("protocol: string exceeds its limit or is not UTF-8");
    varint(static_cast<std::int32_t>(v.size()));
    return raw(as_bytes(v));
}
Writer& Writer::uuid(Uuid v) { return i64(static_cast<std::int64_t>(v.high)).i64(static_cast<std::int64_t>(v.low)); }
Writer& Writer::position(WirePosition p) {
    if (!packable(p)) throw std::out_of_range("protocol: position outside the packed range");
    return i64(pack_position(p));
}
Writer& Writer::angle(float degrees) {
    const auto steps = static_cast<std::int64_t>(std::floor(static_cast<double>(degrees) * 256.0 / 360.0));
    return u8(static_cast<std::uint8_t>(steps & 0xFF));
}
Writer& Writer::raw(std::span<const std::byte> v) {
    out_.insert(out_.end(), v.begin(), v.end());
    return *this;
}
Writer& Writer::prefixed_bytes(std::span<const std::byte> v) {
    varint(static_cast<std::int32_t>(v.size()));
    return raw(v);
}
Writer& Writer::bitset(std::span<const std::uint64_t> words) {
    varint(static_cast<std::int32_t>(words.size()));
    for (const auto w : words) i64(static_cast<std::int64_t>(w));
    return *this;
}

Decoded<std::uint8_t> Reader::u8() {
    if (at_ >= in_.size()) return std::unexpected(CodecError::incomplete);
    return static_cast<std::uint8_t>(in_[at_++]);
}
Decoded<std::int8_t> Reader::i8() {
    const auto v = u8();
    if (!v) return std::unexpected(v.error());
    return static_cast<std::int8_t>(*v);
}
Decoded<bool> Reader::boolean() {
    const auto v = u8();
    if (!v) return std::unexpected(v.error());
    if (*v > 1) return std::unexpected(CodecError::malformed);
    return *v == 1;
}
Decoded<std::uint16_t> Reader::u16() {
    if (remaining() < 2) return std::unexpected(CodecError::incomplete);
    const auto v = static_cast<std::uint16_t>((std::to_integer<unsigned>(in_[at_]) << 8) | std::to_integer<unsigned>(in_[at_ + 1]));
    at_ += 2;
    return v;
}
Decoded<std::int16_t> Reader::i16() {
    const auto v = u16();
    if (!v) return std::unexpected(v.error());
    return static_cast<std::int16_t>(*v);
}
Decoded<std::int32_t> Reader::i32() {
    if (remaining() < 4) return std::unexpected(CodecError::incomplete);
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | std::to_integer<std::uint32_t>(in_[at_++]);
    return static_cast<std::int32_t>(v);
}
Decoded<std::int64_t> Reader::i64() {
    if (remaining() < 8) return std::unexpected(CodecError::incomplete);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(in_[at_++]);
    return static_cast<std::int64_t>(v);
}
Decoded<float> Reader::f32() {
    const auto v = i32();
    if (!v) return std::unexpected(v.error());
    return std::bit_cast<float>(*v);
}
Decoded<double> Reader::f64() {
    const auto v = i64();
    if (!v) return std::unexpected(v.error());
    return std::bit_cast<double>(*v);
}
Decoded<std::int32_t> Reader::varint() {
    std::uint32_t v = 0;
    for (int i = 0; i < 5; ++i) {
        if (at_ >= in_.size()) return std::unexpected(CodecError::incomplete);
        const auto b = std::to_integer<std::uint32_t>(in_[at_++]);
        if (i == 4 && (b & 0xF0) != 0) return std::unexpected(CodecError::malformed); // more than 32 bits
        v |= (b & 0x7F) << (7 * i);
        if ((b & 0x80) == 0) return static_cast<std::int32_t>(v);
    }
    return std::unexpected(CodecError::malformed);
}
Decoded<std::int64_t> Reader::varlong() {
    std::uint64_t v = 0;
    for (int i = 0; i < 10; ++i) {
        if (at_ >= in_.size()) return std::unexpected(CodecError::incomplete);
        const auto b = std::to_integer<std::uint64_t>(in_[at_++]);
        if (i == 9 && (b & 0xFE) != 0) return std::unexpected(CodecError::malformed); // more than 64 bits
        v |= (b & 0x7F) << (7 * i);
        if ((b & 0x80) == 0) return static_cast<std::int64_t>(v);
    }
    return std::unexpected(CodecError::malformed);
}
Decoded<std::string> Reader::string(std::size_t max_units) {
    const auto n = varint();
    if (!n) return std::unexpected(n.error());
    if (*n < 0) return std::unexpected(CodecError::malformed);
    const auto bytes = static_cast<std::size_t>(*n);
    if (bytes > max_units * 3) return std::unexpected(CodecError::too_large);
    if (remaining() < bytes) return std::unexpected(CodecError::incomplete);
    std::string s(reinterpret_cast<const char*>(in_.data() + at_), bytes);
    at_ += bytes;
    const auto units = utf16_units(s);
    if (!units) return std::unexpected(units.error());
    if (*units > max_units) return std::unexpected(CodecError::too_large);
    return s;
}
Decoded<std::string> Reader::identifier() {
    auto s = string(max_string_units);
    if (s && !valid_identifier(*s)) return std::unexpected(CodecError::malformed);
    return s;
}
Decoded<Uuid> Reader::uuid() {
    const auto h = i64();
    if (!h) return std::unexpected(h.error());
    const auto l = i64();
    if (!l) return std::unexpected(l.error());
    return Uuid{static_cast<std::uint64_t>(*h), static_cast<std::uint64_t>(*l)};
}
Decoded<WirePosition> Reader::position() {
    const auto v = i64();
    if (!v) return std::unexpected(v.error());
    return unpack_position(*v);
}
Decoded<std::span<const std::byte>> Reader::raw(std::size_t n) {
    if (remaining() < n) return std::unexpected(CodecError::incomplete);
    const auto s = in_.subspan(at_, n);
    at_ += n;
    return s;
}
Decoded<Bytes> Reader::prefixed_bytes(std::size_t max_bytes) {
    const auto n = length(max_bytes);
    if (!n) return std::unexpected(n.error());
    const auto s = raw(*n);
    if (!s) return std::unexpected(s.error());
    return Bytes(s->begin(), s->end());
}
Decoded<std::size_t> Reader::length(std::size_t max, std::size_t min_element_bytes) {
    const auto n = varint();
    if (!n) return std::unexpected(n.error());
    if (*n < 0) return std::unexpected(CodecError::malformed);
    const auto count = static_cast<std::size_t>(*n);
    if (count > max) return std::unexpected(CodecError::too_large);
    if (min_element_bytes > 0 && count > remaining() / min_element_bytes) return std::unexpected(CodecError::incomplete);
    return count;
}

} // namespace circuit::protocol
