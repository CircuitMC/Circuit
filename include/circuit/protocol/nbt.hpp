// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/codec.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Named Binary Tag, written from the public format description (minecraft.wiki
// "NBT format"). Network NBT (protocol, since 1.20.2) has a nameless root;
// disk NBT (region files) names its root. Strings use Java's modified UTF-8.
namespace circuit::protocol::nbt {

enum class TagType : std::uint8_t {
    end = 0, byte_ = 1, short_ = 2, int_ = 3, long_ = 4, float_ = 5, double_ = 6,
    byte_array = 7, string = 8, list = 9, compound = 10, int_array = 11, long_array = 12,
};

class Tag {
public:
    Tag() = default;
    static Tag byte(std::int8_t v) { return integral(TagType::byte_, v); }
    static Tag boolean(bool v) { return byte(v ? 1 : 0); }
    static Tag short_(std::int16_t v) { return integral(TagType::short_, v); }
    static Tag int_(std::int32_t v) { return integral(TagType::int_, v); }
    static Tag long_(std::int64_t v) { return integral(TagType::long_, v); }
    static Tag float_(float v);
    static Tag double_(double v);
    static Tag string(std::string v);
    static Tag byte_array(std::vector<std::int8_t> v);
    static Tag int_array(std::vector<std::int32_t> v);
    static Tag long_array(std::vector<std::int64_t> v);
    static Tag list(TagType element);
    static Tag compound();

    [[nodiscard]] TagType type() const noexcept { return type_; }
    [[nodiscard]] std::int64_t integer() const noexcept { return integer_; }
    [[nodiscard]] double real() const noexcept { return real_; }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const std::vector<std::int64_t>& array() const noexcept { return array_; }
    [[nodiscard]] TagType element() const noexcept { return element_; }
    [[nodiscard]] const std::vector<Tag>& items() const noexcept { return items_; }
    [[nodiscard]] const std::vector<std::string>& keys() const noexcept { return keys_; }

    // Compound access. `set` replaces an existing key.
    Tag& set(std::string key, Tag value);
    [[nodiscard]] const Tag* find(std::string_view key) const noexcept;
    // List access. Precondition: the item has the list's element type (an
    // empty list of type end adopts the first item's type).
    Tag& push(Tag value);

    bool operator==(const Tag&) const = default;

private:
    static Tag integral(TagType t, std::int64_t v) {
        Tag tag;
        tag.type_ = t;
        tag.integer_ = v;
        return tag;
    }
    TagType type_{TagType::end};
    std::int64_t integer_{};
    double real_{};
    std::string text_;
    std::vector<std::int64_t> array_;
    TagType element_{TagType::end};
    std::vector<Tag> items_;
    std::vector<std::string> keys_;
};

struct Limits {
    std::size_t max_depth{512};
    std::size_t max_bytes{2 * 1024 * 1024};
};

void write_network(Writer& w, const Tag& root);
void write_named(Writer& w, std::string_view name, const Tag& root);
Decoded<Tag> read_network(Reader& r, const Limits& limits = {});
// Returns the root and fills `name` with its name.
Decoded<Tag> read_named(Reader& r, std::string& name, const Limits& limits = {});

// The same tag with every compound's keys in order: equal tags, whatever
// order their keys were written in, encode to equal bytes.
Tag canonical(const Tag& tag);

// Modified UTF-8 (Java DataOutput.writeUTF) conversion of standard UTF-8.
[[nodiscard]] std::string to_modified_utf8(std::string_view utf8);
[[nodiscard]] Decoded<std::string> from_modified_utf8(std::span<const std::byte> bytes);

// A plain text component as sent in protocol text fields (a string tag).
inline Tag text(std::string s) { return Tag::string(std::move(s)); }
// A text component with a color, e.g. "red" or "gray".
Tag colored_text(std::string s, std::string_view color);

} // namespace circuit::protocol::nbt
