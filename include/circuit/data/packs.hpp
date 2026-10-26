// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/version.hpp>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Data packs: the built-in pack of the official server jar an operator
// provides, and data-pack directories and archives, loaded in precedence
// order. Circuit ships no game data: the jar is the operator's own copy,
// checked against the SHA-1 the version bundle records and against the
// version it declares. Files of a later pack replace those of an earlier one
// with the same path; tag files merge unless a later one sets "replace".
// Formats follow minecraft.wiki "Data pack" and "Tag (Java Edition)".
namespace circuit::data {

struct PackOptions {
    std::filesystem::path vanilla_jar;          // empty: no built-in data
    std::vector<std::filesystem::path> packs;   // directories or .zip archives, lowest precedence first
};

struct PackInfo {
    std::string name;   // "vanilla", or the file or directory name
    std::string origin; // the path it was read from
    std::string sha256; // of the archive, or of a directory's files in path order
    std::size_t files{};
};

// Whether `entry` is in `registry` ("block", "item", "worldgen/biome"), or
// nullopt when Circuit cannot list that registry: its tags are then not
// loaded, and a note says so.
using RegistryLookup = std::function<std::optional<bool>(std::string_view registry, std::string_view entry)>;

class DataPacks {
public:
    static std::expected<DataPacks, std::string> load(const contracts::VersionBundle& bundle, const PackOptions& options,
                                                      const RegistryLookup& registries);

    [[nodiscard]] const std::vector<PackInfo>& packs() const noexcept { return packs_; }
    [[nodiscard]] bool empty() const noexcept { return packs_.empty(); }
    // Identity of the loaded data: "none", or "data-" and 16 hex digits of
    // SHA-256 over the packs' names and digests in order.
    [[nodiscard]] const std::string& fingerprint() const noexcept { return fingerprint_; }
    // Flattened tags: registry -> tag ("minecraft:mineable/pickaxe") -> entries, sorted.
    [[nodiscard]] const std::map<std::string, std::map<std::string, std::vector<std::string>>>& tags() const noexcept { return tags_; }
    [[nodiscard]] const std::vector<std::string>* tag(std::string_view registry, std::string_view name) const;
    // The same entries in the order the tag files list them (each at its first place).
    [[nodiscard]] const std::vector<std::string>* ordered_tag(std::string_view registry, std::string_view name) const;
    // Other resources by kind ("loot_table", "recipe", "enchantment"): id -> JSON text.
    [[nodiscard]] const std::map<std::string, std::string>* resources(std::string_view kind) const;
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }

private:
    std::vector<PackInfo> packs_;
    std::string fingerprint_{"none"};
    std::map<std::string, std::map<std::string, std::vector<std::string>>> tags_;
    std::map<std::string, std::map<std::string, std::vector<std::string>>> ordered_tags_;
    std::map<std::string, std::map<std::string, std::string>> resources_;
    std::vector<std::string> notes_;
};

// The tag registries Circuit reads: tag directories under data/<namespace>/tags/.
std::span<const std::string_view> tag_registries() noexcept;

} // namespace circuit::data
