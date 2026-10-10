// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Reading ZIP archives (data packs, the official server jar) from memory,
// written from the public format description (PKWARE APPNOTE.TXT 6.3.10):
// the end of central directory record, the central directory, local file
// headers, and entries stored or compressed with deflate. Encryption, ZIP64
// and multi-disk archives are refused.
namespace circuit::data {

struct ZipEntry {
    std::string name;
    std::uint16_t method{};          // 0 stored, 8 deflate
    std::uint32_t compressed_size{};
    std::uint32_t size{};
    std::uint32_t crc32{};
    std::uint32_t local_header{};
};

class ZipArchive {
public:
    // Reads the central directory of `bytes`, which must outlive the archive.
    static std::expected<ZipArchive, std::string> open(std::span<const std::byte> bytes);

    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const ZipEntry* find(std::string_view name) const noexcept;
    // The uncompressed contents of an entry, checked against its size and
    // CRC-32; refused above `max_size` bytes.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> read(const ZipEntry& entry,
                                                                          std::size_t max_size = std::size_t{256} << 20) const;

private:
    std::span<const std::byte> bytes_;
    std::vector<ZipEntry> entries_;
};

} // namespace circuit::data
