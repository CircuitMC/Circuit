// SPDX-License-Identifier: AGPL-3.0-only
// Wire codec tests. Expected encodings come from the public protocol
// documentation (minecraft.wiki "Java Edition protocol/Data types" and
// "Packets") and from the encoding definitions themselves.
#include "../support/check.hpp"
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/content.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/protocol/nbt.hpp>
#include <circuit/protocol/packets.hpp>
#include <climits>
#include <random>
#include <set>

using namespace circuit::protocol;
using circuit::test::require;
using circuit::test::Suite;

namespace {
Bytes hex(std::initializer_list<unsigned> v) {
    Bytes b;
    for (auto x : v) b.push_back(static_cast<std::byte>(x));
    return b;
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
