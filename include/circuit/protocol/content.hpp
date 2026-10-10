// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Numeric content ids of one protocol version: the global block-state
// palette, item ids and entity-type ids. Compiled by
// tools/compile_content_tables.py from a pinned public dataset; see
// docs/PROTOCOL.md for provenance.
namespace circuit::protocol {

using Property = std::pair<std::string, std::string>;

// Coarse physical class of a block, from the dataset: no collision,
// unbreakable, a full light-blocking solid, or any other solid.
enum class BlockShape : std::uint8_t { empty, unbreakable, opaque, translucent };

struct BlockStateInfo {
    std::string_view block;          // e.g. "minecraft:lever"
    std::vector<Property> properties; // every property, in registry order
};

class ContentTables {
public:
    static const ContentTables* for_protocol(std::int32_t protocol) noexcept;

    [[nodiscard]] std::int32_t protocol() const noexcept;
    [[nodiscard]] std::string_view source_commit() const noexcept;
    [[nodiscard]] std::int32_t state_count() const noexcept;
    // Entries of the block and item registries.
    [[nodiscard]] std::uint32_t block_count() const noexcept;
    [[nodiscard]] std::uint32_t item_count() const noexcept;

    // State id of a block with the given properties; properties not listed take
    // the block's default value. nullopt for unknown blocks, properties or values.
    [[nodiscard]] std::optional<std::int32_t> state_id(std::string_view block, const std::vector<Property>& properties) const;
    // Same, from notation such as "minecraft:repeater[delay=2,facing=north]".
    [[nodiscard]] std::optional<std::int32_t> state_id(std::string_view notation) const;
    [[nodiscard]] std::optional<std::int32_t> default_state(std::string_view block) const;
    // Block registry id ("minecraft:stone" -> 1), for tags.
    [[nodiscard]] std::optional<std::int32_t> block_id(std::string_view block) const;
    [[nodiscard]] std::optional<BlockShape> shape(std::string_view block) const;
    [[nodiscard]] std::optional<BlockStateInfo> state(std::int32_t id) const;
    // Full notation of a state id, with every property.
    [[nodiscard]] std::optional<std::string> notation(std::int32_t id) const;

    [[nodiscard]] std::optional<std::int32_t> item_id(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> item_name(std::int32_t id) const;
    [[nodiscard]] std::optional<std::int32_t> entity_type(std::string_view name) const;

    struct Table;
    explicit ContentTables(const Table& table);
    ~ContentTables();
    ContentTables(const ContentTables&) = delete;
    ContentTables& operator=(const ContentTables&) = delete;

private:
    struct Index;
    const Table& table_;
    Index* index_;
};

// Splits "ns:block[k=v,...]" into the block id and its properties.
std::optional<std::pair<std::string, std::vector<Property>>> parse_notation(std::string_view notation);

} // namespace circuit::protocol
