// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Wire data types of the Java Edition protocol, written from the public
// protocol documentation (minecraft.wiki "Java Edition protocol/Data types").
// Everything except VarInt and VarLong is big-endian. Readers are bounded:
// they never read past their input and report why a value is rejected.
namespace circuit::protocol {

enum class CodecError : std::uint8_t {
    incomplete,  // more bytes are needed
    malformed,   // the bytes cannot encode a valid value
    too_large,   // a declared size exceeds its limit
};

template <class T>
using Decoded = std::expected<T, CodecError>;

using Bytes = std::vector<std::byte>;

struct Uuid {
    std::uint64_t high{}, low{};
    auto operator<=>(const Uuid&) const = default;
    [[nodiscard]] std::string text() const; // 8-4-4-4-12 lowercase hex
};

// Block position packed as x (26 bits), z (26 bits), y (12 bits).
struct WirePosition {
    std::int32_t x{}, y{}, z{};
    auto operator<=>(const WirePosition&) const = default;
};
[[nodiscard]] std::int64_t pack_position(WirePosition p) noexcept;
[[nodiscard]] WirePosition unpack_position(std::int64_t v) noexcept;
// True when the position fits the packed ranges (x, z: 26 bits; y: 12 bits).
[[nodiscard]] bool packable(WirePosition p) noexcept;

inline constexpr std::size_t max_string_units = 32767;

class Writer {
public:
    Writer& u8(std::uint8_t v);
    Writer& i8(std::int8_t v) { return u8(static_cast<std::uint8_t>(v)); }
    Writer& boolean(bool v) { return u8(v ? 1 : 0); }
    Writer& u16(std::uint16_t v);
    Writer& i16(std::int16_t v) { return u16(static_cast<std::uint16_t>(v)); }
    Writer& i32(std::int32_t v);
    Writer& i64(std::int64_t v);
    Writer& f32(float v);
    Writer& f64(double v);
    Writer& varint(std::int32_t v);
    Writer& varlong(std::int64_t v);
    // Precondition: valid UTF-8 within `max_units` UTF-16 code units.
    Writer& string(std::string_view v, std::size_t max_units = max_string_units);
    Writer& uuid(Uuid v);
    Writer& position(WirePosition p);
    Writer& angle(float degrees);
    Writer& raw(std::span<const std::byte> v);
    Writer& prefixed_bytes(std::span<const std::byte> v);
    // Bit set as a VarInt-prefixed array of longs.
    Writer& bitset(std::span<const std::uint64_t> words);

    [[nodiscard]] const Bytes& bytes() const noexcept { return out_; }
    [[nodiscard]] Bytes take() noexcept { return std::move(out_); }
    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }

private:
    Bytes out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> in) noexcept : in_(in) {}

    Decoded<std::uint8_t> u8();
    Decoded<std::int8_t> i8();
    Decoded<bool> boolean();
    Decoded<std::uint16_t> u16();
    Decoded<std::int16_t> i16();
    Decoded<std::int32_t> i32();
    Decoded<std::int64_t> i64();
    Decoded<float> f32();
    Decoded<double> f64();
    Decoded<std::int32_t> varint();
    Decoded<std::int64_t> varlong();
    // Checks both the byte bound (3 bytes per unit) and the UTF-16 unit bound.
    Decoded<std::string> string(std::size_t max_units = max_string_units);
    Decoded<std::string> identifier();
    Decoded<Uuid> uuid();
    Decoded<WirePosition> position();
    Decoded<std::span<const std::byte>> raw(std::size_t n);
    Decoded<Bytes> prefixed_bytes(std::size_t max_bytes);
    // A VarInt array length that must not exceed `max` elements nor the bytes
    // left (at least `min_element_bytes` per element).
    Decoded<std::size_t> length(std::size_t max, std::size_t min_element_bytes = 1);

    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - at_; }
    [[nodiscard]] std::size_t position_in_input() const noexcept { return at_; }
    [[nodiscard]] std::span<const std::byte> rest() const noexcept { return in_.subspan(at_); }

private:
    std::span<const std::byte> in_;
    std::size_t at_{};
};

// UTF-16 code units of a UTF-8 string; nullopt-like failure for invalid UTF-8.
[[nodiscard]] std::expected<std::size_t, CodecError> utf16_units(std::string_view utf8) noexcept;
// Identifier syntax: optional "namespace:" then a path, lowercase [a-z0-9._-/].
[[nodiscard]] bool valid_identifier(std::string_view id) noexcept;
[[nodiscard]] std::size_t varint_size(std::int32_t v) noexcept;

inline std::span<const std::byte> as_bytes(std::string_view s) noexcept {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

} // namespace circuit::protocol
