// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/protocol/framing.hpp>
#include <stdexcept>
#include <zlib.h>

namespace circuit::protocol {

std::expected<void, CodecError> FrameDecoder::feed(std::span<const std::byte> bytes) {
    if (read_ > 0 && read_ >= buffer_.size() / 2) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_));
        read_ = 0;
    }
    if (buffered() + bytes.size() > limits_.max_buffered) return std::unexpected(CodecError::too_large);
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return {};
}

std::expected<std::optional<Bytes>, CodecError> FrameDecoder::next() {
    Reader r(std::span<const std::byte>(buffer_).subspan(read_));
    const auto length = r.varint();
    if (!length) {
        if (length.error() == CodecError::incomplete) {
            // A frame length longer than three bytes cannot be valid.
            if (r.remaining() == 0 && buffered() >= 3) return std::unexpected(CodecError::too_large);
            return std::optional<Bytes>{};
        }
        return std::unexpected(length.error());
    }
    if (*length <= 0) return std::unexpected(CodecError::malformed);
    if (static_cast<std::size_t>(*length) > limits_.max_frame) return std::unexpected(CodecError::too_large);
    if (r.remaining() < static_cast<std::size_t>(*length)) return std::optional<Bytes>{};
    const auto frame = *r.raw(static_cast<std::size_t>(*length));
    read_ += r.position_in_input();

    if (threshold_ < 0) return std::optional<Bytes>{Bytes(frame.begin(), frame.end())};
    Reader c(frame);
    const auto size = c.varint();
    if (!size) return std::unexpected(CodecError::malformed);
    if (*size == 0) {
        // Uncompressed packets must be shorter than the threshold.
        if (c.remaining() >= static_cast<std::size_t>(threshold_)) return std::unexpected(CodecError::malformed);
        const auto rest = c.rest();
        return std::optional<Bytes>{Bytes(rest.begin(), rest.end())};
    }
    if (*size < 0 || *size < threshold_) return std::unexpected(CodecError::malformed);
    if (static_cast<std::size_t>(*size) > limits_.max_uncompressed) return std::unexpected(CodecError::too_large);
    auto data = inflate_exact(c.rest(), static_cast<std::size_t>(*size));
    if (!data) return std::unexpected(data.error());
    return std::optional<Bytes>{std::move(*data)};
}

Bytes encode_frame(std::span<const std::byte> packet, std::int32_t threshold) {
    Writer body;
    if (threshold < 0) {
        body.raw(packet);
    } else if (packet.size() >= static_cast<std::size_t>(threshold)) {
        body.varint(static_cast<std::int32_t>(packet.size()));
        body.raw(deflate_bytes(packet));
    } else {
        body.varint(0);
        body.raw(packet);
    }
    if (body.size() > FrameLimits{}.max_frame) throw std::length_error("protocol: frame exceeds the maximum size");
    Writer out;
    out.varint(static_cast<std::int32_t>(body.size()));
    out.raw(body.bytes());
    return out.take();
}

Decoded<PacketView> open_packet(std::span<const std::byte> packet) {
    Reader r(packet);
    const auto id = r.varint();
    if (!id) return std::unexpected(CodecError::malformed);
    if (*id < 0) return std::unexpected(CodecError::malformed);
    return PacketView{*id, Reader(r.rest())};
}

Bytes deflate_bytes(std::span<const std::byte> data) {
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    Bytes out(size);
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &size, reinterpret_cast<const Bytef*>(data.data()),
                  static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION)
        != Z_OK)
        throw std::runtime_error("protocol: compression failed");
    out.resize(size);
    return out;
}

Decoded<Bytes> inflate_exact(std::span<const std::byte> data, std::size_t size) {
    Bytes out(size);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw std::runtime_error("protocol: inflate init failed");
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    const auto rc = inflate(&z, Z_FINISH);
    const auto produced = out.size() - z.avail_out;
    const bool all_input = z.avail_in == 0;
    inflateEnd(&z);
    // The stream must end exactly at the declared size, using all its input.
    if (rc != Z_STREAM_END || produced != size || !all_input) return std::unexpected(CodecError::malformed);
    return out;
}

} // namespace circuit::protocol
