// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <circuit/protocol/components.hpp>
#include <circuit/protocol/nbt.hpp>
#include <optional>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Import and export of Java Edition worlds in the Anvil format, written from
// the public format descriptions (minecraft.wiki "Region file format", "Chunk
// format", "Entity format", "Block entity format" and "Java Edition level
// format", revisions of August 2025 that describe 1.21.8).
//
// Supported: the overworld of a 1.21.8 world (DataVersion 4440): blocks the
// reference rules model, chests, hoppers, comparators and moving pistons with
// their data, item entities, and scheduled block and fluid ticks. Everything
// else is unsupported content. By default an import that meets unsupported
// content fails and lists it; the explicit substitution policy replaces
// unsupported blocks by a stand-in of the same coarse shape (air, stone,
// glass or obsidian) and drops unsupported block entities, items and
// entities, reporting each kind. The source world is only read.
namespace circuit::server::anvil {

inline constexpr std::int32_t supported_data_version = 4440; // Java Edition 1.21.8

// The data the block entity of a registry state's block saves when the block
// has just been placed, as both official servers write it (`release` "1.21.8"
// or "1.21.4"; checked against their reports for every block): its id
// (registry::block_entity_type), from 1.21.5 an empty component map, and the
// empty fields of its type. A block without rules of its own that a player
// places keeps an empty block entity, written out as this.
protocol::nbt::Tag default_block_entity(std::uint32_t state, std::string_view release);

enum class Unsupported : std::uint8_t {
    reject,     // fail and report every kind of unsupported content
    substitute, // replace or drop it, and report what was changed
};

struct ImportOptions {
    Unsupported unsupported{Unsupported::reject};
    std::size_t max_chunks{65536};
    std::size_t max_chunk_bytes{std::size_t{64} << 20}; // uncompressed NBT per chunk
    // What item components may refer to: the registries the server will send
    // (with data packs, enchantments too). Absent: the bundle's public set.
    std::optional<protocol::ComponentContext> components{};
};

struct ImportReport {
    std::string level_name;
    std::uint64_t seed{};
    std::uint64_t time{};
    contracts::BlockPos spawn{}; // the block players stand on
    std::size_t chunks{}, blocks{}, block_entities{}, entities{}, scheduled_ticks{};
    // Unsupported content by kind ("block minecraft:dirt", "entity
    // minecraft:pig", ...) and count. With the reject policy a non-empty map
    // means the import failed.
    std::map<std::string, std::size_t> unsupported;
    std::vector<std::string> notes; // data that is ignored or regenerated
};

struct ImportResult {
    gameplay::WorldDump state; // all columns unloaded, no tickets
    ImportReport report;
};

// Reads the world at `source`. Fails for a world of another DataVersion, a
// malformed file, or (with the reject policy) unsupported content; the error
// then names the content kinds.
std::expected<ImportResult, std::string> import_world(const std::filesystem::path& source, const contracts::VersionBundle& bundle,
                                                      const ImportOptions& options = {});

struct ExportOptions {
    std::string level_name{"Circuit world"};
    std::uint64_t seed{};
    std::int32_t floor_y{-61};          // the flat generator layer for chunks the game creates later
    contracts::BlockPos spawn{0, -61, 0}; // the block players stand on
    std::optional<protocol::ComponentContext> components{}; // as for import
};

struct ExportReport {
    std::size_t chunks{}, regions{}, block_entities{}, entities{}, scheduled_ticks{};
    std::vector<std::string> notes;
};

// Writes `state` as a new world at `target`, which must not exist. Fails
// without leaving a partial world in place: the world is written to a
// temporary sibling directory and renamed at the end.
std::expected<ExportReport, std::string> export_world(const gameplay::WorldDump& state, const contracts::VersionBundle& bundle,
                                                      const std::filesystem::path& target, const ExportOptions& options = {});

} // namespace circuit::server::anvil

namespace circuit::server {
struct ServerConfig;

// Imports the Anvil world at `source` as a new world in the configured world
// directory, which must not hold a world yet. New columns are generated with
// the configured floor height.
std::expected<anvil::ImportReport, std::string> import_anvil_world(const std::filesystem::path& source, const ServerConfig& config,
                                                                   const anvil::ImportOptions& options = {});
// Exports the latest durable state of the configured world directory as a new
// Anvil world at `target`. The world directory is only read.
std::expected<anvil::ExportReport, std::string> export_anvil_world(const ServerConfig& config, const std::filesystem::path& target);

// Copies the latest durable state of the world at `source` into a new world
// directory at `target` that belongs to `bundle_id` and its rule set. Between
// bundles of the same rule set, the logged ticks are replayed first. A world
// of another rule set (an earlier release's) migrates only when its log holds
// no tick after its last checkpoint: the state carries over and the new rules
// apply from then on. The source is only read. Returns the tick of the
// migrated state.
std::expected<std::uint64_t, std::string> migrate_world(const std::filesystem::path& source, const std::filesystem::path& target,
                                                        std::string_view bundle_id);
} // namespace circuit::server
