// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/codec.hpp>
#include <optional>

// Packet framing (minecraft.wiki "Java Edition protocol/Packets", Packet
// format). A frame is a VarInt length followed by the packet. After Set
// Compression, the packet is a VarInt uncompressed length (0: not compressed)
// followed by the data, zlib-compressed when the length is non-zero.
namespace circuit::protocol {

struct FrameLimits {
    std::size_t max_frame{2097151};       // a length VarInt of at most three bytes
    std::size_t max_uncompressed{8388608}; // 2^23
    std::size_t max_buffered{4 * 2097151}; // unread input kept per connection
};

class FrameDecoder {
public:
    explicit FrameDecoder(FrameLimits limits = {}) : limits_(limits) {}
    // A negative threshold disables compression.
    void set_compression(std::int32_t threshold) noexcept { threshold_ = threshold; }
    // Fails with too_large when the unread input would exceed its bound.
    std::expected<void, CodecError> feed(std::span<const std::byte> bytes);
    // The next complete packet (VarInt id followed by its fields), nullopt when
    // more input is needed, or an error that must close the connection.
    std::expected<std::optional<Bytes>, CodecError> next();
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - read_; }
    // Applies `f` to the unread input in place, e.g. to decrypt bytes that
    // arrived together with the packet that enabled encryption.
    template <class F>
    void transform_unread(F&& f) {
        f(std::span<std::byte>(buffer_).subspan(read_));
    }

private:
    FrameLimits limits_;
    std::int32_t threshold_{-1};
    Bytes buffer_;
    std::size_t read_{};
};

// Frames one packet. With compression enabled, packets of at least
// `threshold` bytes are compressed.
Bytes encode_frame(std::span<const std::byte> packet, std::int32_t threshold = -1);

// Splits a packet into its id and a reader positioned at its fields.
struct PacketView {
    std::int32_t id;
    Reader fields;
};
Decoded<PacketView> open_packet(std::span<const std::byte> packet);

// zlib helpers; `inflate_exact` fails unless the stream expands to exactly `size` bytes.
Bytes deflate_bytes(std::span<const std::byte> data);
Decoded<Bytes> inflate_exact(std::span<const std::byte> data, std::size_t size);

} // namespace circuit::protocol
