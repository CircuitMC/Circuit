// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/world_types.hpp>
#include <cstddef>
#include <expected>
#include <array>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <string>
#include <vector>

// Checkpoint files: the canonical state of an engine between ticks, with the
// identity needed to continue it. Format version 6 (versions 1 to 5 are read;
// see checkpoint.cpp for their differences):
//   "CIRCKPT\0", u32 version, bundle id, rule-set id, block registry id,
//   u64 seed, u64 tick, u64 raw length, u64 compressed length, zlib(body),
//   SHA-256 of all before.
// Block states are stored in the block registry's encoding; decoding refuses
// a checkpoint of another registry.
// The body holds columns with palette-packed sections, block entities,
// entities, scheduled ticks, block events, tickets, counters and named extras
// (for example the server's player data). Decoding checks every length and
// the digest; Engine's restoring constructor checks the semantics.
namespace circuit::storage {

struct Checkpoint {
    std::string bundle_id;
    std::string rules_id;
    std::uint64_t seed{};
    gameplay::WorldDump state;
    std::map<std::string, std::vector<std::byte>> extras;
    bool operator==(const Checkpoint&) const = default;
};

struct CheckpointLimits {
    std::size_t max_bytes{std::size_t{1} << 31};
    std::size_t max_raw_bytes{std::size_t{1} << 33};
};

struct CheckpointOptions {
    int compression_level{1}; // zlib level 0-9 (see docs/STORAGE_EVALUATION.md)
};

std::vector<std::byte> encode_checkpoint(const Checkpoint& checkpoint, const CheckpointOptions& options = {});
// The uncompressed body, for measurements.
std::vector<std::byte> checkpoint_body(const Checkpoint& checkpoint);

// A column's sections, bottom to top; null entries are empty.
using SectionBlocks = std::array<gameplay::BlockState, 4096>;
using ColumnSections = std::vector<std::shared_ptr<const SectionBlocks>>;

// Remembers the encoding of sections across checkpoints. A section is
// identified by the shared, immutable object itself, which the cache keeps
// alive; sections that a checkpoint no longer contains are dropped.
class SectionCache {
public:
    SectionCache();
    ~SectionCache();
    SectionCache(const SectionCache&) = delete;
    SectionCache& operator=(const SectionCache&) = delete;
    [[nodiscard]] std::size_t size() const noexcept;
    // Sections dropped by the last encoding. A caller whose sections are
    // shared copy-on-write with a running engine releases them on the engine's
    // thread, between ticks, as with snapshots.
    std::vector<std::shared_ptr<const SectionBlocks>> take_dropped();

private:
    struct Entry;
    std::unordered_map<const SectionBlocks*, Entry> entries_;
    std::vector<std::shared_ptr<const SectionBlocks>> dropped_;
    std::uint64_t round_{};
    friend std::vector<std::byte> encode_checkpoint(const Checkpoint&, std::span<const ColumnSections>, std::int32_t, SectionCache*,
                                                    const CheckpointOptions&);
};

// Encodes a checkpoint whose blocks come from column sections (one entry per
// column of `checkpoint.state.chunks`, whose block list must be empty), as an
// engine snapshot provides them. The result is byte-identical to encoding the
// equivalent dump. With a cache, unchanged sections are not encoded again.
std::vector<std::byte> encode_checkpoint(const Checkpoint& checkpoint, std::span<const ColumnSections> columns, std::int32_t min_y,
                                         SectionCache* cache, const CheckpointOptions& options = {});
std::expected<Checkpoint, std::string> decode_checkpoint(std::span<const std::byte> bytes, const CheckpointLimits& limits = {});

// Lowercase hexadecimal SHA-256.
std::string sha256_hex(std::span<const std::byte> bytes);

} // namespace circuit::storage
