// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/protocol/nbt.hpp>
#include <stdexcept>

namespace circuit::protocol::nbt {

Tag Tag::float_(float v) {
    Tag t;
    t.type_ = TagType::float_;
    t.real_ = v;
    return t;
}
Tag Tag::double_(double v) {
    Tag t;
    t.type_ = TagType::double_;
    t.real_ = v;
    return t;
}
Tag Tag::string(std::string v) {
    Tag t;
    t.type_ = TagType::string;
    t.text_ = std::move(v);
    return t;
}
Tag Tag::byte_array(std::vector<std::int8_t> v) {
    Tag t;
    t.type_ = TagType::byte_array;
    t.array_.assign(v.begin(), v.end());
    return t;
}
Tag Tag::int_array(std::vector<std::int32_t> v) {
    Tag t;
    t.type_ = TagType::int_array;
    t.array_.assign(v.begin(), v.end());
    return t;
}
Tag Tag::long_array(std::vector<std::int64_t> v) {
    Tag t;
    t.type_ = TagType::long_array;
    t.array_ = std::move(v);
    return t;
}
Tag Tag::list(TagType element) {
    Tag t;
    t.type_ = TagType::list;
    t.element_ = element;
    return t;
}
Tag Tag::compound() {
    Tag t;
    t.type_ = TagType::compound;
    return t;
}

Tag& Tag::set(std::string key, Tag value) {
    if (type_ != TagType::compound) throw std::logic_error("nbt: set on a non-compound tag");
    for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return items_[i] = std::move(value);
    keys_.push_back(std::move(key));
    items_.push_back(std::move(value));
    return items_.back();
}

const Tag* Tag::find(std::string_view key) const noexcept {
    if (type_ != TagType::compound) return nullptr;
    for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

Tag& Tag::push(Tag value) {
    if (type_ != TagType::list) throw std::logic_error("nbt: push on a non-list tag");
    if (element_ == TagType::end && items_.empty()) element_ = value.type();
    if (value.type() != element_) throw std::logic_error("nbt: list element type mismatch");
    items_.push_back(std::move(value));
    return items_.back();
}

Tag colored_text(std::string s, std::string_view color) {
    auto t = Tag::compound();
    t.set("text", Tag::string(std::move(s)));
    t.set("color", Tag::string(std::string(color)));
    return t;
}

std::string to_modified_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0) { // U+0000 as two bytes
            out += '\xC0';
            out += '\x80';
            ++i;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) { // supplementary: surrogate pair, 3 bytes each
            std::uint32_t cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12)
                | ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            cp -= 0x10000;
            for (const std::uint32_t unit : {0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)}) {
                out += static_cast<char>(0xE0 | (unit >> 12));
                out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (unit & 0x3F));
            }
            i += 4;
        } else {
            out += static_cast<char>(c);
            ++i;
        }
    }
    return out;
}

Decoded<std::string> from_modified_utf8(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(bytes.size());
    const auto at = [&](std::size_t i) { return std::to_integer<std::uint32_t>(bytes[i]); };
    std::uint32_t pending_high = 0;
    const auto emit = [&](std::uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (std::size_t i = 0; i < bytes.size();) {
        const auto c = at(i);
        std::uint32_t unit;
        if (c < 0x80 && c != 0) { unit = c; i += 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < bytes.size() && (at(i + 1) & 0xC0) == 0x80) {
            unit = ((c & 0x1F) << 6) | (at(i + 1) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < bytes.size() && (at(i + 1) & 0xC0) == 0x80 && (at(i + 2) & 0xC0) == 0x80) {
            unit = ((c & 0x0F) << 12) | ((at(i + 1) & 0x3F) << 6) | (at(i + 2) & 0x3F);
            i += 3;
        } else {
            return std::unexpected(CodecError::malformed);
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (pending_high) emit(0xFFFD);
            pending_high = unit;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (!pending_high) { emit(0xFFFD); continue; }
            emit(0x10000 + ((pending_high - 0xD800) << 10) + (unit - 0xDC00));
            pending_high = 0;
            continue;
        }
        if (pending_high) {
            emit(0xFFFD); // Unpaired surrogates are replaced, as Java strings may hold them.
            pending_high = 0;
        }
        emit(unit);
    }
    if (pending_high) emit(0xFFFD);
    return out;
}

namespace {
void write_string(Writer& w, std::string_view s) {
    const auto m = to_modified_utf8(s);
    if (m.size() > 65535) throw std::length_error("nbt: string longer than 65535 bytes");
    w.u16(static_cast<std::uint16_t>(m.size()));
    w.raw(as_bytes(m));
}

void write_payload(Writer& w, const Tag& t) {
    switch (t.type()) {
    case TagType::end: break;
    case TagType::byte_: w.i8(static_cast<std::int8_t>(t.integer())); break;
    case TagType::short_: w.i16(static_cast<std::int16_t>(t.integer())); break;
    case TagType::int_: w.i32(static_cast<std::int32_t>(t.integer())); break;
    case TagType::long_: w.i64(t.integer()); break;
    case TagType::float_: w.f32(static_cast<float>(t.real())); break;
    case TagType::double_: w.f64(t.real()); break;
    case TagType::byte_array:
        w.i32(static_cast<std::int32_t>(t.array().size()));
        for (auto v : t.array()) w.i8(static_cast<std::int8_t>(v));
        break;
    case TagType::string: write_string(w, t.text()); break;
    case TagType::list:
        w.u8(static_cast<std::uint8_t>(t.items().empty() ? TagType::end : t.element()));
        w.i32(static_cast<std::int32_t>(t.items().size()));
        for (const auto& item : t.items()) write_payload(w, item);
        break;
    case TagType::compound:
        for (std::size_t i = 0; i < t.items().size(); ++i) {
            w.u8(static_cast<std::uint8_t>(t.items()[i].type()));
            write_string(w, t.keys()[i]);
            write_payload(w, t.items()[i]);
        }
        w.u8(0);
        break;
    case TagType::int_array:
        w.i32(static_cast<std::int32_t>(t.array().size()));
        for (auto v : t.array()) w.i32(static_cast<std::int32_t>(v));
        break;
    case TagType::long_array:
        w.i32(static_cast<std::int32_t>(t.array().size()));
        for (auto v : t.array()) w.i64(v);
        break;
    }
}

struct Decoder {
    Reader& r;
    Limits limits;
    std::size_t start;

    bool over() const { return r.position_in_input() - start > limits.max_bytes; }

    Decoded<std::string> string() {
        const auto n = r.u16();
        if (!n) return std::unexpected(n.error());
        const auto bytes = r.raw(*n);
        if (!bytes) return std::unexpected(bytes.error());
        return from_modified_utf8(*bytes);
    }

    Decoded<std::size_t> count(std::size_t element_bytes) {
        const auto n = r.i32();
        if (!n) return std::unexpected(n.error());
        if (*n < 0) return std::unexpected(CodecError::malformed);
        const auto c = static_cast<std::size_t>(*n);
        if (element_bytes > 0 && c > r.remaining() / element_bytes) return std::unexpected(CodecError::incomplete);
        if (c * std::max<std::size_t>(element_bytes, 1) > limits.max_bytes) return std::unexpected(CodecError::too_large);
        return c;
    }

    Decoded<Tag> payload(TagType type, std::size_t depth) {
        if (depth > limits.max_depth) return std::unexpected(CodecError::too_large);
        if (over()) return std::unexpected(CodecError::too_large);
        const auto fail = [](CodecError e) { return std::unexpected(e); };
        switch (type) {
        case TagType::end: return std::unexpected(CodecError::malformed);
        case TagType::byte_: { const auto v = r.i8(); if (!v) return fail(v.error()); return Tag::byte(*v); }
        case TagType::short_: { const auto v = r.i16(); if (!v) return fail(v.error()); return Tag::short_(*v); }
        case TagType::int_: { const auto v = r.i32(); if (!v) return fail(v.error()); return Tag::int_(*v); }
        case TagType::long_: { const auto v = r.i64(); if (!v) return fail(v.error()); return Tag::long_(*v); }
        case TagType::float_: { const auto v = r.f32(); if (!v) return fail(v.error()); return Tag::float_(*v); }
        case TagType::double_: { const auto v = r.f64(); if (!v) return fail(v.error()); return Tag::double_(*v); }
        case TagType::string: { auto v = string(); if (!v) return fail(v.error()); return Tag::string(std::move(*v)); }
        case TagType::byte_array: case TagType::int_array: case TagType::long_array: {
            const std::size_t width = type == TagType::byte_array ? 1 : type == TagType::int_array ? 4 : 8;
            const auto n = count(width);
            if (!n) return fail(n.error());
            std::vector<std::int64_t> values(*n);
            for (auto& v : values) {
                if (width == 1) { const auto x = r.i8(); if (!x) return fail(x.error()); v = *x; }
                else if (width == 4) { const auto x = r.i32(); if (!x) return fail(x.error()); v = *x; }
                else { const auto x = r.i64(); if (!x) return fail(x.error()); v = *x; }
            }
            if (type == TagType::long_array) return Tag::long_array(std::move(values));
            if (type == TagType::int_array) return Tag::int_array(std::vector<std::int32_t>(values.begin(), values.end()));
            return Tag::byte_array(std::vector<std::int8_t>(values.begin(), values.end()));
        }
        case TagType::list: {
            const auto element = r.u8();
            if (!element) return fail(element.error());
            if (*element > 12) return fail(CodecError::malformed);
            const auto n = count(1);
            if (!n) return fail(n.error());
            if (*element == 0 && *n > 0) return fail(CodecError::malformed);
            auto list = Tag::list(static_cast<TagType>(*element));
            for (std::size_t i = 0; i < *n; ++i) {
                auto item = payload(static_cast<TagType>(*element), depth + 1);
                if (!item) return fail(item.error());
                list.push(std::move(*item));
            }
            return list;
        }
        case TagType::compound: {
            auto c = Tag::compound();
            for (;;) {
                const auto t = r.u8();
                if (!t) return fail(t.error());
                if (*t == 0) return c;
                if (*t > 12) return fail(CodecError::malformed);
                auto key = string();
                if (!key) return fail(key.error());
                auto value = payload(static_cast<TagType>(*t), depth + 1);
                if (!value) return fail(value.error());
                c.set(std::move(*key), std::move(*value));
                if (over()) return fail(CodecError::too_large);
            }
        }
        }
        return std::unexpected(CodecError::malformed);
    }
};
} // namespace

void write_network(Writer& w, const Tag& root) {
    w.u8(static_cast<std::uint8_t>(root.type()));
    if (root.type() != TagType::end) write_payload(w, root);
}

void write_named(Writer& w, std::string_view name, const Tag& root) {
    w.u8(static_cast<std::uint8_t>(root.type()));
    if (root.type() == TagType::end) return;
    write_string(w, name);
    write_payload(w, root);
}

Decoded<Tag> read_network(Reader& r, const Limits& limits) {
    Decoder d{r, limits, r.position_in_input()};
    const auto t = r.u8();
    if (!t) return std::unexpected(t.error());
    if (*t == 0) return Tag{};
    if (*t > 12) return std::unexpected(CodecError::malformed);
    return d.payload(static_cast<TagType>(*t), 0);
}

Decoded<Tag> read_named(Reader& r, std::string& name, const Limits& limits) {
    Decoder d{r, limits, r.position_in_input()};
    const auto t = r.u8();
    if (!t) return std::unexpected(t.error());
    if (*t == 0) return Tag{};
    if (*t > 12) return std::unexpected(CodecError::malformed);
    auto n = d.string();
    if (!n) return std::unexpected(n.error());
    name = std::move(*n);
    return d.payload(static_cast<TagType>(*t), 0);
}

} // namespace circuit::protocol::nbt
