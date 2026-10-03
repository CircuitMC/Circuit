// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace circuit::storage {

using tick_id = std::uint64_t;
using digest256 = std::array<std::byte, 32>;

enum class error {
    unavailable, invalid_manifest, unsupported_schema, checksum_mismatch,
    generator_mismatch, incomplete_checkpoint, io_failure, incompatible_vanilla
};

struct chunk_key {
    std::string dimension;
    std::int32_t x;
    std::int32_t z;
};

// Regeneration requires the exact game/generator/registry/settings/seed identity.
// A different implementation or data pack must not silently serve as a delta base.
struct generator_fingerprint {
    std::string game_version;
    std::string generator_id;
    std::string generator_build;
    digest256 seed_hash;
    digest256 settings_and_datapacks_hash;
    digest256 registry_hash;
};

struct full_snapshot {
    std::vector<std::byte> encoded_chunk;
};

struct generated_delta {
    generator_fingerprint generator;
    digest256 regenerated_base_hash;
    // Opaque draft payload must eventually cover blocks, biomes, entities,
    // block entities, scheduled ticks and other persistent state, not just blocks.
    std::vector<std::byte> encoded_changes;
};

struct chunk_record {
    chunk_key key;
    std::uint32_t schema_version;
    tick_id checkpoint_tick;
    digest256 canonical_content_hash;
    std::variant<full_snapshot, generated_delta> content;
};

struct manifest_entry {
    chunk_key key;
    digest256 record_hash;
    digest256 canonical_content_hash;
};

struct checkpoint_manifest {
    std::uint32_t schema_version;
    std::string world_id;
    std::string game_version;
    tick_id tick;
    std::optional<digest256> parent_manifest_hash;
    digest256 global_state_hash;
    std::vector<manifest_entry> chunks;
};

// Contract only: no serialization, compression or on-disk format is implemented.
// Commit must atomically publish a complete manifest after durable records and
// global state. Load must verify schema/hash/tick and reject missing delta bases.
// Delta is optional: incompatible or non-reproducible bases require full snapshots.
class backend {
public:
    virtual ~backend() = default;
    virtual std::expected<std::optional<checkpoint_manifest>, error>
    latest_checkpoint() = 0;
    virtual std::expected<std::optional<chunk_record>, error>
    load_chunk(const checkpoint_manifest& checkpoint, const chunk_key& key) = 0;
    virtual std::expected<void, error>
    commit(const checkpoint_manifest& manifest, std::span<const chunk_record> records,
           std::span<const std::byte> global_state) = 0;
};

// Conversion must use a frozen checkpoint, preserve a source copy and report
// unsupported data. Import/export capability is planned, not implemented here.
class vanilla_converter {
public:
    virtual ~vanilla_converter() = default;
    virtual std::expected<void, error>
    import_world(const std::filesystem::path& source, backend& destination) = 0;
    virtual std::expected<void, error>
    export_world(backend& source, const checkpoint_manifest& checkpoint,
                 const std::filesystem::path& destination) = 0;
};

} // namespace circuit::storage
