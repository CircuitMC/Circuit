// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Data components of an item stack beyond its damage: the components added to
// the item's defaults and the defaults removed, as one protocol encodes them.
// The protocol layer (protocol/components.hpp) reads, checks and writes them;
// the simulation stores, compares and persists them without interpreting the
// values, except for the few typed fields below.
namespace circuit::contracts {

class ComponentPatch {
public:
    struct Added {
        std::uint16_t type{};          // component type id of the patch's protocol
        std::vector<std::byte> value;  // the value in that protocol's canonical network form
        bool operator==(const Added&) const = default;
    };
    // Values the rules use, read from the components when the patch is made.
    struct Typed {
        std::optional<std::uint8_t> max_stack_size;
        std::optional<std::int32_t> max_damage;
        bool operator==(const Typed&) const = default;
    };

    ComponentPatch() noexcept = default;
    // Precondition: types strictly ascending in `added` and in `removed`, and
    // the values canonical for `protocol`. An empty patch is the default.
    static ComponentPatch make(std::int32_t protocol, std::vector<Added> added, std::vector<std::uint16_t> removed, Typed typed);

    [[nodiscard]] bool empty() const noexcept { return !data_; }
    [[nodiscard]] std::int32_t protocol() const noexcept;
    [[nodiscard]] std::span<const Added> added() const noexcept;
    [[nodiscard]] std::span<const std::uint16_t> removed() const noexcept;
    [[nodiscard]] const Typed& typed() const noexcept;
    // 64-bit FNV-1a of the content; 0 for an empty patch. Stable across runs.
    [[nodiscard]] std::uint64_t hash() const noexcept;
    [[nodiscard]] std::size_t value_bytes() const noexcept;

    bool operator==(const ComponentPatch& o) const noexcept;
    // Content order: by hash, then by content; deterministic.
    std::strong_ordering operator<=>(const ComponentPatch& o) const noexcept;

private:
    struct Data;
    std::shared_ptr<const Data> data_;
};

// Immutable bytes shared between copies and compared by content: data the
// simulation keeps without interpreting it (the saved data of block entities
// of blocks without rules of their own).
class Blob {
public:
    Blob() noexcept = default;
    explicit Blob(std::vector<std::byte> bytes);
    [[nodiscard]] bool empty() const noexcept { return !data_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
    bool operator==(const Blob& o) const noexcept;

private:
    std::shared_ptr<const std::vector<std::byte>> data_;
    std::uint64_t hash_{};
};

// Self-delimiting byte form for records: i32 protocol, u16 added count, each
// (u16 type, u32 length, value), u16 removed count, each u16 type, then the
// typed fields (u8 flags, u8 max stack size, i32 max damage). An empty patch
// is protocol 0 alone.
void write_patch(std::vector<std::byte>& out, const ComponentPatch& patch);
// Reads what `write_patch` wrote from the front of `in`, advancing it. Checks
// the structure only (counts, order and lengths); the protocol layer checks
// the values. nullopt when malformed or beyond `max_bytes` of values.
std::optional<ComponentPatch> read_patch(std::span<const std::byte>& in, std::size_t max_bytes = 1u << 20);

} // namespace circuit::contracts
