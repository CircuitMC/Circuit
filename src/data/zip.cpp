// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/data/zip.hpp>
#include <algorithm>
#include <format>
#include <zlib.h>

namespace circuit::data {
namespace {
std::uint32_t le(std::span<const std::byte> b, std::size_t at, int n) {
    std::uint32_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | std::to_integer<std::uint8_t>(b[at + static_cast<std::size_t>(i)]);
    return v;
}
constexpr std::uint32_t end_signature = 0x06054b50, central_signature = 0x02014b50, local_signature = 0x04034b50;
} // namespace

std::expected<ZipArchive, std::string> ZipArchive::open(std::span<const std::byte> bytes) {
    // The end of central directory record is within the last 64 KiB + 22 bytes.
    if (bytes.size() < 22) return std::unexpected("not a ZIP archive");
    std::size_t end = bytes.size() - 22;
    const std::size_t lowest = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    while (le(bytes, end, 4) != end_signature) {
        if (end == lowest) return std::unexpected("not a ZIP archive: no end of central directory");
        --end;
    }
    const auto disk = le(bytes, end + 4, 2), cd_disk = le(bytes, end + 6, 2);
    const auto count = le(bytes, end + 10, 2), cd_size = le(bytes, end + 12, 4), cd_offset = le(bytes, end + 16, 4);
    if (disk != 0 || cd_disk != 0 || count == 0xFFFF || cd_offset == 0xFFFFFFFF) return std::unexpected("multi-disk or ZIP64 archive");
    if (static_cast<std::size_t>(cd_offset) + cd_size > end) return std::unexpected("central directory outside the archive");
    ZipArchive zip;
    zip.bytes_ = bytes;
    std::size_t at = cd_offset;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 46 > end || le(bytes, at, 4) != central_signature) return std::unexpected("malformed central directory");
        const auto flags = le(bytes, at + 8, 2);
        ZipEntry e;
        e.method = static_cast<std::uint16_t>(le(bytes, at + 10, 2));
        e.crc32 = le(bytes, at + 16, 4);
        e.compressed_size = le(bytes, at + 20, 4);
        e.size = le(bytes, at + 24, 4);
        const auto name_length = le(bytes, at + 28, 2), extra = le(bytes, at + 30, 2), comment = le(bytes, at + 32, 2);
        e.local_header = le(bytes, at + 42, 4);
        if (flags & 1) return std::unexpected("encrypted archive");
        if (at + 46 + name_length + extra + comment > end) return std::unexpected("malformed central directory");
        e.name.assign(reinterpret_cast<const char*>(bytes.data() + at + 46), name_length);
        zip.entries_.push_back(std::move(e));
        at += 46 + name_length + extra + comment;
    }
    return zip;
}

const ZipEntry* ZipArchive::find(std::string_view name) const noexcept {
    const auto it = std::ranges::find(entries_, name, &ZipEntry::name);
    return it == entries_.end() ? nullptr : &*it;
}

std::expected<std::vector<std::byte>, std::string> ZipArchive::read(const ZipEntry& e, std::size_t max_size) const {
    if (e.size > max_size) return std::unexpected(std::format("{}: larger than {} bytes", e.name, max_size));
    const std::size_t at = e.local_header;
    if (at + 30 > bytes_.size() || le(bytes_, at, 4) != local_signature) return std::unexpected(std::format("{}: no local header", e.name));
    const auto data = at + 30 + le(bytes_, at + 26, 2) + le(bytes_, at + 28, 2);
    if (data + e.compressed_size > bytes_.size()) return std::unexpected(std::format("{}: data outside the archive", e.name));
    const auto in = bytes_.subspan(data, e.compressed_size);
    std::vector<std::byte> out(e.size);
    if (e.method == 0) {
        if (e.compressed_size != e.size) return std::unexpected(std::format("{}: stored sizes differ", e.name));
        std::ranges::copy(in, out.begin());
    } else if (e.method == 8) {
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return std::unexpected("zlib unavailable");
        z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
        z.avail_in = static_cast<uInt>(in.size());
        z.next_out = reinterpret_cast<Bytef*>(out.data());
        z.avail_out = static_cast<uInt>(out.size());
        const int r = inflate(&z, Z_FINISH);
        const auto produced = z.total_out;
        inflateEnd(&z);
        if (r != Z_STREAM_END || produced != e.size) return std::unexpected(std::format("{}: corrupt deflate data", e.name));
    } else {
        return std::unexpected(std::format("{}: compression method {} is not supported", e.name, e.method));
    }
    const auto crc = crc32(0L, reinterpret_cast<const Bytef*>(out.data()), static_cast<uInt>(out.size()));
    if (crc != e.crc32) return std::unexpected(std::format("{}: CRC-32 mismatch", e.name));
    return out;
}

} // namespace circuit::data
