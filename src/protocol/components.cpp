// SPDX-License-Identifier: AGPL-3.0-only
#include "component_schema.hpp"
#include <circuit/protocol/components.hpp>
#include <circuit/protocol/nbt.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace circuit::protocol {
using detail::ComponentSchema;
using detail::SchemaNode;
using detail::SchemaOp;
using detail::schema_string;

namespace {
// Nesting of stacks inside components (bundles, containers, matchers).
constexpr int max_depth = 16;
constexpr std::int32_t max_entries = 1 << 16;

using nbt::canonical;

const ComponentSchema& schema_of(const void* p) { return *static_cast<const ComponentSchema*>(p); }

// Reads one value of a schema node and writes its canonical form, checking
// registry ids, enumerations and mapped values on the way.
class Transcoder {
public:
    Transcoder(const ComponentSchema& s, const ComponentContext& ctx) : s_(s), ctx_(ctx) {}

    bool node(Reader& r, Writer& w, std::uint32_t index) {
        const auto& n = s_.nodes[index];
        switch (n.op) {
        case SchemaOp::void_: return true;
        case SchemaOp::boolean_: {
            const auto v = r.u8();
            if (!v || *v > 1) return fail("invalid boolean");
            w.u8(*v);
            return note(*v ? "true" : "false");
        }
        case SchemaOp::i8_: return copy(r, w, 1);
        case SchemaOp::u8_: return copy(r, w, 1);
        case SchemaOp::i16_: case SchemaOp::u16_: return copy(r, w, 2);
        case SchemaOp::i32_: {
            const auto v = r.i32();
            if (!v) return fail("truncated");
            w.i32(*v);
            return note(std::to_string(*v));
        }
        case SchemaOp::f32_: return copy(r, w, 4);
        case SchemaOp::i64_: case SchemaOp::f64_: case SchemaOp::position_: return copy(r, w, 8);
        case SchemaOp::uuid_: return copy(r, w, 16);
        case SchemaOp::varint_: {
            const auto v = r.varint();
            if (!v) return fail("truncated varint");
            if (n.registry && !in_registry(n.registry, *v)) return false;
            w.varint(*v);
            return note(std::to_string(*v));
        }
        case SchemaOp::varlong_: {
            const auto v = r.varlong();
            if (!v) return fail("truncated varlong");
            w.varlong(*v);
            return note(std::to_string(*v));
        }
        case SchemaOp::string_: {
            const auto v = r.string();
            if (!v) return fail("invalid string");
            w.string(*v);
            return note(*v);
        }
        case SchemaOp::nbt_: {
            const auto t = nbt::read_network(r);
            if (!t || t->type() == nbt::TagType::end) return fail("invalid NBT");
            nbt::write_network(w, canonical(*t));
            return true;
        }
        case SchemaOp::optional_nbt_: {
            const auto rest = r.rest();
            if (rest.empty()) return fail("truncated NBT");
            if (std::to_integer<std::uint8_t>(rest[0]) == 0) {
                (void)r.u8();
                w.u8(0);
                return true;
            }
            const auto t = nbt::read_network(r);
            if (!t) return fail("invalid NBT");
            nbt::write_network(w, canonical(*t));
            return true;
        }
        case SchemaOp::option_: {
            const auto present = r.u8();
            if (!present || *present > 1) return fail("invalid option");
            w.u8(*present);
            return *present ? node(r, w, n.a) : true;
        }
        case SchemaOp::array_: {
            std::int32_t count = 0;
            if (n.b) {
                const auto v = lookup(schema_string(n.b));
                if (!v || std::from_chars(v->data(), v->data() + v->size(), count).ec != std::errc{}) return fail("array count");
            } else {
                const auto c = r.varint();
                if (!c) return fail("truncated array length");
                count = *c;
                w.varint(count);
            }
            if (count < 0 || count > max_entries || static_cast<std::size_t>(count) > r.remaining()) return fail("array length");
            if (n.c == 0) {
                for (std::int32_t i = 0; i < count; ++i)
                    if (!node(r, w, n.a)) return false;
                return true;
            }
            // A map or set: elements in byte order.
            std::vector<Bytes> elements(static_cast<std::size_t>(count));
            for (auto& e : elements) {
                Writer one;
                if (!node(r, one, n.a)) return false;
                e = one.take();
            }
            std::ranges::sort(elements);
            for (const auto& e : elements) w.raw(e);
            return true;
        }
        case SchemaOp::container_: {
            frames_.emplace_back();
            bool ok = true;
            for (std::uint32_t i = 0; i < n.b && ok; ++i) {
                const auto& f = s_.fields[n.a + i];
                field_ = f.anon ? "" : schema_string(f.name);
                ok = node(r, w, f.node);
            }
            frames_.pop_back();
            return ok;
        }
        case SchemaOp::switch_: {
            const auto value = lookup(schema_string(n.a));
            if (!value) return fail("switch field");
            for (std::uint32_t i = 0; i < n.c; ++i) {
                const auto& c = s_.cases[n.b + i];
                if (*value == schema_string(c.value)) return node(r, w, c.node);
            }
            if (n.d) return node(r, w, n.d - 1);
            return true; // no case: no data
        }
        case SchemaOp::mapper_: {
            const auto v = r.varint();
            if (!v) return fail("truncated varint");
            for (std::uint32_t i = 0; i < n.b; ++i) {
                const auto& m = s_.mappings[n.a + i];
                if (m.key == *v) {
                    w.varint(*v);
                    return note(schema_string(m.value));
                }
            }
            return fail(std::format("value {} outside its enumeration", *v));
        }
        case SchemaOp::bitfield_: {
            std::uint32_t total = 0;
            for (std::uint32_t i = 0; i < n.b; ++i) total += s_.bits[n.a + i].size;
            if (total % 8) return fail("bit field size");
            return copy(r, w, total / 8);
        }
        case SchemaOp::holder_: {
            const auto v = r.varint();
            if (!v) return fail("truncated holder");
            w.varint(*v);
            if (*v == 0) return node(r, w, n.a); // a value given inline
            return in_registry(n.registry, *v - 1);
        }
        case SchemaOp::holder_set_: {
            const auto v = r.varint();
            if (!v || *v < 0 || *v > max_entries) return fail("holder set length");
            w.varint(*v);
            if (*v == 0) {
                const auto tag = r.identifier();
                if (!tag) return fail("invalid tag name");
                w.string(*tag);
                return true;
            }
            for (std::int32_t i = 0; i < *v - 1; ++i) {
                const auto id = r.varint();
                if (!id || !in_registry(n.registry, *id)) return id ? false : fail("truncated holder set");
                w.varint(*id);
            }
            return true;
        }
        case SchemaOp::slot_: return stack(r, w);
        case SchemaOp::component_: {
            const auto type = r.varint();
            if (!type || *type < 0 || static_cast<std::size_t>(*type) >= s_.component_count) return fail("unknown component type");
            w.varint(*type);
            return value(r, w, static_cast<std::uint16_t>(*type));
        }
        case SchemaOp::buffer_: {
            const auto b = r.prefixed_bytes(r.remaining());
            if (!b) return fail("truncated bytes");
            w.prefixed_bytes(*b);
            return true;
        }
        case SchemaOp::ref_: return node(r, w, n.a);
        }
        return fail("unknown schema node");
    }

    // One component value of `type`.
    bool value(Reader& r, Writer& w, std::uint16_t type) {
        const auto saved = std::exchange(frames_, {});
        frames_.emplace_back();
        field_ = "";
        const bool ok = node(r, w, s_.components[type].node);
        frames_ = saved;
        if (!ok && component_.empty()) component_ = schema_string(s_.components[type].name);
        return ok;
    }

    // A stack inside a component: count, item and its own components.
    bool stack(Reader& r, Writer& w) {
        if (++depth_ > max_depth) return fail("stacks nested too deeply");
        const auto count = r.varint();
        if (!count || *count < 0 || *count > 99) return fail("stack count");
        w.varint(*count);
        bool ok = true;
        if (*count > 0) {
            const auto item = r.varint();
            if (!item || *item <= 0 || static_cast<std::uint32_t>(*item) >= ctx_.items) return fail("unknown item");
            w.varint(*item);
            ok = patch(r, w, false).has_value();
        }
        --depth_;
        return ok;
    }

    struct Parsed {
        std::vector<contracts::ComponentPatch::Added> added;
        std::vector<std::uint16_t> removed;
    };
    // A patch: counts, added components (sorted on output), removed types.
    std::optional<Parsed> patch(Reader& r, Writer& w, bool length_prefixed) {
        auto parsed = read_patch(r, length_prefixed);
        if (!parsed) return std::nullopt;
        w.varint(static_cast<std::int32_t>(parsed->added.size())).varint(static_cast<std::int32_t>(parsed->removed.size()));
        for (const auto& a : parsed->added) w.varint(a.type).raw(a.value);
        for (const auto t : parsed->removed) w.varint(t);
        return parsed;
    }
    std::optional<Parsed> read_patch(Reader& r, bool length_prefixed) {
        const auto added = r.varint();
        const auto removed = r.varint();
        if (!added || !removed || *added < 0 || *removed < 0 || static_cast<std::size_t>(*added) + static_cast<std::size_t>(*removed) > s_.component_count)
            return fail("component counts"), std::nullopt;
        Parsed p;
        for (std::int32_t i = 0; i < *added; ++i) {
            const auto type = r.varint();
            if (!type || *type < 0 || static_cast<std::size_t>(*type) >= s_.component_count) return fail("unknown component type"), std::nullopt;
            Writer value_out;
            if (length_prefixed) {
                const auto bytes = r.prefixed_bytes(r.remaining());
                if (!bytes) return fail("truncated component"), std::nullopt;
                Reader inner(*bytes);
                if (!value(inner, value_out, static_cast<std::uint16_t>(*type))) return std::nullopt;
                if (inner.remaining() != 0) {
                    component_ = schema_string(s_.components[*type].name);
                    return fail("trailing bytes"), std::nullopt;
                }
            } else if (!value(r, value_out, static_cast<std::uint16_t>(*type))) {
                return std::nullopt;
            }
            p.added.push_back({static_cast<std::uint16_t>(*type), value_out.take()});
        }
        for (std::int32_t i = 0; i < *removed; ++i) {
            const auto type = r.varint();
            if (!type || *type < 0 || static_cast<std::size_t>(*type) >= s_.component_count) return fail("unknown component type"), std::nullopt;
            p.removed.push_back(static_cast<std::uint16_t>(*type));
        }
        std::ranges::sort(p.added, {}, &contracts::ComponentPatch::Added::type);
        std::ranges::sort(p.removed);
        for (std::size_t i = 1; i < p.added.size(); ++i)
            if (p.added[i].type == p.added[i - 1].type) return fail("component given twice"), std::nullopt;
        for (std::size_t i = 1; i < p.removed.size(); ++i)
            if (p.removed[i] == p.removed[i - 1]) return fail("component removed twice"), std::nullopt;
        for (const auto t : p.removed)
            if (std::ranges::binary_search(p.added, t, {}, &contracts::ComponentPatch::Added::type))
                return fail("component added and removed"), std::nullopt;
        return p;
    }

    [[nodiscard]] std::string error() const {
        return component_.empty() ? error_ : std::format("component minecraft:{}: {}", component_, error_);
    }

private:
    bool fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        return false;
    }
    bool copy(Reader& r, Writer& w, std::size_t n) {
        const auto b = r.raw(n);
        if (!b) return fail("truncated");
        w.raw(*b);
        return true;
    }
    bool note(std::string value) {
        if (!frames_.empty() && !field_.empty()) frames_.back().emplace_back(field_, std::move(value));
        return true;
    }
    std::optional<std::string> lookup(std::string_view path) const {
        std::size_t up = 0;
        while (path.starts_with("../")) {
            path.remove_prefix(3);
            ++up;
        }
        if (up >= frames_.size()) return std::nullopt;
        const auto& frame = frames_[frames_.size() - 1 - up];
        for (auto it = frame.rbegin(); it != frame.rend(); ++it)
            if (it->first == path) return it->second;
        return std::nullopt;
    }
    std::optional<std::uint32_t> registry_size(std::string_view registry) const {
        if (registry == "item") return ctx_.items;
        if (registry == "block") return ctx_.blocks;
        for (std::size_t i = 0; i < s_.size_count; ++i)
            if (schema_string(s_.sizes[i].name) == registry) return s_.sizes[i].size;
        return ctx_.synced ? ctx_.synced(registry) : std::nullopt;
    }
    bool in_registry(std::uint32_t registry, std::int64_t id) {
        const std::string_view name = schema_string(registry);
        const auto size = registry_size(name);
        if (!size || *size == 0) return fail(std::format("refers to the {} registry, which this server does not synchronize", name));
        if (id < 0 || id >= *size) return fail(std::format("{} id {} outside the registry", name, id));
        return true;
    }

    const ComponentSchema& s_;
    const ComponentContext& ctx_;
    std::vector<std::vector<std::pair<std::string, std::string>>> frames_;
    std::string field_;
    std::string error_, component_;
    int depth_{};
};

std::optional<std::int64_t> varint_value(std::span<const std::byte> value) {
    Reader r(value);
    const auto v = r.varint();
    if (!v || r.remaining() != 0) return std::nullopt;
    return *v;
}
} // namespace

const ComponentCodec* ComponentCodec::for_protocol(std::int32_t protocol) noexcept {
    static const ComponentCodec c772(detail::component_schema(772));
    static const ComponentCodec c769(detail::component_schema(769));
    if (protocol == 772) return &c772;
    if (protocol == 769) return &c769;
    return nullptr;
}

std::int32_t ComponentCodec::protocol() const noexcept { return schema_of(schema_).protocol; }
std::size_t ComponentCodec::type_count() const noexcept { return schema_of(schema_).component_count; }

std::string_view ComponentCodec::type_name(std::uint16_t type) const noexcept {
    const auto& s = schema_of(schema_);
    return type < s.component_count ? schema_string(s.components[type].name) : std::string_view{};
}

std::optional<std::uint16_t> ComponentCodec::type_id(std::string_view name) const noexcept {
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    const auto& s = schema_of(schema_);
    for (std::size_t i = 0; i < s.component_count; ++i)
        if (schema_string(s.components[i].name) == name) return static_cast<std::uint16_t>(i);
    return std::nullopt;
}

namespace {
// Splits the damage off and reads the typed fields; checks their ranges.
std::expected<WirePatch, std::string> finish(const ComponentCodec& codec, std::int32_t protocol,
                                             std::vector<contracts::ComponentPatch::Added> added, std::vector<std::uint16_t> removed) {
    WirePatch out;
    contracts::ComponentPatch::Typed typed;
    const auto damage = codec.type_id("damage"), max_stack = codec.type_id("max_stack_size"), max_damage = codec.type_id("max_damage");
    std::vector<contracts::ComponentPatch::Added> rest;
    for (auto& a : added) {
        if (a.type == damage) {
            const auto v = varint_value(a.value);
            if (!v || *v < 0) return std::unexpected("component minecraft:damage: negative damage");
            out.damage = static_cast<std::int32_t>(*v);
            continue;
        }
        if (a.type == max_stack) {
            const auto v = varint_value(a.value);
            if (!v || *v < 1 || *v > 99) return std::unexpected("component minecraft:max_stack_size: outside 1-99");
            typed.max_stack_size = static_cast<std::uint8_t>(*v);
        }
        if (a.type == max_damage) {
            const auto v = varint_value(a.value);
            if (!v || *v < 1) return std::unexpected("component minecraft:max_damage: below 1");
            typed.max_damage = static_cast<std::int32_t>(*v);
        }
        rest.push_back(std::move(a));
    }
    out.rest = contracts::ComponentPatch::make(protocol, std::move(rest), std::move(removed), typed);
    return out;
}
} // namespace

std::expected<WirePatch, std::string> ComponentCodec::read(Reader& r, const ComponentContext& ctx, bool length_prefixed) const {
    Transcoder t(schema_of(schema_), ctx);
    auto parsed = t.read_patch(r, length_prefixed);
    if (!parsed) return std::unexpected(t.error());
    return finish(*this, protocol(), std::move(parsed->added), std::move(parsed->removed));
}

void ComponentCodec::write(Writer& w, std::optional<std::int32_t> damage, const contracts::ComponentPatch& patch) const {
    const auto damage_type = *type_id("damage");
    const auto added = patch.added();
    w.varint(static_cast<std::int32_t>(added.size() + (damage ? 1 : 0))).varint(static_cast<std::int32_t>(patch.removed().size()));
    bool damage_written = !damage;
    for (const auto& a : added) {
        if (!damage_written && damage_type < a.type) {
            w.varint(damage_type).varint(*damage);
            damage_written = true;
        }
        w.varint(a.type).raw(a.value);
    }
    if (!damage_written) w.varint(damage_type).varint(*damage);
    for (const auto t : patch.removed()) w.varint(t);
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::check(const contracts::ComponentPatch& patch,
                                                                             const ComponentContext& ctx) const {
    if (patch.empty()) return patch;
    if (patch.protocol() != protocol()) return std::unexpected(std::format("components of protocol {}, not {}", patch.protocol(), protocol()));
    Writer w;
    write(w, std::nullopt, patch);
    Reader r(w.bytes());
    auto read_back = read(r, ctx);
    if (!read_back) return std::unexpected(read_back.error());
    if (read_back->damage) return std::unexpected("component minecraft:damage: stored with the other components");
    if (!(read_back->rest == patch) || !(read_back->rest.typed() == patch.typed()))
        return std::unexpected("components not in canonical form");
    return read_back->rest;
}

namespace {
// Components whose save form mirrors the network form in protocol 772
// (minecraft.wiki "Data component format", revision 3135677): plain values,
// text components, NBT and lists of them.
constexpr std::string_view save_mirrors_772[] = {
    "custom_data", "max_stack_size", "max_damage", "unbreakable", "custom_name", "item_name", "item_model", "lore", "rarity",
    "repair_cost", "enchantment_glint_override", "intangible_projectile", "glider", "tooltip_style", "dyed_color", "map_color",
    "map_id", "map_decorations", "potion_duration_scale", "recipes", "ominous_bottle_amplifier", "note_block_sound",
    "provides_banner_patterns", "lock", "container_loot", "debug_stick_state", "entity_data", "bucket_entity_data",
    "block_entity_data",
};

const SchemaNode& resolve(const ComponentSchema& s, std::uint32_t index) {
    while (s.nodes[index].op == SchemaOp::ref_) index = s.nodes[index].a;
    return s.nodes[index];
}

// Dye colors by id (minecraft.wiki "Dye", data values).
constexpr std::string_view dye_colors[] = {
    "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
};
std::optional<std::int32_t> dye_id(std::string_view name) {
    for (std::size_t i = 0; i < std::size(dye_colors); ++i)
        if (dye_colors[i] == name) return static_cast<std::int32_t>(i);
    return std::nullopt;
}

// Components of protocol 772 whose save form names registry entries.
enum class Named { none, trim, banner_patterns, base_color, enchantments };
Named named_form(const ComponentSchema& s, std::uint16_t type) {
    if (s.protocol != 772) return Named::none;
    const std::string_view name = schema_string(s.components[type].name);
    if (name == "enchantments" || name == "stored_enchantments") return Named::enchantments;
    if (name == "trim") return Named::trim;
    if (name == "banner_patterns") return Named::banner_patterns;
    if (name == "base_color") return Named::base_color;
    return Named::none;
}

// A registry holder (0: an inline definition, else id + 1) as an entry name.
std::expected<nbt::Tag, std::string> holder_name(Reader& r, std::string_view registry, const ComponentContext& ctx) {
    const auto v = r.varint();
    if (!v) return std::unexpected("truncated");
    if (*v == 0) return std::unexpected(std::format("an inline {} definition has no save form yet", registry));
    const auto name = ctx.name_of ? ctx.name_of(registry, static_cast<std::uint32_t>(*v - 1)) : std::nullopt;
    if (!name) return std::unexpected(std::format("{} id {} has no name", registry, *v - 1));
    return nbt::Tag::string(std::string(*name));
}
std::optional<std::int32_t> holder_id(const nbt::Tag& t, std::string_view registry, const ComponentContext& ctx) {
    if (t.type() != nbt::TagType::string || !ctx.id_of) return std::nullopt;
    const auto id = ctx.id_of(registry, t.text());
    return id ? std::optional(static_cast<std::int32_t>(*id + 1)) : std::nullopt;
}

bool mirrors(const ComponentSchema& s, std::uint16_t type) {
    if (s.protocol != 772) return false;
    const std::string_view name = schema_string(s.components[type].name);
    return std::ranges::find(save_mirrors_772, name) != std::end(save_mirrors_772);
}

std::optional<std::int64_t> whole(const nbt::Tag& t) {
    switch (t.type()) {
    case nbt::TagType::byte_: case nbt::TagType::short_: case nbt::TagType::int_: case nbt::TagType::long_: return t.integer();
    default: return std::nullopt;
    }
}
} // namespace

std::expected<nbt::Tag, std::string> ComponentCodec::to_save(std::uint16_t type, std::span<const std::byte> value,
                                                             const ComponentContext& ctx) const {
    const auto& s = schema_of(schema_);
    if (type >= s.component_count) return std::unexpected("unknown component type");
    const std::string name = std::format("item component minecraft:{}", schema_string(s.components[type].name));
    Reader r(value);
    const auto done = [&](nbt::Tag t) -> std::expected<nbt::Tag, std::string> {
        if (r.remaining() != 0) return std::unexpected(name + ": trailing bytes");
        return t;
    };
    const auto failed = [&](const std::string& why) { return std::unexpected(std::format("{}: {}", name, why)); };
    switch (named_form(s, type)) {
    case Named::trim: {
        // {material: "minecraft:gold", pattern: "minecraft:coast"}
        auto out = nbt::Tag::compound();
        auto material = holder_name(r, "trim_material", ctx);
        if (!material) return failed(material.error());
        auto pattern = holder_name(r, "trim_pattern", ctx);
        if (!pattern) return failed(pattern.error());
        out.set("material", std::move(*material));
        out.set("pattern", std::move(*pattern));
        return done(std::move(out));
    }
    case Named::banner_patterns: {
        // [{pattern: "minecraft:stripe_top", color: "red"}, ...]
        const auto count = r.varint();
        if (!count || *count < 0) return failed("invalid list");
        auto list = nbt::Tag::list(nbt::TagType::compound);
        for (std::int32_t i = 0; i < *count; ++i) {
            auto pattern = holder_name(r, "banner_pattern", ctx);
            if (!pattern) return failed(pattern.error());
            const auto color = r.varint();
            if (!color || *color < 0 || *color >= static_cast<std::int32_t>(std::size(dye_colors))) return failed("invalid color");
            auto layer = nbt::Tag::compound();
            layer.set("pattern", std::move(*pattern));
            layer.set("color", nbt::Tag::string(std::string(dye_colors[*color])));
            list.push(std::move(layer));
        }
        return done(std::move(list));
    }
    case Named::base_color: {
        const auto color = r.varint();
        if (!color || *color < 0 || *color >= static_cast<std::int32_t>(std::size(dye_colors))) return failed("invalid color");
        return done(nbt::Tag::string(std::string(dye_colors[*color])));
    }
    case Named::enchantments: {
        // {"minecraft:sharpness": 3, ...}
        const auto count = r.varint();
        if (!count || *count < 0) return failed("invalid list");
        auto levels = nbt::Tag::compound();
        for (std::int32_t i = 0; i < *count; ++i) {
            const auto id = r.varint();
            const auto level = r.varint();
            if (!id || !level) return failed("truncated");
            const auto entry = ctx.name_of ? ctx.name_of("enchantment", static_cast<std::uint32_t>(*id)) : std::nullopt;
            if (!entry) return failed(std::format("enchantment id {} has no name", *id));
            levels.set(std::string(*entry), nbt::Tag::int_(*level));
        }
        return done(std::move(levels));
    }
    case Named::none: break;
    }
    if (!mirrors(s, type)) return std::unexpected(name + " has no save form in Circuit yet");
    const auto& n = resolve(s, s.components[type].node);
    switch (n.op) {
    case SchemaOp::void_: return done(nbt::Tag::compound());
    case SchemaOp::nbt_: {
        auto t = nbt::read_network(r);
        if (!t) return std::unexpected(name + ": invalid NBT");
        return done(std::move(*t));
    }
    case SchemaOp::varint_: {
        const auto v = r.varint();
        if (!v) return std::unexpected(name + ": truncated");
        return done(nbt::Tag::int_(*v));
    }
    case SchemaOp::i32_: {
        const auto v = r.i32();
        if (!v) return std::unexpected(name + ": truncated");
        return done(nbt::Tag::int_(*v));
    }
    case SchemaOp::f32_: {
        const auto v = r.f32();
        if (!v) return std::unexpected(name + ": truncated");
        return done(nbt::Tag::float_(*v));
    }
    case SchemaOp::boolean_: {
        const auto v = r.u8();
        if (!v) return std::unexpected(name + ": truncated");
        return done(nbt::Tag::boolean(*v != 0));
    }
    case SchemaOp::string_: {
        const auto v = r.string();
        if (!v) return std::unexpected(name + ": invalid string");
        return done(nbt::Tag::string(*v));
    }
    case SchemaOp::mapper_: {
        const auto v = r.varint();
        if (!v) return std::unexpected(name + ": truncated");
        for (std::uint32_t i = 0; i < n.b; ++i)
            if (s.mappings[n.a + i].key == *v) return done(nbt::Tag::string(schema_string(s.mappings[n.a + i].value)));
        return std::unexpected(name + ": value outside its enumeration");
    }
    case SchemaOp::array_: {
        if (n.b != 0 || resolve(s, n.a).op != SchemaOp::nbt_) break;
        const auto count = r.varint();
        if (!count || *count < 0) return std::unexpected(name + ": invalid list");
        auto list = nbt::Tag::list(nbt::TagType::end);
        for (std::int32_t i = 0; i < *count; ++i) {
            auto t = nbt::read_network(r);
            if (!t) return std::unexpected(name + ": invalid NBT");
            if (list.element() != nbt::TagType::end && t->type() != list.element()) return std::unexpected(name + ": mixed list");
            list.push(std::move(*t));
        }
        return done(std::move(list));
    }
    default: break;
    }
    return std::unexpected(name + " has no save form in Circuit yet");
}

std::expected<Bytes, std::string> ComponentCodec::from_save(std::uint16_t type, const nbt::Tag& value, const ComponentContext& ctx) const {
    const auto& s = schema_of(schema_);
    if (type >= s.component_count) return std::unexpected("unknown component type");
    const std::string name = std::format("item component minecraft:{}", schema_string(s.components[type].name));
    Writer w;
    const auto bad = [&] { return std::unexpected(name + " with an invalid value"); };
    switch (named_form(s, type)) {
    case Named::trim: {
        const auto* material = value.find("material");
        const auto* pattern = value.find("pattern");
        const auto m = material ? holder_id(*material, "trim_material", ctx) : std::nullopt;
        const auto p = pattern ? holder_id(*pattern, "trim_pattern", ctx) : std::nullopt;
        if (value.type() != nbt::TagType::compound || !m || !p) return bad();
        w.varint(*m).varint(*p);
        return canonical_value(type, w.bytes(), ctx);
    }
    case Named::banner_patterns: {
        if (value.type() != nbt::TagType::list) return bad();
        w.varint(static_cast<std::int32_t>(value.items().size()));
        for (const auto& layer : value.items()) {
            const auto* pattern = layer.type() == nbt::TagType::compound ? layer.find("pattern") : nullptr;
            const auto* color = layer.type() == nbt::TagType::compound ? layer.find("color") : nullptr;
            const auto p = pattern ? holder_id(*pattern, "banner_pattern", ctx) : std::nullopt;
            const auto c = color && color->type() == nbt::TagType::string ? dye_id(color->text()) : std::nullopt;
            if (!p || !c) return bad();
            w.varint(*p).varint(*c);
        }
        return canonical_value(type, w.bytes(), ctx);
    }
    case Named::base_color: {
        const auto c = value.type() == nbt::TagType::string ? dye_id(value.text()) : std::nullopt;
        if (!c) return bad();
        w.varint(*c);
        return canonical_value(type, w.bytes(), ctx);
    }
    case Named::enchantments: {
        if (!ctx.synced || !ctx.synced("enchantment"))
            return std::unexpected(name + ": the enchantment registry is not synchronized without the official data pack");
        if (value.type() != nbt::TagType::compound) return bad();
        w.varint(static_cast<std::int32_t>(value.keys().size()));
        for (std::size_t i = 0; i < value.keys().size(); ++i) {
            const auto id = ctx.id_of ? ctx.id_of("enchantment", value.keys()[i]) : std::nullopt;
            const auto level = whole(value.items()[i]);
            if (!id || !level || *level < 1 || *level > 255) return bad();
            w.varint(static_cast<std::int32_t>(*id)).varint(static_cast<std::int32_t>(*level));
        }
        return canonical_value(type, w.bytes(), ctx);
    }
    case Named::none: break;
    }
    if (!mirrors(s, type)) return std::unexpected(name);
    const auto& n = resolve(s, s.components[type].node);
    switch (n.op) {
    case SchemaOp::void_:
        if (value.type() != nbt::TagType::compound) return bad();
        break;
    case SchemaOp::nbt_: nbt::write_network(w, value); break;
    case SchemaOp::varint_: case SchemaOp::i32_: {
        const auto v = whole(value);
        if (!v || *v < INT32_MIN || *v > INT32_MAX) return bad();
        if (n.op == SchemaOp::varint_) w.varint(static_cast<std::int32_t>(*v));
        else w.i32(static_cast<std::int32_t>(*v));
        break;
    }
    case SchemaOp::f32_:
        if (value.type() == nbt::TagType::float_ || value.type() == nbt::TagType::double_) w.f32(static_cast<float>(value.real()));
        else if (const auto v = whole(value)) w.f32(static_cast<float>(*v));
        else return bad();
        break;
    case SchemaOp::boolean_: {
        const auto v = whole(value);
        if (!v) return bad();
        w.boolean(*v != 0);
        break;
    }
    case SchemaOp::string_:
        if (value.type() != nbt::TagType::string) return bad();
        w.string(value.text());
        break;
    case SchemaOp::mapper_: {
        if (value.type() != nbt::TagType::string) return bad();
        std::optional<std::int32_t> key;
        for (std::uint32_t i = 0; i < n.b; ++i)
            if (schema_string(s.mappings[n.a + i].value) == value.text()) key = s.mappings[n.a + i].key;
        if (!key) return bad();
        w.varint(*key);
        break;
    }
    case SchemaOp::array_:
        if (value.type() != nbt::TagType::list) return bad();
        w.varint(static_cast<std::int32_t>(value.items().size()));
        for (const auto& item : value.items()) nbt::write_network(w, item);
        break;
    default: return std::unexpected(name);
    }
    // The value then goes through the same checks as one from a client.
    return canonical_value(type, w.bytes(), ctx);
}

std::expected<Bytes, std::string> ComponentCodec::canonical_value(std::uint16_t type, std::span<const std::byte> value,
                                                                   const ComponentContext& ctx) const {
    if (type >= type_count()) return std::unexpected("unknown component type");
    Transcoder t(schema_of(schema_), ctx);
    Reader r(value);
    Writer w;
    if (!t.value(r, w, type)) return std::unexpected(t.error());
    if (r.remaining() != 0) return std::unexpected(std::format("component minecraft:{}: trailing bytes", type_name(type)));
    return w.take();
}

long ComponentCodec::enchantment_level(const contracts::ComponentPatch& patch, std::uint32_t enchantment) const {
    if (patch.empty() || patch.protocol() != protocol()) return 0;
    const auto type = type_id("enchantments");
    if (!type) return 0;
    for (const auto& a : patch.added()) {
        if (a.type != *type) continue;
        // Canonical form: a count, then (enchantment id, level) pairs; 1.21.4
        // adds the tooltip flag after them.
        Reader r(a.value);
        const auto count = r.varint();
        for (std::int32_t i = 0; count && i < *count; ++i) {
            const auto id = r.varint();
            const auto level = r.varint();
            if (!id || !level) return 0;
            if (static_cast<std::uint32_t>(*id) == enchantment) return *level;
        }
    }
    return 0;
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::enchanted(std::span<const std::pair<std::uint32_t, std::int32_t>> levels,
                                                                                const ComponentContext& ctx) const {
    const auto type = type_id("enchantments");
    if (!type) return std::unexpected("no enchantments component");
    Writer w;
    w.varint(static_cast<std::int32_t>(levels.size()));
    for (const auto& [id, level] : levels) w.varint(static_cast<std::int32_t>(id)).varint(level);
    auto value = canonical_value(*type, w.bytes(), ctx);
    if (!value) {
        // 1.21.4's form ends with whether the tooltip shows them.
        w.boolean(true);
        value = canonical_value(*type, w.bytes(), ctx);
    }
    if (!value) return std::unexpected(value.error());
    return contracts::ComponentPatch::make(protocol(), {{*type, std::move(*value)}}, {}, {});
}

std::vector<std::pair<std::uint32_t, std::int32_t>> ComponentCodec::enchantments_of(const contracts::ComponentPatch& patch, bool stored) const {
    std::vector<std::pair<std::uint32_t, std::int32_t>> out;
    const auto type = type_id(stored ? "stored_enchantments" : "enchantments");
    if (patch.empty() || patch.protocol() != protocol() || !type) return out;
    for (const auto& a : patch.added()) {
        if (a.type != *type) continue;
        Reader r(a.value);
        const auto count = r.varint();
        for (std::int32_t i = 0; count && i < *count; ++i) {
            const auto id = r.varint();
            const auto level = r.varint();
            if (!id || !level) return {};
            out.emplace_back(static_cast<std::uint32_t>(*id), *level);
        }
    }
    return out;
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_enchantments(
    const contracts::ComponentPatch& base, bool stored, std::span<const std::pair<std::uint32_t, std::int32_t>> levels, bool keep_empty,
    std::int32_t repair_cost, const ComponentContext& ctx) const {
    const auto type = type_id(stored ? "stored_enchantments" : "enchantments");
    const auto cost_type = type_id("repair_cost");
    if (!type || !cost_type) return std::unexpected("no enchantment components");
    // 1.21.4's form ends with whether the tooltip shows them: kept as it was.
    std::optional<bool> flag;
    for (const auto& a : base.added())
        if (a.type == *type && !a.value.empty()) flag = a.value.back() != std::byte{0};
    std::vector<contracts::ComponentPatch::Added> added;
    for (const auto& a : base.added())
        if (a.type != *type && a.type != *cost_type) added.push_back(a);
    if (!levels.empty() || keep_empty) {
        Writer w;
        w.varint(static_cast<std::int32_t>(levels.size()));
        for (const auto& [id, level] : levels) w.varint(static_cast<std::int32_t>(id)).varint(level);
        auto value = canonical_value(*type, w.bytes(), ctx);
        if (!value) {
            w.boolean(flag.value_or(true));
            value = canonical_value(*type, w.bytes(), ctx);
        }
        if (!value) return std::unexpected(value.error());
        added.push_back({*type, std::move(*value)});
    }
    if (repair_cost > 0) {
        Writer w;
        w.varint(repair_cost);
        auto value = canonical_value(*cost_type, w.bytes(), ctx);
        if (!value) return std::unexpected(value.error());
        added.push_back({*cost_type, std::move(*value)});
    }
    std::ranges::sort(added, {}, &contracts::ComponentPatch::Added::type);
    std::vector<std::uint16_t> removed;
    for (const auto r : base.removed())
        if (r != *type && r != *cost_type) removed.push_back(r);
    if (added.empty() && removed.empty()) return contracts::ComponentPatch{};
    return contracts::ComponentPatch::make(protocol(), std::move(added), std::move(removed), base.typed());
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_banner_layer(const contracts::ComponentPatch& base,
                                                                                        std::uint32_t pattern, std::uint32_t color,
                                                                                        const ComponentContext& ctx) const {
    const auto type = type_id("banner_patterns");
    if (!type) return std::unexpected("no banner_patterns component");
    // The layers so far (a count, then each layer's pattern holder and
    // color), then the new one as a registry reference (id + 1).
    std::int32_t count = 0;
    std::span<const std::byte> layers;
    for (const auto& a : base.added())
        if (a.type == *type) {
            Reader r(a.value);
            const auto n = r.varint();
            if (!n) return std::unexpected("banner_patterns: unreadable");
            count = *n;
            layers = std::span(a.value).subspan(a.value.size() - r.remaining());
        }
    Writer w;
    w.varint(count + 1);
    w.raw(layers);
    w.varint(static_cast<std::int32_t>(pattern) + 1).varint(static_cast<std::int32_t>(color));
    auto value = canonical_value(*type, w.bytes(), ctx);
    if (!value) return std::unexpected(value.error());
    std::vector<contracts::ComponentPatch::Added> added;
    for (const auto& a : base.added())
        if (a.type != *type) added.push_back(a);
    added.push_back({*type, std::move(*value)});
    std::ranges::sort(added, {}, &contracts::ComponentPatch::Added::type);
    std::vector<std::uint16_t> removed;
    for (const auto r : base.removed())
        if (r != *type) removed.push_back(r);
    return contracts::ComponentPatch::make(protocol(), std::move(added), std::move(removed), base.typed());
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_trim(const contracts::ComponentPatch& base, std::uint32_t material,
                                                                                std::uint32_t pattern, const ComponentContext& ctx) const {
    const auto type = type_id("trim");
    if (!type) return std::unexpected("no trim component");
    // Both holders as registry references (id + 1).
    Writer w;
    w.varint(static_cast<std::int32_t>(material) + 1).varint(static_cast<std::int32_t>(pattern) + 1);
    auto value = canonical_value(*type, w.bytes(), ctx);
    if (!value) {
        // 1.21.4's form ends with whether the tooltip shows it.
        w.boolean(true);
        value = canonical_value(*type, w.bytes(), ctx);
    }
    if (!value) return std::unexpected(value.error());
    std::vector<contracts::ComponentPatch::Added> added;
    for (const auto& a : base.added())
        if (a.type != *type) added.push_back(a);
    added.push_back({*type, std::move(*value)});
    std::ranges::sort(added, {}, &contracts::ComponentPatch::Added::type);
    std::vector<std::uint16_t> removed;
    for (const auto r : base.removed())
        if (r != *type) removed.push_back(r);
    return contracts::ComponentPatch::make(protocol(), std::move(added), std::move(removed), base.typed());
}


std::expected<contracts::ComponentPatch, std::string> ComponentCodec::writable_book(std::int32_t pages, const ComponentContext& ctx) const {
    const auto type = type_id("writable_book_content");
    if (!type) return std::unexpected("no writable_book_content component");
    // Pages "1" to "n", none filtered.
    Writer w;
    w.varint(pages);
    for (std::int32_t k = 1; k <= pages; ++k) w.string(std::to_string(k)).boolean(false);
    auto value = canonical_value(*type, w.bytes(), ctx);
    if (!value) return std::unexpected(value.error());
    return contracts::ComponentPatch::make(protocol(), {{*type, std::move(*value)}}, {}, {});
}

namespace {
// `base` without the components `gone`, with `added` (none: nothing more).
contracts::ComponentPatch replaced(const contracts::ComponentPatch& base, std::int32_t protocol, std::span<const std::uint16_t> gone,
                                   std::optional<contracts::ComponentPatch::Added> added) {
    std::vector<contracts::ComponentPatch::Added> out;
    for (const auto& a : base.added())
        if (std::ranges::find(gone, a.type) == gone.end() && (!added || a.type != added->type)) out.push_back(a);
    if (added) out.push_back(std::move(*added));
    std::ranges::sort(out, {}, &contracts::ComponentPatch::Added::type);
    std::vector<std::uint16_t> removed;
    for (const auto r : base.removed())
        if (std::ranges::find(gone, r) == gone.end() && (!added || r != added->type)) removed.push_back(r);
    if (out.empty() && removed.empty()) return contracts::ComponentPatch{};
    return contracts::ComponentPatch::make(protocol, std::move(out), std::move(removed), base.typed());
}
} // namespace

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_book_pages(const contracts::ComponentPatch& base,
                                                                                      std::span<const std::string> pages,
                                                                                      const ComponentContext& ctx) const {
    const auto type = type_id("writable_book_content");
    if (!type) return std::unexpected("no writable_book_content component");
    Writer w;
    w.varint(static_cast<std::int32_t>(pages.size()));
    for (const auto& p : pages) w.string(p).boolean(false);
    auto value = canonical_value(*type, w.bytes(), ctx);
    if (!value) return std::unexpected(value.error());
    return replaced(base, protocol(), {}, contracts::ComponentPatch::Added{*type, std::move(*value)});
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::signed_book(const contracts::ComponentPatch& base,
                                                                                  std::span<const std::string> pages, std::string_view title,
                                                                                  std::string_view author, const ComponentContext& ctx) const {
    const auto writable = type_id("writable_book_content");
    const auto written = type_id("written_book_content");
    if (!writable || !written) return std::unexpected("no book content components");
    // The title (no filtered title), the author, generation 0, the pages as
    // plain text (none filtered), resolved.
    Writer w;
    w.string(std::string(title)).boolean(false).string(std::string(author)).varint(0).varint(static_cast<std::int32_t>(pages.size()));
    for (const auto& p : pages) {
        nbt::write_network(w, nbt::Tag::string(p));
        w.boolean(false);
    }
    w.boolean(true);
    auto value = canonical_value(*written, w.bytes(), ctx);
    if (!value) return std::unexpected(value.error());
    const std::array gone{*writable};
    return replaced(base, protocol(), gone, contracts::ComponentPatch::Added{*written, std::move(*value)});
}

std::int32_t ComponentCodec::banner_layers(const contracts::ComponentPatch& patch) const {
    const auto type = type_id("banner_patterns");
    if (!type || patch.empty() || patch.protocol() != protocol()) return 0;
    for (const auto& a : patch.added()) {
        if (a.type != *type) continue;
        // The layers' count first.
        Reader r(a.value);
        const auto n = r.varint();
        return n && *n > 0 ? *n : 0;
    }
    return 0;
}

std::optional<std::int32_t> ComponentCodec::dyed_color_of(const contracts::ComponentPatch& patch) const {
    const auto type = type_id("dyed_color");
    if (!type || patch.empty() || patch.protocol() != protocol()) return std::nullopt;
    for (const auto& a : patch.added()) {
        if (a.type != *type) continue;
        // The color first (1.21.4's tooltip flag after it).
        Reader r(a.value);
        const auto rgb = r.i32();
        if (!rgb) return std::nullopt;
        return *rgb;
    }
    return std::nullopt;
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_dyed_color(const contracts::ComponentPatch& base, std::int32_t rgb,
                                                                                      const ComponentContext& ctx) const {
    const auto type = type_id("dyed_color");
    if (!type) return std::unexpected("no dyed_color component");
    std::optional<bool> flag;
    for (const auto& a : base.added())
        if (a.type == *type && a.value.size() == 5) flag = a.value.back() != std::byte{0};
    Writer w;
    w.i32(rgb);
    auto value = canonical_value(*type, w.bytes(), ctx);
    if (!value) {
        w.boolean(flag.value_or(true));
        value = canonical_value(*type, w.bytes(), ctx);
    }
    if (!value) return std::unexpected(value.error());
    return replaced(base, protocol(), {}, contracts::ComponentPatch::Added{*type, std::move(*value)});
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_component_from(const contracts::ComponentPatch& base,
                                                                                          const contracts::ComponentPatch& from,
                                                                                          std::string_view name) const {
    const auto type = type_id(name);
    if (!type) return std::unexpected(std::format("no {} component", name));
    for (const auto& a : from.added())
        if (a.type == *type) return replaced(base, protocol(), {}, a);
    const std::array gone{*type};
    return replaced(base, protocol(), gone, std::nullopt);
}

std::optional<std::vector<std::string>> ComponentCodec::recipes_of(const contracts::ComponentPatch& patch) const {
    const auto type = type_id("recipes");
    if (!type || patch.empty() || patch.protocol() != protocol()) return std::nullopt;
    for (const auto& a : patch.added()) {
        if (a.type != *type) continue;
        // A list of ids, as NBT.
        Reader r(a.value);
        const auto tag = nbt::read_network(r);
        if (!tag) return std::nullopt;
        std::vector<std::string> out;
        if (tag->type() == nbt::TagType::list)
            for (const auto& e : tag->items())
                if (e.type() == nbt::TagType::string) out.push_back(e.text());
        return out;
    }
    return std::nullopt;
}

std::int32_t ComponentCodec::book_pages_of(const contracts::ComponentPatch& patch) const {
    const auto writable = type_id("writable_book_content");
    const auto written = type_id("written_book_content");
    if (patch.empty() || patch.protocol() != protocol()) return 0;
    for (const auto& a : patch.added()) {
        Reader r(a.value);
        if (writable && a.type == *writable) {
            // The pages first.
            const auto n = r.varint();
            return n && *n > 0 ? *n : 0;
        }
        if (written && a.type == *written) {
            // The title (and its filtered form), the author and the generation, then the pages.
            const auto title = r.string();
            const auto filtered = r.boolean();
            if (!title || !filtered || (*filtered && !r.string())) return 0;
            if (!r.string() || !r.varint()) return 0;
            const auto n = r.varint();
            return n && *n > 0 ? *n : 0;
        }
    }
    return 0;
}

std::expected<std::vector<contracts::ItemStack>, std::string> ComponentCodec::container_of(
    const contracts::ComponentPatch& patch, const ComponentContext& ctx,
    const std::function<std::optional<std::uint16_t>(std::int32_t)>& item_of) const {
    const auto type = type_id("container");
    std::vector<contracts::ItemStack> out;
    if (!type || patch.empty() || patch.protocol() != protocol()) return out;
    for (const auto& a : patch.added()) {
        if (a.type != *type) continue;
        // The slots as optional stacks: a count, then (when not 0) the item
        // and its components (client/shulker: both versions).
        Reader r(a.value);
        const auto n = r.varint();
        if (!n || *n < 0 || *n > 256) return std::unexpected("container: invalid slot count");
        for (std::int32_t k = 0; k < *n; ++k) {
            const auto count = r.varint();
            if (!count || *count < 0 || *count > 99) return std::unexpected("container: invalid count");
            if (*count == 0) {
                out.emplace_back();
                continue;
            }
            const auto wire = r.varint();
            if (!wire) return std::unexpected("container: truncated item");
            const auto item = item_of(*wire);
            if (!item) return std::unexpected(std::format("container: unknown item {}", *wire));
            auto components = read(r, ctx);
            if (!components) return std::unexpected("container: " + components.error());
            out.push_back({*item, static_cast<std::uint8_t>(*count), static_cast<std::uint16_t>(components->damage.value_or(0)),
                           std::move(components->rest)});
        }
        return out;
    }
    return out;
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_container(
    const contracts::ComponentPatch& base, std::span<const contracts::ItemStack> stacks, const ComponentContext& ctx,
    const std::function<std::optional<std::int32_t>(std::uint16_t)>& wire_of) const {
    const auto type = type_id("container");
    if (!type) return std::unexpected("no container component");
    std::vector<contracts::ComponentPatch::Added> added;
    for (const auto& a : base.added())
        if (a.type != *type) added.push_back(a);
    std::size_t last = stacks.size();
    while (last > 0 && stacks[last - 1].empty()) --last;
    if (last > 0) {
        Writer w;
        w.varint(static_cast<std::int32_t>(last));
        for (std::size_t k = 0; k < last; ++k) {
            const auto& s = stacks[k];
            w.varint(s.empty() ? 0 : s.count);
            if (s.empty()) continue;
            const auto wire = wire_of(s.item);
            if (!wire) return std::unexpected(std::format("container: item {} has no id in this protocol", s.item));
            w.varint(*wire);
            write(w, s.damage > 0 ? std::optional<std::int32_t>(s.damage) : std::nullopt, s.components);
        }
        auto value = canonical_value(*type, w.bytes(), ctx);
        if (!value) return std::unexpected(value.error());
        added.push_back({*type, std::move(*value)});
    }
    std::ranges::sort(added, {}, &contracts::ComponentPatch::Added::type);
    std::vector<std::uint16_t> removed;
    for (const auto r : base.removed())
        if (r != *type) removed.push_back(r);
    if (added.empty() && removed.empty()) return contracts::ComponentPatch{};
    return contracts::ComponentPatch::make(protocol(), std::move(added), std::move(removed), base.typed());
}

std::optional<std::uint32_t> ComponentCodec::potion_of(const contracts::ComponentPatch& patch) const {
    const auto type = type_id("potion_contents");
    if (patch.empty() || patch.protocol() != protocol() || !type) return std::nullopt;
    for (const auto& a : patch.added()) {
        if (a.type != *type) continue;
        // Whether there is a potion, then its id.
        Reader r(a.value);
        const auto has = r.boolean();
        if (!has || !*has) return std::nullopt;
        const auto id = r.varint();
        if (!id || *id < 0) return std::nullopt;
        return static_cast<std::uint32_t>(*id);
    }
    return std::nullopt;
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_potion(const contracts::ComponentPatch& base, std::uint32_t potion,
                                                                                  const ComponentContext& ctx) const {
    const auto type = type_id("potion_contents");
    if (!type) return std::unexpected("no potion_contents component");
    Writer w;
    w.boolean(true).varint(static_cast<std::int32_t>(potion));
    // The rest of the contents as they were: the custom color, effects and
    // name; none of them without contents.
    bool had = false;
    for (const auto& a : base.added()) {
        if (a.type != *type) continue;
        Reader r(a.value);
        const auto has = r.boolean();
        if (!has || (*has && !r.varint())) return std::unexpected("potion_contents: unreadable");
        w.raw(std::span(a.value).subspan(a.value.size() - r.remaining()));
        had = true;
    }
    if (!had) w.boolean(false).varint(0).boolean(false);
    auto value = canonical_value(*type, w.bytes(), ctx);
    if (!value) return std::unexpected(value.error());
    std::vector<contracts::ComponentPatch::Added> added;
    for (const auto& a : base.added())
        if (a.type != *type) added.push_back(a);
    added.push_back({*type, std::move(*value)});
    std::ranges::sort(added, {}, &contracts::ComponentPatch::Added::type);
    std::vector<std::uint16_t> removed;
    for (const auto r : base.removed())
        if (r != *type) removed.push_back(r);
    return contracts::ComponentPatch::make(protocol(), std::move(added), std::move(removed), base.typed());
}


std::optional<std::int32_t> ComponentCodec::varint_of(const contracts::ComponentPatch& patch, std::string_view component) const {
    const auto type = type_id(component);
    if (patch.empty() || patch.protocol() != protocol() || !type) return std::nullopt;
    for (const auto& a : patch.added())
        if (a.type == *type) {
            Reader r(a.value);
            if (const auto v = r.varint()) return *v;
        }
    return std::nullopt;
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_varint(const contracts::ComponentPatch& base,
                                                                                  std::string_view component,
                                                                                  std::optional<std::int32_t> value,
                                                                                  const ComponentContext& ctx) const {
    const auto type = type_id(component);
    if (!type) return std::unexpected(std::format("no {} component", component));
    std::vector<contracts::ComponentPatch::Added> added;
    for (const auto& a : base.added())
        if (a.type != *type) added.push_back(a);
    if (value) {
        Writer w;
        w.varint(*value);
        auto canonical = canonical_value(*type, w.bytes(), ctx);
        if (!canonical) return std::unexpected(canonical.error());
        added.push_back({*type, std::move(*canonical)});
        std::ranges::sort(added, {}, &contracts::ComponentPatch::Added::type);
    }
    std::vector<std::uint16_t> removed;
    for (const auto r : base.removed())
        if (r != *type) removed.push_back(r);
    return contracts::ComponentPatch::make(protocol(), std::move(added), std::move(removed), base.typed());
}

std::int32_t ComponentCodec::repair_cost_of(const contracts::ComponentPatch& patch) const {
    const auto type = type_id("repair_cost");
    if (patch.empty() || patch.protocol() != protocol() || !type) return 0;
    for (const auto& a : patch.added())
        if (a.type == *type) {
            Reader r(a.value);
            if (const auto v = r.varint()) return *v;
        }
    return 0;
}

namespace {
// A text component's plain text: its own text and its children's, in
// order, without their style (translations and other contents count as
// their key or nothing).
void plain_text(const nbt::Tag& tag, std::string& out, int depth) {
    if (depth > 32) return;
    if (tag.type() == nbt::TagType::string) {
        out += tag.text();
    } else if (tag.type() == nbt::TagType::list) {
        for (const auto& e : tag.items()) plain_text(e, out, depth + 1);
    } else if (tag.type() == nbt::TagType::compound) {
        if (const auto* text = tag.find("text"); text && text->type() == nbt::TagType::string) out += text->text();
        else if (const auto* key = tag.find("translate"); key && key->type() == nbt::TagType::string) out += key->text();
        if (const auto* extra = tag.find("extra")) plain_text(*extra, out, depth + 1);
    }
}
} // namespace

std::optional<std::string> ComponentCodec::custom_name_of(const contracts::ComponentPatch& patch) const {
    const auto type = type_id("custom_name");
    if (patch.empty() || patch.protocol() != protocol() || !type) return std::nullopt;
    for (const auto& a : patch.added()) {
        if (a.type != *type) continue;
        Reader r(a.value);
        const auto tag = nbt::read_network(r);
        if (!tag) return std::string{};
        std::string out;
        plain_text(*tag, out, 0);
        return out;
    }
    return std::nullopt;
}

std::expected<contracts::ComponentPatch, std::string> ComponentCodec::with_custom_name(const contracts::ComponentPatch& base,
                                                                                       const std::optional<std::string>& name,
                                                                                       const ComponentContext& ctx) const {
    const auto type = type_id("custom_name");
    if (!type) return std::unexpected("no custom_name component");
    std::vector<contracts::ComponentPatch::Added> added;
    for (const auto& a : base.added())
        if (a.type != *type) added.push_back(a);
    if (name) {
        Writer w;
        nbt::write_network(w, nbt::Tag::string(*name));
        auto value = canonical_value(*type, w.bytes(), ctx);
        if (!value) return std::unexpected(value.error());
        added.push_back({*type, std::move(*value)});
    }
    std::ranges::sort(added, {}, &contracts::ComponentPatch::Added::type);
    std::vector<std::uint16_t> removed;
    for (const auto r : base.removed())
        if (r != *type) removed.push_back(r);
    if (added.empty() && removed.empty()) return contracts::ComponentPatch{};
    return contracts::ComponentPatch::make(protocol(), std::move(added), std::move(removed), base.typed());
}

} // namespace circuit::protocol
