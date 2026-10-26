// SPDX-License-Identifier: AGPL-3.0-only
// Wire codec tests. Expected encodings come from the public protocol
// documentation (minecraft.wiki "Java Edition protocol/Data types" and
// "Packets") and from the encoding definitions themselves.
#include "../support/check.hpp"
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/components.hpp>
#include <circuit/protocol/content.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/protocol/nbt.hpp>
#include <circuit/protocol/output_queue.hpp>
#include <circuit/protocol/packets.hpp>
#include <climits>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>

using namespace circuit::protocol;
namespace contracts = circuit::contracts;
using circuit::test::require;
using circuit::test::Suite;

namespace {
Bytes hex(std::initializer_list<unsigned> v) {
    Bytes b;
    for (auto x : v) b.push_back(static_cast<std::byte>(x));
    return b;
}
Bytes from_hex(std::string_view text) {
    Bytes b;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) b.push_back(static_cast<std::byte>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return b;
}
// Stacks the official servers sent (tests/fixtures/components), as (command, slot bytes).
std::vector<std::pair<std::string, Bytes>> official_stacks(std::string_view release) {
    std::vector<std::pair<std::string, Bytes>> out;
    std::ifstream in(std::format("{}/tests/fixtures/components/java-{}.tsv", CIRCUIT_SOURCE_DIR, release));
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto tab = line.find('\t');
        out.emplace_back(line.substr(0, tab), from_hex(line.substr(tab + 1)));
    }
    return out;
}
ComponentContext open_context(std::int32_t protocol) {
    const auto* t = ContentTables::for_protocol(protocol);
    return {t->item_count(), t->block_count(), [](std::string_view) { return std::optional<std::uint32_t>(1u << 16); }};
}
template <class T, class F>
T decode_all(const Bytes& b, F f) {
    Reader r(b);
    auto v = f(r);
    require(v.has_value() && r.remaining() == 0, "decodes completely");
    return *v;
}
} // namespace

int main() {
    Suite suite("protocol_tests");

    suite.run("VarInt and VarLong encodings", [] {
        const std::vector<std::pair<std::int32_t, Bytes>> ints{
            {0, hex({0x00})}, {1, hex({0x01})}, {2, hex({0x02})}, {127, hex({0x7f})}, {128, hex({0x80, 0x01})},
            {255, hex({0xff, 0x01})}, {25565, hex({0xdd, 0xc7, 0x01})}, {2097151, hex({0xff, 0xff, 0x7f})},
            {INT32_MAX, hex({0xff, 0xff, 0xff, 0xff, 0x07})}, {-1, hex({0xff, 0xff, 0xff, 0xff, 0x0f})},
            {INT32_MIN, hex({0x80, 0x80, 0x80, 0x80, 0x08})}};
        for (const auto& [v, bytes] : ints) {
            require(Writer().varint(v).bytes() == bytes, std::format("encode {}", v));
            require(decode_all<std::int32_t>(bytes, [](Reader& r) { return r.varint(); }) == v, std::format("decode {}", v));
            require(varint_size(v) == bytes.size(), "size");
        }
        const std::vector<std::pair<std::int64_t, Bytes>> longs{
            {0, hex({0x00})}, {INT32_MAX, hex({0xff, 0xff, 0xff, 0xff, 0x07})},
            {INT64_MAX, hex({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f})},
            {-1, hex({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01})},
            {INT64_MIN, hex({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01})}};
        for (const auto& [v, bytes] : longs) {
            require(Writer().varlong(v).bytes() == bytes, std::format("encode long {}", v));
            require(decode_all<std::int64_t>(bytes, [](Reader& r) { return r.varlong(); }) == v, "decode long");
        }
        const auto six = hex({0xff, 0xff, 0xff, 0xff, 0xff, 0x01}), wide = hex({0xff, 0xff, 0xff, 0xff, 0x1f}),
                   cut = hex({0x80, 0x80});
        Reader too_long(six), extra_bits(wide), partial(cut);
        require(too_long.varint().error() == CodecError::malformed, "six-byte VarInt rejected");
        require(extra_bits.varint().error() == CodecError::malformed, "bits beyond 32 rejected");
        require(partial.varint().error() == CodecError::incomplete, "partial VarInt");
    });

    suite.run("documented position example", [] {
        // x=18357644, z=-20882616, y=831 (Data types, Position).
        const auto bits = std::string("01000110000001110110001100") + "10110000010101101101001000" + "001100111111";
        const auto expected = static_cast<std::int64_t>(std::stoull(bits, nullptr, 2));
        const WirePosition p{18357644, 831, -20882616};
        require(pack_position(p) == expected, "pack");
        require(unpack_position(expected) == p, "unpack");
        for (const auto q : {WirePosition{-33554432, -2048, -33554432}, WirePosition{33554431, 2047, 33554431}, WirePosition{-1, -1, -1}})
            require(packable(q) && unpack_position(pack_position(q)) == q, "range ends round-trip");
        require(!packable({33554432, 0, 0}) && !packable({0, 2048, 0}), "outside the packed range");
    });

    suite.run("strings and identifiers", [] {
        const std::string emoji = "a\xF0\x9F\x98\x80"; // 1 + 2 UTF-16 units
        require(utf16_units(emoji).value() == 3, "supplementary characters count twice");
        const auto b = Writer().string(emoji, 3).take();
        Reader r(b);
        require(r.string(3).value() == emoji, "round trip at the limit");
        Reader over(b);
        require(over.string(2).error() == CodecError::too_large, "unit limit");
        auto big = Writer().varint(16 * 3 + 1).take();
        big.resize(big.size() + 49, std::byte{'a'});
        Reader bytes_over(big);
        require(bytes_over.string(16).error() == CodecError::too_large, "byte limit checked first");
        for (const char* bad : {"\xC0\x80", "\xED\xA0\x80", "\xF8\x88\x80\x80\x80", "\xE2\x82"}) {
            const std::string s(bad);
            require(!utf16_units(s), std::format("invalid UTF-8 {} rejected", s.size()));
        }
        require(valid_identifier("minecraft:stone") && valid_identifier("stone") && valid_identifier("a.b-c_d:x/y"), "valid ids");
        require(!valid_identifier("Minecraft:stone") && !valid_identifier("a:") && !valid_identifier("a:b c")
                    && !valid_identifier("a/b:c") && !valid_identifier(""),
                "invalid ids");
        require(Uuid{0x0123456789abcdefULL, 0xfedcba9876543210ULL}.text() == "01234567-89ab-cdef-fedc-ba9876543210", "uuid text");
    });

    suite.run("NBT round trips and limits", [] {
        auto root = nbt::Tag::compound();
        root.set("byte", nbt::Tag::byte(-3));
        root.set("long", nbt::Tag::long_(INT64_MIN));
        root.set("double", nbt::Tag::double_(0.25));
        root.set("text", nbt::Tag::string(std::string("nul\0and \xF0\x9F\x98\x80", 12)));
        root.set("ints", nbt::Tag::int_array({1, -2, 3}));
        root.set("longs", nbt::Tag::long_array({INT64_MAX}));
        auto list = nbt::Tag::list(nbt::TagType::compound);
        list.push(nbt::Tag::compound()).set("k", nbt::Tag::short_(7));
        root.set("list", std::move(list));
        root.set("empty", nbt::Tag::list(nbt::TagType::end));

        Writer w;
        nbt::write_network(w, root);
        Reader r(w.bytes());
        require(nbt::read_network(r).value() == root && r.remaining() == 0, "network round trip");
        Writer named;
        nbt::write_named(named, "Level", root);
        Reader rn(named.bytes());
        std::string name;
        require(nbt::read_named(rn, name).value() == root && name == "Level", "named round trip");
        // Modified UTF-8: U+0000 is two bytes and supplementary characters are surrogate pairs.
        const auto m = nbt::to_modified_utf8(std::string("\0", 1));
        require(m == "\xC0\x80", "modified null");
        require(nbt::to_modified_utf8("\xF0\x9F\x98\x80").size() == 6, "surrogate pair");
        require(nbt::from_modified_utf8(as_bytes("\xED\xA0\xBD\xED\xB8\x80")).value() == "\xF0\x9F\x98\x80", "pair decodes");

        // Truncation anywhere fails cleanly; deep nesting is limited.
        for (std::size_t n = 0; n < w.size(); ++n) {
            Reader t(std::span<const std::byte>(w.bytes()).first(n));
            require(!nbt::read_network(t).has_value(), "truncated input rejected");
        }
        Writer deep;
        deep.u8(9);
        for (int i = 0; i < 600; ++i) deep.u8(9).i32(1);
        deep.u8(0).i32(0);
        Reader dr(deep.bytes());
        require(nbt::read_network(dr).error() == CodecError::too_large, "depth limit");
        Writer huge;
        huge.u8(12).i32(INT32_MAX);
        Reader hr(huge.bytes());
        require(!nbt::read_network(hr).has_value(), "array length beyond the input");
        bool threw = false;
        try {
            auto l = nbt::Tag::list(nbt::TagType::int_);
            l.push(nbt::Tag::byte(1));
        } catch (const std::logic_error&) {
            threw = true;
        }
        require(threw, "list element type enforced");
    });

    suite.run("framing with and without compression", [] {
        const auto packet = Writer().varint(0x08).i64(42).take();
        const auto frame = encode_frame(packet);
        FrameDecoder d;
        for (auto byte : frame) {
            require(d.feed(std::span<const std::byte>(&byte, 1)).has_value(), "feed");
        }
        require(d.next().value().value() == packet && !d.next().value().has_value(), "byte-by-byte input");

        Bytes big = Writer().varint(1).take();
        big.resize(1000, std::byte{7});
        FrameDecoder c;
        c.set_compression(256);
        Bytes two = encode_frame(packet, 256);
        const auto compressed = encode_frame(big, 256);
        require(compressed.size() < big.size(), "large packet compressed");
        two.insert(two.end(), compressed.begin(), compressed.end());
        require(c.feed(two).has_value(), "feed");
        require(c.next().value().value() == packet, "small packet stays uncompressed");
        require(c.next().value().value() == big, "large packet inflates");

        const auto reject = [](Bytes frame, std::int32_t threshold, CodecError e, std::string_view what) {
            FrameDecoder f;
            f.set_compression(threshold);
            require(f.feed(frame).has_value(), "feed");
            const auto r = f.next();
            require(!r.has_value() && r.error() == e, what);
        };
        // An uncompressed packet at or above the threshold is malformed.
        Bytes uncompressed_big = Writer().varint(0).take();
        uncompressed_big.insert(uncompressed_big.end(), big.begin(), big.end());
        reject(Writer().varint(static_cast<std::int32_t>(uncompressed_big.size())).raw(uncompressed_big).take(), 256,
               CodecError::malformed, "uncompressed above threshold");
        // A declared size that differs from the stream (a decompression bomb) is malformed.
        const auto deflated = deflate_bytes(big);
        auto lying = Writer().varint(500).raw(deflated).take();
        reject(Writer().varint(static_cast<std::int32_t>(lying.size())).raw(lying).take(), 256, CodecError::malformed,
               "declared size mismatch");
        auto oversized = Writer().varint(9'000'000).raw(deflated).take();
        reject(Writer().varint(static_cast<std::int32_t>(oversized.size())).raw(oversized).take(), 256,
               CodecError::too_large, "uncompressed limit");
        reject(Writer().varint(2097152).take(), -1, CodecError::too_large, "frame limit");
        reject(hex({0x80, 0x80, 0x80, 0x01}), -1, CodecError::too_large, "four-byte frame length");
        reject(hex({0x00}), -1, CodecError::malformed, "empty frame");
    });

    suite.run("output frames stay alive until their write completes", [] {
        OutputQueue q;
        q.push(Bytes(100, std::byte{1}));
        q.push(Bytes(50, std::byte{2}));
        require(q.bytes() == 150 && !q.writing(), "two frames wait");
        auto first = q.begin();
        require(first && first->size() == 100 && q.writing() && q.waiting() == 1, "the first frame is active");
        require(!q.begin(), "one write at a time");
        // Normal completion releases the active frame and its bytes.
        q.finish(first);
        require(q.bytes() == 50 && !q.writing(), "first write accounted");
        first.reset();

        // Closing discards the queue while a write is outstanding: the handler
        // still owns the buffer, and its completion changes nothing.
        auto second = q.begin();
        const std::weak_ptr<const Bytes> watch = second;
        const auto* data = second->data();
        q.push(Bytes(10, std::byte{3}));
        q.discard();
        require(q.bytes() == 0 && q.waiting() == 0 && !q.writing(), "queue discarded");
        require(!watch.expired() && watch.lock()->data() == data && watch.lock()->size() == 50,
                "the outstanding write's buffer is unchanged");
        q.finish(second); // a completion already queued before the close
        require(q.bytes() == 0, "a late completion does not underflow the count");
        second.reset();
        require(watch.expired(), "the buffer is released by its handler");

        // A stale completion cannot release a newer active frame.
        q.push(Bytes(7, std::byte{4}));
        auto stale = std::make_shared<const Bytes>(Bytes(7, std::byte{4}));
        auto third = q.begin();
        q.finish(stale);
        require(q.writing() && q.bytes() == 7, "only the active frame's completion counts");
        q.finish(third);
        require(!q.writing() && q.bytes() == 0, "drained");
        q.finish(nullptr);
    });

    suite.run("documented packet ids", [] {
        const auto* ids = PacketIds::for_protocol(772);
        require(ids && ids->documentation_revision() == 3258009, "1.21.8 table");
        require(ids->id(State::handshaking, Bound::serverbound, "intention") == 0x00, "handshake");
        require(ids->id(State::play, Bound::clientbound, "block_update") == 0x08, "block update");
        require(ids->id(State::play, Bound::clientbound, "level_chunk_with_light") == 0x27, "chunk data");
        require(ids->id(State::play, Bound::clientbound, "login") == 0x2b, "play login");
        require(ids->id(State::configuration, Bound::clientbound, "registry_data") == 0x07, "registry data");
        require(ids->name(State::play, Bound::clientbound, 0x08) == "block_update", "reverse lookup");
        require(!ids->find(State::play, Bound::clientbound, "no_such_packet"), "unknown name");
        bool threw = false;
        try {
            (void)ids->id(State::login, Bound::serverbound, "no_such_packet");
        } catch (const std::out_of_range&) {
            threw = true;
        }
        require(threw, "unknown packet fails early");
        require(PacketIds::for_protocol(769)->documentation_revision() == 2938097, "1.21.4 table");
        require(!PacketIds::for_protocol(1), "undocumented protocol");
    });

    suite.run("block-state palette and item ids", [] {
        for (const std::int32_t protocol : {772, 769}) {
            const auto* c = ContentTables::for_protocol(protocol);
            require(c != nullptr, "tables exist");
            // Every state id round-trips through its full notation.
            for (std::int32_t id = 0; id < c->state_count(); ++id) {
                const auto n = c->notation(id);
                require(n && c->state_id(*n) == id, std::format("state {} round trip", id));
            }
            require(!c->state(c->state_count()) && !c->state(-1), "out of range");
        }
        const auto* c = ContentTables::for_protocol(772);
        require(c->state_count() == 27946 && c->state_id("minecraft:air") == 0 && c->state_id("stone") == 1, "air and stone");
        // Default states listed by the dataset.
        require(c->default_state("minecraft:repeater") == 6063 && c->default_state("minecraft:lever") == 5811
                    && c->default_state("minecraft:observer") == 13578,
                "documented defaults");
        require(c->state_id("minecraft:repeater[delay=1,facing=north,locked=false,powered=false]") == 6063, "explicit default");
        require(c->state_id("minecraft:repeater[delay=4,facing=east,locked=false,powered=false]") == 6060 + 63, "last state (true sorts first)");
        require(c->state_id("minecraft:lever[powered=true]") == 5810, "unlisted properties take defaults");
        require(!c->state_id("minecraft:lever[colour=red]") && !c->state_id("minecraft:nonexistent"), "unknown");
        require(c->notation(6063) == "minecraft:repeater[delay=1,facing=north,locked=false,powered=false]", "notation");
        require(c->item_name(0) == "minecraft:air" && c->item_id("minecraft:redstone").has_value()
                    && c->item_id("redstone") == c->item_id("minecraft:redstone"),
                "items");
        require(c->entity_type("minecraft:item").has_value() && c->entity_type("minecraft:player").has_value(), "entity types");
    });

    suite.run("item components the official servers sent read, check and write back", [] {
        for (const auto& [protocol, release] : {std::pair{772, "1.21.8"}, std::pair{769, "1.21.4"}}) {
            const auto& codec = *ComponentCodec::for_protocol(protocol);
            const auto ctx = open_context(protocol);
            std::set<std::uint16_t> types;
            std::map<std::string, contracts::ComponentPatch> by_command;
            const auto stacks = official_stacks(release);
            for (const auto& [command, bytes] : stacks) {
                Reader r(bytes);
                const auto count = r.varint();
                const auto item = r.varint();
                const auto patch = codec.read(r, ctx);
                require(count && item && patch && r.remaining() == 0,
                        std::format("{}: {}: {}", release, command, patch ? "trailing bytes" : patch.error()));
                // The canonical form has the same size and reads back to the same patch.
                Writer w;
                w.varint(*count).varint(*item);
                codec.write(w, patch->damage, patch->rest);
                require(w.size() == bytes.size(), std::format("{}: {}: canonical size", release, command));
                Reader again(w.bytes());
                (void)again.varint();
                (void)again.varint();
                const auto back = codec.read(again, ctx);
                require(back && back->rest == patch->rest && back->damage == patch->damage, "canonical form reads back");
                require(codec.check(patch->rest, ctx).value() == patch->rest, "a canonical patch checks");
                // A stack given twice is the same, whatever order the server sent its maps in.
                if (const auto [it, fresh] = by_command.emplace(command, patch->rest); !fresh)
                    require(it->second == patch->rest, std::format("{}: {}: same stack, same components", release, command));
                for (const auto& a : patch->rest.added()) types.insert(a.type);
                for (const auto t : patch->rest.removed()) types.insert(t);
                if (patch->damage) types.insert(*codec.type_id("damage"));
            }
            require(stacks.size() >= 26, "every recorded stack");
            std::cout << std::format("  {}: {} stacks with {} of {} component types\n", release, stacks.size(), types.size(), codec.type_count());
        }
    });

    suite.run("item components are canonical and checked", [] {
        const auto& codec = *ComponentCodec::for_protocol(772);
        const auto ctx = open_context(772);
        const auto read = [&](const Bytes& b, const ComponentContext& c, bool prefixed = false) {
            Reader r(b);
            auto p = codec.read(r, c, prefixed);
            if (p && r.remaining() != 0) return std::expected<WirePatch, std::string>(std::unexpected("trailing"));
            return p;
        };
        const auto custom_data = *codec.type_id("custom_data"), max_stack = *codec.type_id("max_stack_size"),
                   damage = *codec.type_id("damage"), enchantments = *codec.type_id("enchantments");
        // custom_data {b:1b, a:1b} and max_stack_size 16, in either order.
        const auto compound_ba = hex({0x0A, 0x01, 0x00, 0x01, 'b', 0x01, 0x01, 0x00, 0x01, 'a', 0x01, 0x00});
        Writer one, two;
        one.varint(2).varint(0).varint(custom_data).raw(compound_ba).varint(max_stack).varint(16);
        two.varint(2).varint(0).varint(max_stack).varint(16).varint(custom_data).raw(compound_ba);
        const auto a = read(one.bytes(), ctx), b = read(two.bytes(), ctx);
        require(a && b && a->rest == b->rest && a->rest.typed().max_stack_size == 16, "order does not matter");
        const auto value = a->rest.added()[0].value;
        require(value.size() == compound_ba.size() && std::to_integer<char>(value[4]) == 'a', "compound keys in order");
        // Damage is kept apart and written back among the others.
        Writer d;
        d.varint(2).varint(0).varint(max_stack).varint(1).varint(damage).varint(9);
        const auto with_damage = read(d.bytes(), ctx);
        require(with_damage && with_damage->damage == 9 && with_damage->rest.added().size() == 1, "damage apart");
        Writer back;
        codec.write(back, with_damage->damage, with_damage->rest);
        Writer sorted;
        sorted.varint(2).varint(0).varint(max_stack).varint(1).varint(damage).varint(9);
        require(back.bytes() == sorted.bytes(), "damage in type order");
        // Malformed and refused patches.
        const auto refused = [&](const Writer& w, std::string_view why, const ComponentContext& c, bool prefixed = false) {
            const auto r = read(w.bytes(), c, prefixed);
            require(!r && r.error().find(why) != std::string::npos, std::format("expected `{}`, got `{}`", why, r ? "ok" : r.error()));
        };
        Writer twice, both, unknown, range, too_big;
        twice.varint(2).varint(0).varint(max_stack).varint(2).varint(max_stack).varint(3);
        refused(twice, "given twice", ctx);
        both.varint(1).varint(1).varint(max_stack).varint(2).varint(max_stack);
        refused(both, "added and removed", ctx);
        unknown.varint(1).varint(0).varint(static_cast<std::int32_t>(codec.type_count()));
        refused(unknown, "unknown component type", ctx);
        too_big.varint(1).varint(0).varint(max_stack).varint(100);
        refused(too_big, "outside 1-99", ctx);
        // An enchantment refers to a registry the server must synchronize.
        range.varint(1).varint(0).varint(enchantments).varint(1).varint(0).varint(1);
        refused(range, "enchantment registry", ComponentContext{ctx.items, ctx.blocks, {}});
        require(read(range.bytes(), ctx).has_value(), "with the registry synchronized");
        Writer far;
        far.varint(1).varint(0).varint(enchantments).varint(1).varint(1 << 16).varint(1);
        refused(far, "outside the registry", ctx);
        // Potions are a static registry of 46 entries in both versions.
        const auto none = ComponentContext{ctx.items, ctx.blocks, {}};
        Writer potion, beyond;
        potion.varint(1).varint(0).varint(*codec.type_id("potion_contents")).u8(1).varint(45).u8(0).varint(0).u8(0);
        require(read(potion.bytes(), none).has_value(), "the last potion");
        beyond.varint(1).varint(0).varint(*codec.type_id("potion_contents")).u8(1).varint(46).u8(0).varint(0).u8(0);
        refused(beyond, "potion id 46 outside the registry", none);
        // An enumeration value outside its mapping.
        Writer rarity;
        rarity.varint(1).varint(0).varint(*codec.type_id("rarity")).varint(4);
        refused(rarity, "outside its enumeration", ctx);
        // Length-prefixed values (1.21.5+ creative slots) must use their bytes exactly.
        Writer prefixed, short_value;
        prefixed.varint(1).varint(0).varint(max_stack).varint(1).varint(64);
        require(read(prefixed.bytes(), ctx, true)->rest.typed().max_stack_size == 64, "length-prefixed value");
        short_value.varint(1).varint(0).varint(max_stack).varint(2).varint(64).u8(0);
        refused(short_value, "trailing bytes", ctx, true);
        // Every truncation of a recorded stack is refused.
        for (const auto& [command, bytes] : official_stacks("1.21.8"))
            for (std::size_t n = 2; n < bytes.size(); n += 3) {
                Reader r(std::span<const std::byte>(bytes).first(n));
                (void)r.varint();
                (void)r.varint();
                const auto p = codec.read(r, ctx);
                require(!p || r.remaining() == 0, "a truncated stack never reads with bytes left");
            }
    });

    suite.run("save forms name registry entries", [] {
        const auto& codec = *ComponentCodec::for_protocol(772);
        auto ctx = open_context(772);
        const std::vector<std::string> enchantments{"minecraft:mending", "minecraft:sharpness"};
        ctx.name_of = [&](std::string_view registry, std::uint32_t id) -> std::optional<std::string_view> {
            if (registry == "enchantment" && id < enchantments.size()) return enchantments[id];
            if (registry == "trim_material" && id == 0) return "minecraft:gold";
            if (registry == "trim_pattern" && id == 0) return "minecraft:coast";
            return std::nullopt;
        };
        ctx.id_of = [&](std::string_view registry, std::string_view name) -> std::optional<std::uint32_t> {
            if (registry == "enchantment")
                for (std::uint32_t i = 0; i < enchantments.size(); ++i)
                    if (enchantments[i] == name) return i;
            if ((registry == "trim_material" && name == "minecraft:gold") || (registry == "trim_pattern" && name == "minecraft:coast")) return 0;
            return std::nullopt;
        };
        auto levels = nbt::Tag::compound();
        levels.set("minecraft:sharpness", nbt::Tag::int_(5));
        levels.set("minecraft:mending", nbt::Tag::int_(1));
        const auto type = *codec.type_id("enchantments");
        const auto value = codec.from_save(type, levels, ctx);
        require(value.has_value(), value ? "" : value.error());
        const auto back = codec.to_save(type, *value, ctx).value();
        require(back.find("minecraft:sharpness")->integer() == 5 && back.find("minecraft:mending")->integer() == 1, "levels round-trip");
        auto unknown = levels;
        unknown.set("minecraft:nonsense", nbt::Tag::int_(1));
        require(!codec.from_save(type, unknown, ctx).has_value(), "unknown enchantment");
        auto none = ctx;
        none.synced = [](std::string_view r) -> std::optional<std::uint32_t> { return r == "enchantment" ? std::nullopt : std::optional(64u); };
        require(codec.from_save(type, levels, none).error().find("not synchronized") != std::string::npos, "without the registry");
        auto trim = nbt::Tag::compound();
        trim.set("material", nbt::Tag::string("minecraft:gold"));
        trim.set("pattern", nbt::Tag::string("minecraft:coast"));
        const auto t = codec.from_save(*codec.type_id("trim"), trim, ctx).value();
        require(codec.to_save(*codec.type_id("trim"), t, ctx).value() == trim, "trim round-trips");
        // A smithing table's trim, set on a stack with other components and
        // replacing its trim; on both protocols (1.21.4's tooltip flag shown).
        for (const std::int32_t protocol : {769, 772}) {
            const auto& c = *ComponentCodec::for_protocol(protocol);
            auto pctx = open_context(protocol);
            pctx.name_of = ctx.name_of;
            pctx.id_of = ctx.id_of;
            const auto made = c.enchanted(std::vector<std::pair<std::uint32_t, std::int32_t>>{{1, 5}}, pctx);
            require(made.has_value(), std::format("{}: {}", protocol, made ? "" : made.error()));
            const auto& enchanted = *made;
            const auto trimmed = c.with_trim(enchanted, 0, 0, pctx);
            require(trimmed.has_value(), trimmed ? "" : trimmed.error());
            require(trimmed->added().size() == 2 && c.enchantment_level(*trimmed, 1) == 5, std::format("{}: the enchantment kept", protocol));
            const auto trim_type = *c.type_id("trim");
            const auto it = std::ranges::find(trimmed->added(), trim_type, &circuit::contracts::ComponentPatch::Added::type);
            // Both holders as references to entry 0 (id + 1), then 1.21.4's flag.
            const std::vector<std::byte> expected = protocol == 769 ? std::vector{std::byte{1}, std::byte{1}, std::byte{1}}
                                                                    : std::vector{std::byte{1}, std::byte{1}};
            require(it != trimmed->added().end() && it->value == expected, std::format("{}: the trim's value", protocol));
            const auto again = c.with_trim(*trimmed, 0, 0, pctx);
            require(again.has_value() && *again == *trimmed, std::format("{}: the same trim again changes nothing", protocol));
        }
    });

    suite.run("random input never escapes its bounds", [] {
        std::mt19937_64 rng(1234);
        for (int round = 0; round < 3000; ++round) {
            Bytes b(rng() % 64);
            for (auto& x : b) x = static_cast<std::byte>(rng());
            FrameDecoder d;
            d.set_compression(round % 2 ? 64 : -1);
            if (!d.feed(b)) continue;
            for (int i = 0; i < 8; ++i) {
                const auto r = d.next();
                if (!r || !r->has_value()) break;
            }
            Reader r1(b);
            (void)nbt::read_network(r1);
            Reader r2(b);
            for (int i = 0; i < 8 && r2.remaining(); ++i) {
                if (!r2.string(16) && !r2.varint()) break;
            }
            Reader r3(b);
            (void)r3.length(1000, 4);
        }
    });

    return suite.finish();
}
