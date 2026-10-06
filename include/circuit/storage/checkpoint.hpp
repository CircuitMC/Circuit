// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/world_types.hpp>
#include <cstddef>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <vector>

// Checkpoint files: the canonical state of an engine between ticks, with the
// identity needed to continue it. Format version 1:
//   "CIRCKPT\0", u32 version, bundle id, rule-set id, u64 seed, u64 tick,
//   u64 raw length, u64 compressed length, zlib(body), SHA-256 of all before.
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

std::vector<std::byte> encode_checkpoint(const Checkpoint& checkpoint);
std::expected<Checkpoint, std::string> decode_checkpoint(std::span<const std::byte> bytes, const CheckpointLimits& limits = {});

// Lowercase hexadecimal SHA-256.
std::string sha256_hex(std::span<const std::byte> bytes);

} // namespace circuit::storage
