// SPDX-License-Identifier: AGPL-3.0-only
#include "registries.hpp"
#include "world_codec.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <circuit/evidence/content.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/rules.hpp>
#include <circuit/protocol/content.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/protocol/nbt.hpp>
#include <circuit/server/anvil.hpp>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <unordered_map>
#include <zlib.h>

namespace circuit::server::anvil {
namespace nbt = protocol::nbt;
using contracts::BlockPos;
using contracts::ChunkPos;
using gameplay::BlockEntity;
using gameplay::BlockState;
using gameplay::BlockType;
using gameplay::WorldDump;
using nbt::Tag;
using nbt::TagType;
namespace fs = std::filesystem;

namespace {
constexpr std::size_t sector = 4096;
constexpr std::size_t max_region_bytes = std::size_t{256} << 20;
constexpr std::size_t max_level_bytes = std::size_t{16} << 20;
constexpr std::int32_t level_dat_version = 19133;

// ---- bytes, compression and files -----------------------------------------

std::uint32_t be32(std::span<const std::byte> b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v = (v << 8) | std::to_integer<std::uint32_t>(b[at + i]);
    return v;
}
void put_be32(std::vector<std::byte>& out, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) out[at + i] = static_cast<std::byte>((v >> (24 - 8 * i)) & 0xFF);
}

std::expected<std::vector<std::byte>, std::string> read_file(const fs::path& p, std::size_t max) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return std::unexpected(std::format("cannot read {}", p.string()));
    const auto size = static_cast<std::size_t>(in.tellg());
    if (size > max) return std::unexpected(std::format("{} is larger than {} bytes", p.string(), max));
    std::vector<std::byte> data(size);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
        return std::unexpected(std::format("cannot read {}", p.string()));
    return data;
}
std::expected<void, std::string> write_file(const fs::path& p, std::span<const std::byte> data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())) || !out.flush())
        return std::unexpected(std::format("cannot write {}", p.string()));
    return {};
}

// Inflates a zlib or gzip stream of unknown size, up to `max` bytes.
std::expected<std::vector<std::byte>, std::string> inflate_any(std::span<const std::byte> in, std::size_t max) {
    z_stream z{};
    if (inflateInit2(&z, 15 + 32) != Z_OK) return std::unexpected("inflate init failed");
    std::vector<std::byte> out;
    std::array<std::byte, 65536> buf{};
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size(), std::numeric_limits<uInt>::max()));
    int rc = Z_OK;
    while (rc == Z_OK) {
        z.next_out = reinterpret_cast<Bytef*>(buf.data());
        z.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        const auto n = buf.size() - z.avail_out;
        if (out.size() + n > max) {
            inflateEnd(&z);
            return std::unexpected(std::format("data expands beyond {} bytes", max));
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
        if (rc == Z_OK && n == 0 && z.avail_in == 0) break;
    }
    inflateEnd(&z);
    if (rc != Z_STREAM_END) return std::unexpected("truncated or corrupt compressed data");
    return out;
}
std::vector<std::byte> gzip(std::span<const std::byte> in) {
    z_stream z{};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("anvil: deflate init failed");
    std::vector<std::byte> out(deflateBound(&z, static_cast<uLong>(in.size())) + 32);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    const auto rc = deflate(&z, Z_FINISH);
    out.resize(out.size() - z.avail_out);
    deflateEnd(&z);
    if (rc != Z_STREAM_END) throw std::runtime_error("anvil: deflate failed");
    return out;
}

std::expected<Tag, std::string> parse_nbt(std::span<const std::byte> data, std::size_t max_bytes) {
    protocol::Reader r(data);
    std::string name;
    auto tag = nbt::read_named(r, name, {.max_depth = 512, .max_bytes = max_bytes});
    if (!tag || tag->type() != TagType::compound) return std::unexpected("malformed NBT");
    return std::move(*tag);
}
std::vector<std::byte> named_nbt(const Tag& root) {
    protocol::Writer w;
    nbt::write_named(w, "", root);
    return w.take();
}

// ---- NBT access -------------------------------------------------------------

const Tag* child(const Tag& c, std::string_view key, TagType type) {
    const auto* t = c.find(key);
    return t && t->type() == type ? t : nullptr;
}
bool is_integral(TagType t) { return t == TagType::byte_ || t == TagType::short_ || t == TagType::int_ || t == TagType::long_; }
std::optional<std::int64_t> integer(const Tag& c, std::string_view key) {
    const auto* t = c.find(key);
    if (!t || !is_integral(t->type())) return std::nullopt;
    return t->integer();
}
std::optional<double> real(const Tag& c, std::string_view key) {
    const auto* t = c.find(key);
    if (!t) return std::nullopt;
    if (t->type() == TagType::float_ || t->type() == TagType::double_) return t->real();
    if (is_integral(t->type())) return static_cast<double>(t->integer());
    return std::nullopt;
}
std::optional<std::string> text(const Tag& c, std::string_view key) {
    const auto* t = child(c, key, TagType::string);
    return t ? std::optional<std::string>(t->text()) : std::nullopt;
}
// A list of exactly `n` numbers.
std::optional<std::vector<double>> numbers(const Tag& c, std::string_view key, std::size_t n) {
    const auto* t = child(c, key, TagType::list);
    if (!t || t->items().size() != n) return std::nullopt;
    std::vector<double> out;
    for (const auto& item : t->items()) {
        if (item.type() != TagType::double_ && item.type() != TagType::float_) return std::nullopt;
        out.push_back(item.real());
    }
    return out;
}


// "name[key=value,...]" with properties in key order, for the content mapping.
std::string notation_of(const Tag& state) {
    auto name = text(state, "Name").value_or("");
    const auto* props = child(state, "Properties", TagType::compound);
    if (!props || props->keys().empty()) return name;
    std::vector<std::pair<std::string, std::string>> kv;
    for (std::size_t i = 0; i < props->keys().size(); ++i)
        kv.emplace_back(props->keys()[i], props->items()[i].type() == TagType::string ? props->items()[i].text() : "?");
    std::ranges::sort(kv);
    name += '[';
    for (std::size_t i = 0; i < kv.size(); ++i) name += std::format("{}{}={}", i ? "," : "", kv[i].first, kv[i].second);
    return name + ']';
}
Tag state_tag(const protocol::BlockStateInfo& info) {
    auto t = Tag::compound();
    t.set("Name", Tag::string(std::string(info.block)));
    if (!info.properties.empty()) {
        auto props = Tag::compound();
        for (const auto& [k, v] : info.properties) props.set(k, Tag::string(v));
        t.set("Properties", std::move(props));
    }
    return t;
}

std::optional<std::pair<std::int32_t, std::int32_t>> region_coords(const std::string& file) {
    static const std::regex pattern(R"(r\.(-?\d+)\.(-?\d+)\.mca)");
    std::smatch m;
    if (!std::regex_match(file, m, pattern)) return std::nullopt;
    try {
        return std::pair{std::stoi(m[1]), std::stoi(m[2])};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// ---- region files -----------------------------------------------------------

struct StoredChunk {
    ChunkPos pos;
    Tag root;
};

// Reads every chunk of one region file. Fails on any malformed entry.
std::expected<std::vector<StoredChunk>, std::string> read_region(const fs::path& file, std::int32_t rx, std::int32_t rz,
                                                                  std::size_t max_chunk_bytes) {
    auto data = read_file(file, max_region_bytes);
    if (!data) return std::unexpected(data.error());
    const std::span<const std::byte> d(*data);
    std::vector<StoredChunk> out;
    if (d.empty()) return out; // an empty region file holds no chunks
    if (d.size() < 2 * sector) return std::unexpected(std::format("{}: header truncated", file.string()));
    for (std::size_t k = 0; k < 1024; ++k) {
        const auto location = be32(d, 4 * k);
        if (location == 0) continue;
        const ChunkPos pos{rx * 32 + static_cast<std::int32_t>(k % 32), rz * 32 + static_cast<std::int32_t>(k / 32)};
        const auto where = std::format("{} chunk {} {}", file.filename().string(), pos.x, pos.z);
        const auto offset = static_cast<std::size_t>(location >> 8) * sector;
        if (offset < 2 * sector || offset + 5 > d.size()) return std::unexpected(where + ": offset outside the file");
        const auto length = be32(d, offset);
        if (length < 1 || offset + 4 + length > d.size()) return std::unexpected(where + ": length outside the file");
        auto type = std::to_integer<std::uint8_t>(d[offset + 4]);
        std::vector<std::byte> external;
        std::span<const std::byte> payload = d.subspan(offset + 5, length - 1);
        if (type & 128) {
            auto ext = read_file(file.parent_path() / std::format("c.{}.{}.mcc", pos.x, pos.z), max_region_bytes);
            if (!ext) return std::unexpected(where + ": " + ext.error());
            external = std::move(*ext);
            payload = external;
            type &= 127;
        }
        std::expected<std::vector<std::byte>, std::string> raw;
        if (type == 1 || type == 2) raw = inflate_any(payload, max_chunk_bytes);
        else if (type == 3) raw = std::vector<std::byte>(payload.begin(), payload.end());
        else if (type == 4) return std::unexpected(where + ": LZ4 compression is not supported; save the world with zlib");
        else return std::unexpected(std::format("{}: unsupported compression type {}", where, type));
        if (!raw) return std::unexpected(where + ": " + raw.error());
        auto root = parse_nbt(*raw, max_chunk_bytes);
        if (!root) return std::unexpected(where + ": " + root.error());
        out.push_back({pos, std::move(*root)});
    }
    return out;
}

// Writes chunks (all of one region) as a region file with zlib compression;
// a chunk over 255 sectors goes to its own c.<x>.<z>.mcc file.
std::expected<void, std::string> write_region(const fs::path& file, const std::vector<std::pair<ChunkPos, Tag>>& chunks) {
    std::vector<std::byte> out(2 * sector);
    for (const auto& [pos, root] : chunks) {
        const auto compressed = protocol::deflate_bytes(named_nbt(root));
        const auto k = static_cast<std::size_t>((pos.x & 31) + (pos.z & 31) * 32);
        const auto offset = out.size() / sector;
        std::uint8_t type = 2;
        std::span<const std::byte> body(compressed);
        if (compressed.size() + 5 > 255 * sector) {
            if (auto ok = write_file(file.parent_path() / std::format("c.{}.{}.mcc", pos.x, pos.z), compressed); !ok) return ok;
            type = 2 | 128;
            body = {};
        }
        const auto start = out.size();
        out.resize(start + 5 + body.size());
        put_be32(out, start, static_cast<std::uint32_t>(body.size() + 1));
        out[start + 4] = static_cast<std::byte>(type);
        std::ranges::copy(body, out.begin() + static_cast<std::ptrdiff_t>(start + 5));
        out.resize((out.size() + sector - 1) / sector * sector);
        const auto sectors = (out.size() - start) / sector;
        if (offset > 0xFFFFFF) return std::unexpected("region file too large");
        put_be32(out, 4 * k, static_cast<std::uint32_t>((offset << 8) | sectors));
    }
    return write_file(file, out);
}

// ---- content mapping ----------------------------------------------------------

class Content {
public:
    Content(const contracts::VersionBundle& bundle, const std::optional<protocol::ComponentContext>& components)
        : tables_(protocol::ContentTables::for_protocol(772)),
          mapping_(evidence::ContentMapping::for_game_version(bundle.game_version)),
          codec_(protocol::ComponentCodec::for_protocol(772)), ctx_(components ? *components : server::component_context(772)) {
        if (bundle.game_version != "1.21.8" || !tables_ || !mapping_ || !codec_)
            throw std::invalid_argument(std::format("Anvil import and export support java-1.21.8 worlds only, not {}", bundle.id));
        palette_.emplace(*tables_, *mapping_);
    }
    // A component map of the save format ("minecraft:name": value, and
    // "!minecraft:name": {} for a removed default), with `extra` values
    // already in network form, as a canonical patch; the damage comes apart.
    std::expected<protocol::WirePatch, std::string> patch(const Tag* components,
                                                         std::vector<contracts::ComponentPatch::Added> extra = {}) const {
        auto added = std::move(extra);
        std::vector<std::uint16_t> removed;
        std::optional<std::int32_t> damage;
        if (components) {
            if (components->type() != TagType::compound) return std::unexpected("malformed components");
            for (std::size_t i = 0; i < components->keys().size(); ++i) {
                std::string_view key = components->keys()[i];
                const bool remove = key.starts_with('!');
                if (remove) key.remove_prefix(1);
                const auto type = key.starts_with("minecraft:") ? codec_->type_id(key) : std::nullopt;
                if (!type) return std::unexpected(std::format("item component {}", key));
                if (remove) {
                    removed.push_back(*type);
                } else if (key == "minecraft:damage") {
                    const auto& v = components->items()[i];
                    if (v.type() != TagType::int_ || v.integer() < 0 || v.integer() > INT32_MAX)
                        return std::unexpected("item component minecraft:damage with an invalid value");
                    damage = static_cast<std::int32_t>(v.integer());
                } else {
                    auto value = codec_->from_save(*type, components->items()[i], ctx_);
                    if (!value) return std::unexpected(value.error());
                    added.push_back({*type, std::move(*value)});
                }
            }
        }
        // Read back as a client's would be: canonical order and typed fields.
        protocol::Writer w;
        w.varint(static_cast<std::int32_t>(added.size())).varint(static_cast<std::int32_t>(removed.size()));
        for (const auto& a : added) w.varint(a.type).raw(a.value);
        for (const auto t : removed) w.varint(t);
        protocol::Reader r(w.bytes());
        auto out = codec_->read(r, ctx_);
        if (!out) return std::unexpected(out.error());
        if (damage) out->damage = damage;
        return out;
    }
    // A text component in network form as the value of custom_name.
    std::expected<contracts::ComponentPatch::Added, std::string> custom_name(const Tag& text) const {
        const auto type = *codec_->type_id("custom_name");
        auto value = codec_->from_save(type, text, ctx_);
        if (!value) return std::unexpected(value.error());
        return contracts::ComponentPatch::Added{type, std::move(*value)};
    }
    // Writes a patch back as a save-format component map; an error names a
    // component without a save form yet. custom_name goes to `name` instead
    // when given.
    std::expected<void, std::string> save(const contracts::ComponentPatch& patch, std::optional<std::int32_t> damage, Tag& out,
                                          std::optional<Tag>* name = nullptr) const {
        auto c = Tag::compound();
        if (damage) c.set("minecraft:damage", Tag::int_(*damage));
        for (const auto& a : patch.added()) {
            auto value = codec_->to_save(a.type, a.value, ctx_);
            if (!value) return std::unexpected(value.error());
            if (name && codec_->type_name(a.type) == "custom_name") *name = std::move(*value);
            else c.set(std::format("minecraft:{}", codec_->type_name(a.type)), std::move(*value));
        }
        for (const auto t : patch.removed()) c.set(std::format("!minecraft:{}", codec_->type_name(t)), Tag::compound());
        out = std::move(c);
        return {};
    }
    // The stack an item tag ("id", "count", "components") describes; an error
    // names the unsupported content.
    std::expected<contracts::ItemStack, std::string> stack(const Tag& item) const {
        const auto id = text(item, "id").value_or("");
        const auto mapped = mapping_->item(id);
        if (!mapped) return std::unexpected("item " + id);
        const auto count = integer(item, "count").value_or(1);
        auto patch = this->patch(item.find("components"));
        if (!patch) return std::unexpected(patch.error());
        const auto damage = patch->damage;
        contracts::ItemStack s{*mapped, 0, 0, std::move(patch->rest)};
        if (count < 1 || count > std::min<std::int64_t>(contracts::max_stack_size, gameplay::items::max_stack(s)))
            return std::unexpected(std::format("item stack of {}", count));
        s.count = static_cast<std::uint8_t>(count);
        if (damage && (*damage < 0 || *damage > gameplay::items::max_damage(s)))
            return std::unexpected("item component minecraft:damage with an invalid value");
        s.damage = static_cast<std::uint16_t>(damage.value_or(0));
        return s;
    }
    // The "components" of an item tag for a stack; an error names a
    // component that has no save form yet.
    std::expected<void, std::string> set_components(Tag& item, const contracts::ItemStack& stack) const {
        Tag c;
        if (auto ok = save(stack.components, stack.damage != 0 ? std::optional<std::int32_t>(stack.damage) : std::nullopt, c); !ok)
            return ok;
        if (!c.keys().empty()) item.set("components", std::move(c));
        return {};
    }
    // The reference state for a stored block state, or nullopt.
    std::optional<BlockState> block(const std::string& notation) {
        if (const auto it = blocks_.find(notation); it != blocks_.end()) return it->second;
        const auto s = mapping_->block(notation);
        return blocks_[notation] = s ? std::optional<BlockState>(*s) : std::nullopt;
    }
    // A stand-in of the same coarse shape for an unsupported block.
    std::optional<BlockState> substitute(const std::string& name) const {
        const auto shape = tables_->shape(name);
        if (!shape) return std::nullopt;
        switch (*shape) {
        case protocol::BlockShape::empty: return gameplay::blocks::air();
        case protocol::BlockShape::unbreakable: return gameplay::blocks::obsidian();
        case protocol::BlockShape::opaque: return gameplay::blocks::stone();
        case protocol::BlockShape::translucent: return gameplay::blocks::glass();
        }
        return std::nullopt;
    }
    // The block type a scheduled tick of block `name` refers to.
    std::optional<BlockType> tick_type(const std::string& name) {
        const auto id = tables_->default_state(name);
        if (!id) return std::nullopt;
        // Blocks without rules of their own have no scheduled behavior to
        // resume, but falling blocks, sugar cane and cacti.
        const auto s = block(*tables_->notation(*id));
        if (s && s->is(BlockType::other)) return gameplay::block_ticks_scheduled(*s) ? std::optional(BlockType::other) : std::nullopt;
        return s ? std::optional<BlockType>(s->type()) : std::nullopt;
    }
    std::optional<std::uint16_t> item(const std::string& name) const {
        const auto i = mapping_->item(name);
        return i ? std::optional<std::uint16_t>(*i) : std::nullopt;
    }
    std::string item_name(std::uint16_t item) const { return mapping_->item_name(item); }
    protocol::BlockStateInfo info(BlockState s, const NeighborLookup& at, BlockPos p) const {
        return *tables_->state(display_state(*palette_, at, p, s));
    }
    protocol::BlockStateInfo info(BlockState s) const { return *tables_->state(palette_->wire(s)); }
    std::string block_name(BlockState s) const { return protocol::parse_notation(mapping_->notation(s))->first; }

private:
    const protocol::ContentTables* tables_;
    const evidence::ContentMapping* mapping_;
    const protocol::ComponentCodec* codec_;
    protocol::ComponentContext ctx_;
    std::optional<BlockPalette> palette_;
    std::unordered_map<std::string, std::optional<BlockState>> blocks_;
};

// ---- import -------------------------------------------------------------------

class Importer {
public:
    Importer(const contracts::VersionBundle& bundle, const ImportOptions& options)
        : bundle_(bundle), options_(options), content_(bundle, options.components) {}

    std::expected<ImportResult, std::string> run(const fs::path& source) {
        if (!fs::is_directory(source)) return std::unexpected(std::format("{} is not a world directory", source.string()));
        if (auto ok = read_level(source / "level.dat"); !ok) return std::unexpected(ok.error());
        if (auto ok = read_regions(source / "region", false); !ok) return std::unexpected(ok.error());
        if (auto ok = read_regions(source / "entities", true); !ok) return std::unexpected(ok.error());
        for (const auto& [dir, name] : {std::pair{"DIM-1", "the_nether"}, std::pair{"DIM1", "the_end"}}) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(source / dir / "region", ec))
                if (e.is_regular_file() && region_coords(e.path().filename().string()) && fs::file_size(e.path(), ec) > 0) {
                    unsupported(std::format("dimension minecraft:{}", name));
                    break;
                }
        }
        if (fs::exists(source / "playerdata")) note("player data is not imported");
        if (const auto file = source / "data" / "random_sequences.dat"; fs::exists(file))
            if (auto ok = read_sequences(file); !ok) return std::unexpected(ok.error());
        if (fs::exists(source / "data")) note("saved data other than random sequences (maps, raids, scoreboards, world border) is not imported");
        note("light and heightmaps are not imported; Circuit has no lighting engine");
        // Containers that lost their data to unsupported content still need a block entity.
        for (const auto& [pos, raw] : result_.state.level(contracts::DimensionId::overworld).blocks) {
            const auto s = BlockState::decode(raw).value();
            if (gameplay::has_block_entity(s.type()) && !block_entities_.contains(pos)) {
                block_entities_.emplace(pos, BlockEntity{s.type(), 0, std::vector<contracts::ItemStack>(gameplay::container_size(s.type())), 0});
                ++created_block_entities_;
            }
        }
        if (created_block_entities_ > 0) note(std::format("{} block entities were missing and were created empty", created_block_entities_));
        if (!report().unsupported.empty() && options_.unsupported == Unsupported::reject) {
            std::string list;
            for (const auto& [kind, count] : report().unsupported) list += std::format("\n  {} ({})", kind, count);
            return std::unexpected("the world contains content Circuit does not support; nothing was imported. "
                                   "Import with the substitution policy to replace or drop it:" + list);
        }
        finish();
        return std::move(result_);
    }

private:
    ImportReport& report() { return result_.report; }
    void note(std::string text) { report().notes.push_back(std::move(text)); }
    void unsupported(const std::string& kind, std::size_t n = 1) { report().unsupported[kind] += n; }

    // The overworld's random sequences of loot tables and the world's settings
    // for new ones (minecraft.wiki "Random sequence"); the settings' flags are
    // absent when they are true.
    std::expected<void, std::string> read_sequences(const fs::path& file) {
        const auto fail = [](std::string what) { return std::unexpected("data/random_sequences.dat: " + what); };
        auto data = read_file(file, max_level_bytes);
        if (!data) return std::unexpected(data.error());
        auto raw = inflate_any(*data, max_level_bytes);
        if (!raw) return fail(raw.error());
        auto root = parse_nbt(*raw, max_level_bytes);
        if (!root) return fail(root.error());
        const auto version = integer(*root, "DataVersion");
        if (version != supported_data_version) {
            unsupported(std::format("random sequences of DataVersion {}", version ? std::to_string(*version) : "none"));
            return {};
        }
        const auto* d = child(*root, "data", TagType::compound);
        if (!d) return fail("no data compound");
        gameplay::SequenceSettings settings;
        settings.salt = static_cast<std::int32_t>(integer(*d, "salt").value_or(0));
        settings.include_world_seed = integer(*d, "include_world_seed").value_or(1) != 0;
        settings.include_sequence_id = integer(*d, "include_sequence_id").value_or(1) != 0;
        static const std::regex id_pattern{"([a-z0-9_.-]+:)?[a-z0-9_./-]+"};
        std::map<std::string, gameplay::RandomSequence> sequences;
        if (const auto* list = child(*d, "sequences", TagType::compound))
            for (std::size_t i = 0; i < list->keys().size(); ++i) {
                const auto& key = list->keys()[i];
                const auto id = key.contains(':') ? key : "minecraft:" + key;
                if (id.size() > 256 || !std::regex_match(key, id_pattern)) return fail(std::format("invalid sequence id \"{}\"", key));
                const auto* source = list->items()[i].type() == TagType::compound
                                         ? child(list->items()[i], "source", TagType::long_array) : nullptr;
                if (!source || source->array().size() != 2) return fail(std::format("sequence {} has no 128-bit source", key));
                sequences.insert_or_assign(id, gameplay::RandomSequence{static_cast<std::uint64_t>(source->array()[0]),
                                                                        static_cast<std::uint64_t>(source->array()[1])});
            }
        result_.state.sequence_settings = settings;
        auto& level = result_.state.level(contracts::DimensionId::overworld).sequences;
        level.assign(sequences.begin(), sequences.end());
        return {};
    }

    std::expected<void, std::string> read_level(const fs::path& file) {
        auto data = read_file(file, max_level_bytes);
        if (!data) return std::unexpected(data.error());
        auto raw = inflate_any(*data, max_level_bytes);
        if (!raw) return std::unexpected("level.dat: " + raw.error());
        auto root = parse_nbt(*raw, max_level_bytes);
        if (!root) return std::unexpected("level.dat: " + root.error());
        const auto* level = child(*root, "Data", TagType::compound);
        if (!level) return std::unexpected("level.dat has no Data compound");
        const auto version = integer(*level, "DataVersion");
        if (version != supported_data_version)
            return std::unexpected(std::format("the world has DataVersion {}; only {} (Java Edition 1.21.8) is supported",
                                               version ? std::to_string(*version) : "none", supported_data_version));
        report().level_name = text(*level, "LevelName").value_or("");
        const auto time = integer(*level, "Time").value_or(0);
        if (time < 0) return std::unexpected("level.dat: negative time");
        time_ = static_cast<std::uint64_t>(time);
        report().time = time_;
        const auto day = integer(*level, "DayTime").value_or(0);
        if (day < 0) return std::unexpected("level.dat: negative day time");
        result_.state.day_time = day;
        // Normal when the file does not say.
        const auto difficulty = integer(*level, "Difficulty").value_or(2);
        if (difficulty < 0 || difficulty > contracts::max_difficulty) return std::unexpected("level.dat: invalid difficulty");
        result_.state.difficulty = static_cast<std::uint8_t>(difficulty);
        // The weather's flags and timers; its levels follow the flags, as when
        // the official servers load a world.
        auto& weather = result_.state.weather;
        const auto timer = [&](std::string_view key, std::int32_t& out) {
            const auto v = integer(*level, key).value_or(0);
            if (v < 0 || v > INT32_MAX) return false;
            out = static_cast<std::int32_t>(v);
            return true;
        };
        if (!timer("clearWeatherTime", weather.clear_time) || !timer("rainTime", weather.rain_time) || !timer("thunderTime", weather.thunder_time))
            return std::unexpected("level.dat: invalid weather time");
        weather.raining = integer(*level, "raining").value_or(0) != 0;
        weather.thundering = integer(*level, "thundering").value_or(0) != 0;
        gameplay::restart_weather(weather);
        // Game rules are strings; the state keeps those away from their defaults.
        if (const auto* rules = child(*level, "GameRules", TagType::compound)) {
            std::size_t ignored = 0;
            for (std::size_t i = 0; i < rules->keys().size(); ++i) {
                const auto& name = rules->keys()[i];
                const auto* rule = bundle_.schema.game_rule(name);
                if (!rule || rule->experimental || rules->items()[i].type() != TagType::string) {
                    ++ignored;
                    continue;
                }
                const auto& value = rules->items()[i].text();
                std::int32_t v{};
                bool valid = true;
                if (rule->type == contracts::GameRuleType::boolean) {
                    valid = value == "true" || value == "false";
                    v = value == "true" ? 1 : 0;
                } else {
                    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
                    valid = ec == std::errc{} && end == value.data() + value.size();
                }
                if (!valid || !gameplay::check_game_rule(bundle_, {name, v}))
                    return std::unexpected(std::format("level.dat: game rule {} has the invalid value {}", name, value));
                if (v != rule->default_value) result_.state.game_rules[name] = v;
            }
            if (ignored > 0) note(std::format("{} game rules the version does not have were ignored", ignored));
        }
        if (const auto* gen = child(*level, "WorldGenSettings", TagType::compound)) report().seed = static_cast<std::uint64_t>(integer(*gen, "seed").value_or(0));
        // SpawnY is where players' feet are; Circuit keeps the block below.
        report().spawn = {static_cast<std::int32_t>(integer(*level, "SpawnX").value_or(0)),
                          static_cast<std::int32_t>(integer(*level, "SpawnY").value_or(1)) - 1,
                          static_cast<std::int32_t>(integer(*level, "SpawnZ").value_or(0))};
        return {};
    }

    std::expected<void, std::string> read_regions(const fs::path& dir, bool entities) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return {};
        std::vector<std::pair<std::pair<std::int32_t, std::int32_t>, fs::path>> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (const auto rc = region_coords(e.path().filename().string()); rc && e.is_regular_file()) files.push_back({*rc, e.path()});
        if (ec) return std::unexpected(std::format("cannot list {}: {}", dir.string(), ec.message()));
        std::ranges::sort(files);
        for (const auto& [rc, path] : files) {
            auto chunks = read_region(path, rc.first, rc.second, options_.max_chunk_bytes);
            if (!chunks) return std::unexpected(chunks.error());
            std::ranges::sort(*chunks, {}, [](const StoredChunk& c) { return std::pair{c.pos.x, c.pos.z}; });
            for (auto& c : *chunks) {
                auto ok = entities ? read_entities(c) : read_chunk(c);
                if (!ok) return ok;
            }
        }
        return {};
    }

    std::expected<void, std::string> read_chunk(const StoredChunk& c) {
        const auto& root = c.root;
        const auto where = std::format("chunk {} {}", c.pos.x, c.pos.z);
        if (integer(root, "xPos") != c.pos.x || integer(root, "zPos") != c.pos.z)
            return std::unexpected(where + ": stored position does not match its place in the region file");
        const auto version = integer(root, "DataVersion");
        if (version != supported_data_version) {
            // Chunks the game has not loaded since an upgrade keep their old version.
            unsupported(std::format("chunk of DataVersion {}", version ? std::to_string(*version) : "none"));
            return {};
        }
        if (text(root, "Status") != "minecraft:full") {
            ++proto_chunks_;
            return {};
        }
        if (++report().chunks > options_.max_chunks) return std::unexpected(std::format("more than {} chunks", options_.max_chunks));
        imported_.insert(c.pos);
        result_.state.level(contracts::DimensionId::overworld).chunks.push_back({c.pos, 0, gameplay::ChunkLevel::unloaded});
        if (const auto* sections = child(root, "sections", TagType::list))
            for (const auto& s : sections->items())
                if (s.type() == TagType::compound)
                    if (auto ok = read_section(c.pos, s, where); !ok) return ok;
        if (const auto* list = child(root, "block_entities", TagType::list))
            for (const auto& be : list->items())
                if (be.type() == TagType::compound)
                    if (auto ok = read_block_entity(c.pos, be, where); !ok) return ok;
        for (const auto& [key, fluid] : {std::pair{"block_ticks", false}, std::pair{"fluid_ticks", true}})
            if (const auto* list = child(root, key, TagType::list))
                for (const auto& t : list->items())
                    if (t.type() == TagType::compound)
                        if (auto ok = read_tick(c.pos, t, fluid, where); !ok) return ok;
        if (const auto* post = child(root, "PostProcessing", TagType::list))
            for (const auto& l : post->items())
                if (!l.items().empty()) {
                    ++post_processing_;
                    break;
                }
        return {};
    }

    std::expected<void, std::string> read_section(ChunkPos chunk, const Tag& s, const std::string& where) {
        const auto* states = child(s, "block_states", TagType::compound);
        if (!states) return {}; // light-only sections above and below the world
        const auto y = integer(s, "Y");
        const auto min_section = bundle_.world.min_y >> 4;
        if (!y || *y < min_section || *y >= min_section + bundle_.world.sections())
            return std::unexpected(std::format("{}: section outside the world height", where));
        const auto* palette = child(*states, "palette", TagType::list);
        if (!palette || palette->items().empty() || palette->items().size() > 4096)
            return std::unexpected(std::format("{}: section {} has no valid palette", where, *y));
        std::vector<std::optional<BlockState>> mapped;
        for (const auto& entry : palette->items()) {
            if (entry.type() != TagType::compound) return std::unexpected(std::format("{}: malformed palette entry", where));
            const auto name = text(entry, "Name").value_or("");
            const auto notation = notation_of(entry);
            auto s = content_.block(notation);
            if (!s) {
                substituted_.insert(notation);
                s = options_.unsupported == Unsupported::substitute ? content_.substitute(name) : std::nullopt;
                if (!s) s = gameplay::blocks::air();
            }
            mapped.push_back(s);
        }
        std::array<std::uint32_t, 4096> index{};
        if (palette->items().size() > 1) {
            const auto* data = child(*states, "data", TagType::long_array);
            const auto bits = std::max(4u, static_cast<unsigned>(std::bit_width(palette->items().size() - 1)));
            const auto per_long = 64 / bits;
            if (!data || data->array().size() != (4096 + per_long - 1) / per_long)
                return std::unexpected(std::format("{}: section {} has a block array of the wrong length", where, *y));
            for (std::size_t i = 0; i < 4096; ++i) {
                const auto word = static_cast<std::uint64_t>(data->array()[i / per_long]);
                index[i] = static_cast<std::uint32_t>((word >> ((i % per_long) * bits)) & ((1ULL << bits) - 1));
                if (index[i] >= palette->items().size())
                    return std::unexpected(std::format("{}: section {} indexes outside its palette", where, *y));
            }
        }
        std::vector<std::size_t> counts(palette->items().size());
        for (std::size_t i = 0; i < 4096; ++i) ++counts[index[i]];
        for (std::size_t p = 0; p < palette->items().size(); ++p) {
            const auto notation = notation_of(palette->items()[p]);
            if (counts[p] > 0 && substituted_.contains(notation)) unsupported("block " + text(palette->items()[p], "Name").value_or("?"), counts[p]);
        }
        for (std::size_t i = 0; i < 4096; ++i) {
            const auto s = *mapped[index[i]];
            if (s == gameplay::blocks::air()) continue;
            result_.state.level(contracts::DimensionId::overworld).blocks.push_back({BlockPos{chunk.x * 16 + static_cast<std::int32_t>(i & 15),
                                                     static_cast<std::int32_t>(*y) * 16 + static_cast<std::int32_t>(i >> 8),
                                                     chunk.z * 16 + static_cast<std::int32_t>((i >> 4) & 15)},
                                            s.raw()});
            ++report().blocks;
        }
        return {};
    }

    std::optional<BlockState> block_at(BlockPos p) const {
        // Blocks of the current chunk were appended last; search from the end.
        for (auto it = result_.state.level(contracts::DimensionId::overworld).blocks.rbegin(); it != result_.state.level(contracts::DimensionId::overworld).blocks.rend(); ++it) {
            if (contracts::chunk_of(it->first) != contracts::chunk_of(p)) break;
            if (it->first == p) return BlockState::decode(it->second).value();
        }
        return std::nullopt;
    }

    // Reads the items of a container; returns false when one is unsupported.
    bool read_items(const Tag& be, BlockEntity& out, const std::string& kind) {
        const auto* items = child(be, "Items", TagType::list);
        if (!items) return true;
        bool all = true;
        for (const auto& item : items->items()) {
            if (item.type() != TagType::compound) continue;
            const auto slot = integer(item, "Slot");
            auto stack = content_.stack(item);
            if (!slot || *slot < 0 || static_cast<std::size_t>(*slot) >= out.slots.size()) {
                unsupported(kind + " slot outside the container");
                all = false;
            } else if (!stack) {
                unsupported(stack.error());
                all = false;
            } else {
                out.slots[static_cast<std::size_t>(*slot)] = std::move(*stack);
            }
        }
        return all;
    }

    std::expected<void, std::string> read_block_entity(ChunkPos chunk, const Tag& be, const std::string& where) {
        const auto id = text(be, "id").value_or("");
        const auto x = integer(be, "x"), y = integer(be, "y"), z = integer(be, "z");
        if (!x || !y || !z) return std::unexpected(std::format("{}: block entity {} without a position", where, id));
        const BlockPos pos{static_cast<std::int32_t>(*x), static_cast<std::int32_t>(*y), static_cast<std::int32_t>(*z)};
        if (contracts::chunk_of(pos) != chunk) return std::unexpected(std::format("{}: block entity {} outside its chunk", where, id));
        const auto block = block_at(pos);
        const auto expect = [&](BlockType t) { return block && block->is(t); };
        BlockEntity out;
        if (block && block->is(BlockType::other)) {
            // A block without rules of its own keeps its block entity as saved.
            auto kept = Tag::compound();
            for (std::size_t i = 0; i < be.keys().size(); ++i)
                if (be.keys()[i] != "x" && be.keys()[i] != "y" && be.keys()[i] != "z") kept.set(be.keys()[i], be.items()[i]);
            protocol::Writer w;
            protocol::nbt::write_network(w, protocol::nbt::canonical(kept));
            out.kind = BlockType::other;
            out.saved = contracts::Blob(w.take());
            if (!block_entities_.emplace(pos, std::move(out)).second) return std::unexpected(std::format("{}: duplicate block entity", where));
            ++report().block_entities;
            return {};
        }
        // Loot tables and locks change what players can do: unsupported until
        // modeled. The game writes an empty component map on every block entity.
        for (const auto* field : {"LootTable", "lock"})
            if (const auto* f = be.find(field); f && !(f->type() == TagType::compound && f->keys().empty()))
                unsupported(std::format("block entity {} with {}", id, field));
        if (integer(be, "keepPacked").value_or(0) != 0) unsupported(std::format("block entity {} with keepPacked", id));
        // A custom name and other components are kept as components.
        std::vector<contracts::ComponentPatch::Added> named;
        if (const auto* name = be.find("CustomName")) {
            auto value = content_.custom_name(*name);
            if (value) named.push_back(std::move(*value));
            else unsupported(std::format("block entity {} with {}", id, value.error()));
        }
        if (auto patch = content_.patch(be.find("components"), std::move(named)); !patch || patch->damage)
            unsupported(std::format("block entity {} with {}", id, patch ? "a damage component" : patch.error()));
        else
            out.components = std::move(patch->rest);
        const auto components = out.components;
        if (id == "minecraft:chest" && expect(BlockType::chest)) {
            out = {BlockType::chest, 0, std::vector<contracts::ItemStack>(27), 0};
            read_items(be, out, "chest");
        } else if (id == "minecraft:hopper" && expect(BlockType::hopper)) {
            out = {BlockType::hopper, 0, std::vector<contracts::ItemStack>(5), 0};
            read_items(be, out, "hopper");
            out.cooldown = static_cast<std::uint8_t>(std::clamp<std::int64_t>(integer(be, "TransferCooldown").value_or(0), 0, 8));
        } else if ((id == "minecraft:dispenser" || id == "minecraft:dropper") && expect(BlockType::dispenser)
                   && (block->variant() == 1) == (id == "minecraft:dropper")) {
            out = {BlockType::dispenser, 0, std::vector<contracts::ItemStack>(9), 0};
            read_items(be, out, id == "minecraft:dropper" ? "dropper" : "dispenser");
        } else if ((id == "minecraft:furnace" || id == "minecraft:smoker" || id == "minecraft:blast_furnace") && expect(BlockType::furnace)
                   && block->variant() == (id == "minecraft:smoker" ? 1 : id == "minecraft:blast_furnace" ? 2 : 0)) {
            out = {BlockType::furnace, 0, std::vector<contracts::ItemStack>(3), 0};
            read_items(be, out, std::string(id.substr(10)));
            // Its times; the recipes it used (the experience they give) are not kept.
            const auto time = [&](std::string_view key) {
                return static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(be, key).value_or(0), 0, 32767));
            };
            out.cook_time = time("cooking_time_spent");
            out.cook_total = time("cooking_total_time");
            out.lit_time = time("lit_time_remaining");
            out.lit_total = time("lit_total_time");
        } else if (id == "minecraft:crafter" && expect(BlockType::crafter)) {
            out = {BlockType::crafter, 0, std::vector<contracts::ItemStack>(9), 0};
            read_items(be, out, "crafter");
            // Its disabled slots and crafting ticks; `triggered` is the block state's.
            if (const auto* disabled = be.find("disabled_slots"); disabled && disabled->type() == TagType::int_array)
                for (const auto slot : disabled->array())
                    if (slot >= 0 && slot < 9) out.disabled = static_cast<std::uint16_t>(out.disabled | 1u << slot);
            out.crafting_ticks = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(be, "crafting_ticks_remaining").value_or(0), 0, 6));
        } else if (id == "minecraft:comparator" && expect(BlockType::comparator)) {
            out = {BlockType::comparator, 0, {}, 0};
            out.signal = static_cast<std::uint8_t>(std::clamp<std::int64_t>(integer(be, "OutputSignal").value_or(0), 0, 15));
        } else if (id == "minecraft:daylight_detector" && expect(BlockType::daylight_detector)) {
            out = {BlockType::daylight_detector, 0, {}, 0};
        } else if (id == "minecraft:piston" && expect(BlockType::moving_piston)) {
            out = {BlockType::moving_piston, 0, {}, 0};
            const auto* moved = child(be, "blockState", TagType::compound);
            const auto moving = moved ? content_.block(notation_of(*moved)) : std::nullopt;
            const auto facing = integer(be, "facing");
            const auto progress = real(be, "progress").value_or(0.0);
            if (!moving || !facing || *facing < 0 || *facing > 5) {
                unsupported(moved ? "moving block " + text(*moved, "Name").value_or("?") : "moving piston without a block");
                return {};
            }
            const auto ticks = std::clamp(std::lround(progress / 0.5), 0L, 2L);
            if (std::abs(progress - static_cast<double>(ticks) * 0.5) > 1e-6) ++approximated_;
            out.moving = moving->raw();
            out.progress = static_cast<std::uint8_t>(ticks);
            out.motion = gameplay::Motion{static_cast<contracts::Direction>(*facing), integer(be, "extending").value_or(0) != 0,
                                          integer(be, "source").value_or(0) != 0}
                             .pack();
        } else {
            // Either an unsupported kind, or the block it belongs to was replaced.
            if (block && !gameplay::has_block_entity(block->type())) unsupported("block entity " + id);
            else if (!block) unsupported("block entity " + id);
            return {};
        }
        out.components = components;
        if (!block_entities_.emplace(pos, std::move(out)).second) return std::unexpected(std::format("{}: duplicate block entity", where));
        ++report().block_entities;
        return {};
    }

    std::expected<void, std::string> read_tick(ChunkPos chunk, const Tag& t, bool fluid, const std::string& where) {
        const auto id = text(t, "i").value_or("");
        const auto x = integer(t, "x"), y = integer(t, "y"), z = integer(t, "z"), delay = integer(t, "t");
        if (!x || !y || !z || !delay) return std::unexpected(std::format("{}: malformed scheduled tick", where));
        const BlockPos pos{static_cast<std::int32_t>(*x), static_cast<std::int32_t>(*y), static_cast<std::int32_t>(*z)};
        if (contracts::chunk_of(pos) != chunk) return std::unexpected(std::format("{}: scheduled tick outside its chunk", where));
        std::optional<BlockType> type;
        // Lava, a registry block, ticks as one; the water of a waterlogged
        // block ticks as the block.
        if (fluid) {
            type = id == "minecraft:water" || id == "minecraft:flowing_water" ? std::optional(BlockType::water)
                 : id == "minecraft:lava" || id == "minecraft:flowing_lava"   ? std::optional(BlockType::other)
                                                                              : std::nullopt;
            if (const auto here = block_at(pos); type == BlockType::water && here && gameplay::waterlogged_block(*here)) type = here->type();
        }
        else type = content_.tick_type(id);
        if (!type) {
            unsupported(std::format("scheduled tick of {}", id));
            return {};
        }
        const auto due = static_cast<std::int64_t>(time_) + *delay;
        WorldDump::PendingTick p{fluid ? contracts::TickList::fluid : contracts::TickList::block,
                                 {pos, *type, static_cast<contracts::Tick>(std::max<std::int64_t>(due, 0)),
                                  static_cast<std::int8_t>(std::clamp<std::int64_t>(integer(t, "p").value_or(0), -128, 127)),
                                  next_suborder_++}};
        if (!tick_keys_.insert({static_cast<int>(p.list), pos, *type}).second) return {}; // same block and position: one tick
        result_.state.level(contracts::DimensionId::overworld).ticks.push_back(p);
        ++report().scheduled_ticks;
        return {};
    }

    std::expected<void, std::string> read_entities(const StoredChunk& c) {
        const auto version = integer(c.root, "DataVersion");
        const auto* list = child(c.root, "Entities", TagType::list);
        if (!list || list->items().empty()) return {};
        if (version != supported_data_version) {
            unsupported(std::format("entity chunk of DataVersion {}", version ? std::to_string(*version) : "none"));
            return {};
        }
        for (const auto& e : list->items()) {
            if (e.type() != TagType::compound) continue;
            const auto id = text(e, "id").value_or("");
            const bool falling = id == "minecraft:falling_block";
            const bool tnt = id == "minecraft:tnt";
            const bool arrow = id == "minecraft:arrow", snowball = id == "minecraft:snowball", minecart = id == "minecraft:minecart";
            const auto boat = std::ranges::find(gameplay::boat_types, id);
            const bool is_boat = boat != gameplay::boat_types.end();
            if (id != "minecraft:item" && !falling && !tnt && !arrow && !snowball && !minecart && !is_boat) {
                unsupported("entity " + id);
                continue;
            }
            const auto pos = numbers(e, "Pos", 3);
            const auto motion = numbers(e, "Motion", 3).value_or(std::vector<double>{0, 0, 0});
            const auto* item = child(e, "Item", TagType::compound);
            const auto* state = child(e, "BlockState", TagType::compound);
            if (!pos || (falling && !state) || (id == "minecraft:item" && !item) || !std::isfinite((*pos)[0]) || !std::isfinite((*pos)[1])
                || !std::isfinite((*pos)[2])) {
                unsupported(falling ? "malformed falling block" : tnt ? "malformed primed TNT" : "malformed item entity");
                continue;
            }
            gameplay::Entity out;
            if (minecart || is_boat) {
                // Their state before this tick is not saved: a loaded boat
                // has no state before, a minecart no water.
                out.kind = minecart ? gameplay::EntityKind::minecart : gameplay::EntityKind::boat;
                if (is_boat) out.variant = static_cast<std::uint8_t>(boat - gameplay::boat_types.begin());
                // A minecart's yaw decides what pushes it; whether it is turned
                // against its motion is not saved.
                if (minecart)
                    if (const auto rotation = numbers(e, "Rotation", 2); rotation && std::isfinite((*rotation)[0]))
                        out.yaw = std::fmod(static_cast<float>((*rotation)[0]), 360.0f);
            } else if (arrow || snowball) {
                // An arrow's owner, damage, pickup rule and item, and a
                // snowball's item, are not modeled.
                out.kind = arrow ? gameplay::EntityKind::arrow : gameplay::EntityKind::snowball;
                if (arrow) {
                    out.in_ground = integer(e, "inGround").value_or(0) != 0;
                    out.life = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(e, "life").value_or(0), 0, 32767));
                    out.fire = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(e, "Fire").value_or(0), -32768, 32767));
                    if (out.fire < 0) out.fire = 0;
                }
            } else if (tnt) {
                // Primed TNT that looks like another block, or explodes with
                // another power, is not modeled; its owner only blames
                // damage on someone.
                const auto* shown = child(e, "block_state", TagType::compound);
                if (shown && text(*shown, "Name") != "minecraft:tnt") {
                    unsupported("primed TNT shown as " + text(*shown, "Name").value_or("?"));
                    continue;
                }
                if (const auto power = real(e, "explosion_power"); power && *power != 4.0) {
                    unsupported(std::format("primed TNT of explosion power {}", *power));
                    continue;
                }
                out.kind = gameplay::EntityKind::primed_tnt;
                // A short in the format, 80 when absent.
                out.fuse = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(e, "fuse").value_or(80), -32768, 32767));
            } else if (falling) {
                const auto carried = content_.block(notation_of(*state));
                if (!carried || carried->is_air()) {
                    unsupported("falling block " + text(*state, "Name").value_or("?"));
                    continue;
                }
                out.kind = gameplay::EntityKind::falling_block;
                out.carried = *carried;
                out.fall_time = static_cast<std::uint32_t>(std::clamp<std::int64_t>(integer(e, "Time").value_or(0), 0, INT32_MAX));
            } else {
                auto stack = content_.stack(*item);
                if (!stack) {
                    unsupported(stack.error());
                    continue;
                }
                out.stack = std::move(*stack);
                // A short in the format; -32768 never ages. The tick count is not
                // saved: it starts again at 0, as when the target versions load it.
                out.age = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(e, "Age").value_or(0), gameplay::item_age_never, 32767));
                out.pickup_delay = static_cast<std::uint16_t>(std::clamp<std::int64_t>(integer(e, "PickupDelay").value_or(0), 0, 32767));
                // A short; an item with none left is gone.
                const auto health = integer(e, "Health").value_or(gameplay::item_health);
                // A burning item's fire ticks (none: -1 or less).
                out.fire = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(e, "Fire").value_or(0), 0, 32767));
                if (health <= 0) continue;
                out.health = static_cast<std::int32_t>(std::min<std::int64_t>(health, 32767));
            }
            if (e.find("Passengers")) unsupported("entity passengers");
            // Checked before conversion: inside the border, and no further below or
            // above the world than the rules let entities fall or fly.
            if (std::abs((*pos)[0]) >= contracts::world_border || std::abs((*pos)[2]) >= contracts::world_border
                || (*pos)[1] < bundle_.world.min_y - 64.0 || (*pos)[1] >= bundle_.world.min_y + bundle_.world.height + 4096.0) {
                unsupported("entity outside the world");
                continue;
            }
            out.position = {(*pos)[0], (*pos)[1], (*pos)[2]};
            // Within the rules' speed limit (motion the target versions keep is far below it).
            const auto speed = [&](double v) { return std::isfinite(v) ? std::clamp(v, -gameplay::max_entity_speed, gameplay::max_entity_speed) : 0.0; };
            out.velocity = {speed(motion[0]), speed(motion[1]), speed(motion[2])};
            const auto block = contracts::block_of(out.position);
            if (!imported_.contains(contracts::chunk_of(block))) {
                unsupported("entity outside the imported chunks");
                continue;
            }
            out.on_ground = integer(e, "OnGround").value_or(0) != 0;
            // How far it has fallen (a double since 1.21.5), within what the rules keep.
            const auto fell = real(e, "fall_distance").value_or(0.0);
            out.fall_distance = std::isfinite(fell) ? std::clamp(fell, 0.0, 65536.0) : 0.0;
            out.last_tick = time_;
            entities_.push_back(out);
        }
        return {};
    }

    void finish() {
        // Everything imported is the overworld's.
        auto& s = result_.state;
        auto& l = s.level(contracts::DimensionId::overworld);
        s.time = time_;
        l.next_suborder = next_suborder_;
        std::uint64_t order = 0;
        for (auto& [pos, be] : block_entities_) {
            be.order = order++;
            l.block_entities.push_back({pos, std::move(be)});
        }
        l.next_block_entity_order = order;
        std::uint64_t id = 1;
        for (auto& e : entities_) {
            e.id = contracts::EntityId{id++};
            l.entities.push_back(e);
        }
        report().entities = entities_.size();
        s.next_entity_id = id;
        gameplay::canonicalize(s);
        if (proto_chunks_ > 0) note(std::format("{} chunks with incomplete generation were skipped", proto_chunks_));
        if (post_processing_ > 0) note(std::format("{} chunks had pending post-processing updates, which are dropped", post_processing_));
        if (approximated_ > 0)
            note(std::format("{} moving-piston progress values were rounded to Circuit's model", approximated_));
        note("block entity creation order and the order of entity ids follow chunk position, not the source world's history");

    }

    const contracts::VersionBundle& bundle_;
    ImportOptions options_;
    Content content_;
    ImportResult result_;
    std::uint64_t time_{};
    std::set<ChunkPos> imported_;
    std::set<std::string> substituted_;
    std::map<BlockPos, BlockEntity> block_entities_;
    std::vector<gameplay::Entity> entities_;
    std::set<std::tuple<int, BlockPos, BlockType>> tick_keys_;
    std::uint64_t next_suborder_{};
    std::size_t proto_chunks_{}, post_processing_{}, approximated_{}, created_block_entities_{};
};

// ---- export -------------------------------------------------------------------

class Exporter {
public:
    // The overworld's state; other levels are refused in run().
    Exporter(const WorldDump& state, const contracts::VersionBundle& bundle, const ExportOptions& options)
        : world_(state), state_(state.overworld()), bundle_(bundle), options_(options), content_(bundle, options.components) {
        for (const auto& [p, raw] : state_.blocks) blocks_.emplace(p, BlockState::decode(raw).value());
    }

    std::expected<ExportReport, std::string> run(const fs::path& target) {
        for (const auto& l : world_.levels)
            if (l.dimension != contracts::DimensionId::overworld)
                return std::unexpected(std::format("the world has state in {}; Anvil export writes the overworld only yet",
                                                   contracts::dimension_name(l.dimension)));
        if (!state_.block_events.empty())
            return std::unexpected("the state has pending block events, which the Anvil format cannot hold; export between ticks");
        std::error_code ec;
        if (fs::exists(target, ec)) return std::unexpected(std::format("{} already exists", target.string()));
        const auto partial = fs::path(target.string() + ".partial");
        fs::remove_all(partial, ec);
        if (!fs::create_directories(partial / "region", ec) || !fs::create_directories(partial / "entities", ec) ||
            !fs::create_directories(partial / "data", ec))
            return std::unexpected(std::format("cannot create {}", partial.string()));
        auto ok = write_all(partial);
        if (ok) {
            fs::rename(partial, target, ec);
            if (ec) ok = std::unexpected(std::format("cannot rename {} to {}: {}", partial.string(), target.string(), ec.message()));
        }
        if (!ok) {
            fs::remove_all(partial, ec);
            return std::unexpected(ok.error());
        }
        return report_;
    }

private:
    BlockState at(BlockPos p) const {
        const auto it = blocks_.find(p);
        return it == blocks_.end() ? gameplay::blocks::air() : it->second;
    }

    std::expected<void, std::string> write_all(const fs::path& dir) {
        std::map<std::pair<std::int32_t, std::int32_t>, std::vector<std::pair<ChunkPos, Tag>>> regions, entity_regions;
        std::map<ChunkPos, std::vector<const std::pair<BlockPos, BlockEntity>*>> block_entities;
        for (const auto& be : state_.block_entities) block_entities[contracts::chunk_of(be.first)].push_back(&be);
        std::map<ChunkPos, std::vector<const WorldDump::PendingTick*>> ticks;
        for (const auto& t : state_.ticks) ticks[contracts::chunk_of(t.tick.pos)].push_back(&t);
        std::map<ChunkPos, std::vector<const gameplay::Entity*>> entities;
        for (const auto& e : state_.entities) entities[contracts::chunk_of(contracts::block_of(e.position))].push_back(&e);
        std::map<ChunkPos, std::vector<const std::pair<BlockPos, std::uint32_t>*>> blocks;
        for (const auto& b : state_.blocks) blocks[contracts::chunk_of(b.first)].push_back(&b);
        for (const auto& c : state_.chunks) {
            const auto region = std::pair{c.pos.x >> 5, c.pos.z >> 5};
            regions[region].emplace_back(c.pos, chunk_tag(c.pos, blocks[c.pos], block_entities[c.pos], ticks[c.pos]));
            if (const auto it = entities.find(c.pos); it != entities.end())
                entity_regions[region].emplace_back(c.pos, entity_chunk_tag(c.pos, it->second));
            ++report_.chunks;
        }
        // Content the Anvil format of Circuit cannot write yet fails the export.
        if (!refused_.empty()) {
            std::string list;
            for (const auto& [kind, count] : refused_) list += std::format("\n  {} ({})", kind, count);
            return std::unexpected("the world holds content Circuit cannot write to Anvil yet; nothing was exported:" + list);
        }
        for (const auto& [rc, chunks] : regions)
            if (auto ok = write_region(dir / "region" / std::format("r.{}.{}.mca", rc.first, rc.second), chunks); !ok) return ok;
        for (const auto& [rc, chunks] : entity_regions)
            if (auto ok = write_region(dir / "entities" / std::format("r.{}.{}.mca", rc.first, rc.second), chunks); !ok) return ok;
        report_.regions = regions.size();
        report_.notes.push_back("light and heightmaps are left for the game to compute");
        report_.notes.push_back("biomes are written as minecraft:plains");
        if (auto ok = write_file(dir / "data" / "random_sequences.dat", gzip(named_nbt(sequences_tag()))); !ok) return ok;
        return write_file(dir / "level.dat", gzip(named_nbt(level_tag())));
    }

    // As the official server saves them: the flags only when they are false.
    Tag sequences_tag() const {
        auto data = Tag::compound();
        data.set("salt", Tag::int_(world_.sequence_settings.salt));
        if (!world_.sequence_settings.include_world_seed) data.set("include_world_seed", Tag::boolean(false));
        if (!world_.sequence_settings.include_sequence_id) data.set("include_sequence_id", Tag::boolean(false));
        auto sequences = Tag::compound();
        for (const auto& [id, sequence] : state_.sequences) {
            auto entry = Tag::compound();
            entry.set("source", Tag::long_array({static_cast<std::int64_t>(sequence.low()), static_cast<std::int64_t>(sequence.high())}));
            sequences.set(id, std::move(entry));
        }
        data.set("sequences", std::move(sequences));
        auto root = Tag::compound();
        root.set("data", std::move(data));
        root.set("DataVersion", Tag::int_(supported_data_version));
        return root;
    }

    Tag chunk_tag(ChunkPos pos, const std::vector<const std::pair<BlockPos, std::uint32_t>*>& blocks,
                  const std::vector<const std::pair<BlockPos, BlockEntity>*>& block_entities,
                  const std::vector<const WorldDump::PendingTick*>& ticks) {
        auto root = Tag::compound();
        const auto min_section = bundle_.world.min_y >> 4;
        root.set("DataVersion", Tag::int_(supported_data_version));
        root.set("xPos", Tag::int_(pos.x));
        root.set("zPos", Tag::int_(pos.z));
        root.set("yPos", Tag::int_(min_section));
        root.set("Status", Tag::string("minecraft:full"));
        root.set("LastUpdate", Tag::long_(static_cast<std::int64_t>(world_.time)));
        root.set("InhabitedTime", Tag::long_(0));
        root.set("isLightOn", Tag::byte(0));
        const NeighborLookup look = [this](BlockPos p) { return at(p); };
        // Per section, 1 + the index of each block's stored state (with display
        // properties); 0 is air.
        std::map<std::int32_t, std::array<std::int32_t, 4096>> sections;
        for (const auto* b : blocks) {
            auto& sec = sections.try_emplace(b->first.y >> 4).first->second;
            const auto s = BlockState::decode(b->second).value();
            sec[contracts::section_local_index(b->first)] = display_state_id(s, look, b->first) + 1;
        }
        auto list = Tag::list(TagType::compound);
        for (std::int32_t y = min_section; y < min_section + bundle_.world.sections(); ++y) {
            auto section = Tag::compound();
            section.set("Y", Tag::byte(static_cast<std::int8_t>(y)));
            std::array<std::int32_t, 4096> ids{};
            if (const auto it = sections.find(y); it != sections.end()) ids = it->second;
            section.set("block_states", block_states(ids));
            auto biomes = Tag::compound();
            auto biome_palette = Tag::list(TagType::string);
            biome_palette.push(Tag::string("minecraft:plains"));
            biomes.set("palette", std::move(biome_palette));
            section.set("biomes", std::move(biomes));
            list.push(std::move(section));
        }
        root.set("sections", std::move(list));
        auto bes = Tag::list(TagType::compound);
        for (const auto* be : block_entities) {
            bes.push(block_entity_tag(be->first, be->second));
            ++report_.block_entities;
        }
        root.set("block_entities", std::move(bes));
        auto block_ticks = Tag::list(TagType::compound), fluid_ticks = Tag::list(TagType::compound);
        for (const auto* t : ticks) {
            auto tag = Tag::compound();
            const bool fluid = t->list == contracts::TickList::fluid;
            // A block tick names the block at its position (the kind of
            // button, the falling block); one whose block is gone names its
            // type, unless that is a block without rules of its own, which
            // a stale tick leaves alone.
            const auto here = at(t->tick.pos);
            if (!fluid && here.type() != t->tick.block && t->tick.block == BlockType::other) continue;
            const bool lava = t->tick.block == BlockType::other && gameplay::is_lava(here);
            const bool source = lava ? gameplay::is_lava_source(here)
                                     : !here.is(BlockType::water) || (here.level() == 0 && !here.flag()); // a waterlogged block holds a source
            tag.set("i", Tag::string(fluid ? (lava ? (source ? "minecraft:lava" : "minecraft:flowing_lava") : (source ? "minecraft:water" : "minecraft:flowing_water"))
                                           : content_.block_name(here.type() == t->tick.block ? here : BlockState::of(t->tick.block))));
            tag.set("p", Tag::int_(t->tick.priority));
            tag.set("t", Tag::int_(static_cast<std::int32_t>(static_cast<std::int64_t>(t->tick.due) - static_cast<std::int64_t>(world_.time))));
            tag.set("x", Tag::int_(t->tick.pos.x));
            tag.set("y", Tag::int_(t->tick.pos.y));
            tag.set("z", Tag::int_(t->tick.pos.z));
            (fluid ? fluid_ticks : block_ticks).push(std::move(tag));
            ++report_.scheduled_ticks;
        }
        root.set("block_ticks", std::move(block_ticks));
        root.set("fluid_ticks", std::move(fluid_ticks));
        auto post = Tag::list(TagType::list);
        for (std::int32_t i = 0; i < bundle_.world.sections(); ++i) post.push(Tag::list(TagType::end));
        root.set("PostProcessing", std::move(post));
        auto structures = Tag::compound();
        structures.set("References", Tag::compound());
        structures.set("starts", Tag::compound());
        root.set("structures", std::move(structures));
        return root;
    }

    std::int32_t display_state_id(BlockState s, const NeighborLookup& look, BlockPos p) {
        const auto info = content_.info(s, look, p);
        const auto key = notation_key(info);
        if (const auto it = ids_.find(key); it != ids_.end()) return it->second;
        const auto id = static_cast<std::int32_t>(infos_.size());
        infos_.push_back(info);
        ids_.emplace(key, id);
        return id;
    }
    static std::string notation_key(const protocol::BlockStateInfo& info) {
        std::string k(info.block);
        for (const auto& [a, b] : info.properties) k += std::format(",{}={}", a, b);
        return k;
    }

    // A paletted block-state container (see `chunk_tag` for the cell values).
    Tag block_states(const std::array<std::int32_t, 4096>& cells) {
        const auto air = display_state_id(gameplay::blocks::air(), [](BlockPos) { return gameplay::blocks::air(); }, {});
        std::vector<std::int32_t> palette;
        std::unordered_map<std::int32_t, std::uint32_t> index;
        std::array<std::uint32_t, 4096> values{};
        for (std::size_t i = 0; i < 4096; ++i) {
            const auto id = cells[i] == 0 ? air : cells[i] - 1;
            const auto [it, fresh] = index.try_emplace(id, static_cast<std::uint32_t>(palette.size()));
            if (fresh) palette.push_back(id);
            values[i] = it->second;
        }
        auto t = Tag::compound();
        auto list = Tag::list(TagType::compound);
        for (const auto id : palette) list.push(state_tag(infos_[static_cast<std::size_t>(id)]));
        t.set("palette", std::move(list));
        if (palette.size() > 1) {
            const auto bits = std::max(4u, static_cast<unsigned>(std::bit_width(palette.size() - 1)));
            const auto per_long = 64 / bits;
            std::vector<std::int64_t> data((4096 + per_long - 1) / per_long);
            for (std::size_t i = 0; i < 4096; ++i)
                data[i / per_long] = static_cast<std::int64_t>(static_cast<std::uint64_t>(data[i / per_long]) |
                                                               (static_cast<std::uint64_t>(values[i]) << ((i % per_long) * bits)));
            t.set("data", Tag::long_array(std::move(data)));
        }
        return t;
    }
    Tag block_entity_tag(BlockPos pos, const BlockEntity& be) {
        auto t = Tag::compound();
        if (be.kind == BlockType::other) {
            // Saved data of a block without rules of its own, as imported, or
            // for one a player placed, what its type saves when just placed.
            protocol::Reader r(be.saved.bytes());
            if (auto saved = protocol::nbt::read_network(r); saved && saved->type() == TagType::compound) {
                t = std::move(*saved);
            } else if (const auto state = at(pos); state.is(BlockType::other)) {
                if (!gameplay::registry::block_entity_type(state.registry_state()).empty())
                    t = default_block_entity(state.registry_state(), bundle_.schema.content_release);
            }
            t.set("x", Tag::int_(pos.x));
            t.set("y", Tag::int_(pos.y));
            t.set("z", Tag::int_(pos.z));
            return t;
        }
        const auto items = [&](const std::vector<contracts::ItemStack>& slots) {
            auto list = Tag::list(TagType::compound);
            for (std::size_t i = 0; i < slots.size(); ++i) {
                if (slots[i].empty()) continue;
                auto item = Tag::compound();
                item.set("Slot", Tag::byte(static_cast<std::int8_t>(i)));
                item.set("id", Tag::string(content_.item_name(slots[i].item)));
                item.set("count", Tag::int_(slots[i].count));
                components(item, slots[i]);
                list.push(std::move(item));
            }
            return list;
        };
        switch (be.kind) {
        case BlockType::chest:
            t.set("id", Tag::string("minecraft:chest"));
            t.set("Items", items(be.slots));
            break;
        case BlockType::hopper:
            t.set("id", Tag::string("minecraft:hopper"));
            t.set("Items", items(be.slots));
            t.set("TransferCooldown", Tag::int_(be.cooldown));
            break;
        case BlockType::dispenser:
            t.set("id", Tag::string(at(pos).variant() == 1 ? "minecraft:dropper" : "minecraft:dispenser"));
            t.set("Items", items(be.slots));
            break;
        case BlockType::furnace: {
            constexpr std::array<std::string_view, 3> kinds{"minecraft:furnace", "minecraft:smoker", "minecraft:blast_furnace"};
            t.set("id", Tag::string(std::string(kinds[at(pos).variant() % 3])));
            t.set("Items", items(be.slots));
            t.set("RecipesUsed", Tag::compound());
            t.set("cooking_time_spent", Tag::short_(static_cast<std::int16_t>(be.cook_time)));
            t.set("cooking_total_time", Tag::short_(static_cast<std::int16_t>(be.cook_total)));
            t.set("lit_time_remaining", Tag::short_(static_cast<std::int16_t>(be.lit_time)));
            t.set("lit_total_time", Tag::short_(static_cast<std::int16_t>(be.lit_total)));
            break;
        }
        case BlockType::crafter: {
            t.set("id", Tag::string("minecraft:crafter"));
            t.set("Items", items(be.slots));
            std::vector<std::int32_t> disabled;
            for (std::int32_t k = 0; k < 9; ++k)
                if (be.disabled >> k & 1) disabled.push_back(k);
            t.set("crafting_ticks_remaining", Tag::int_(be.crafting_ticks));
            t.set("disabled_slots", Tag::int_array(std::move(disabled)));
            t.set("triggered", Tag::int_(at(pos).flag() ? 1 : 0));
            break;
        }
        case BlockType::comparator:
            t.set("id", Tag::string("minecraft:comparator"));
            t.set("OutputSignal", Tag::int_(be.signal));
            break;
        case BlockType::daylight_detector: t.set("id", Tag::string("minecraft:daylight_detector")); break;
        case BlockType::moving_piston: {
            const auto motion = gameplay::Motion::unpack(be.motion);
            t.set("id", Tag::string("minecraft:piston"));
            t.set("blockState", state_tag(content_.info(BlockState::decode(be.moving).value())));
            t.set("facing", Tag::int_(static_cast<std::int32_t>(motion.facing)));
            t.set("progress", Tag::float_(static_cast<float>(be.progress) * 0.5f));
            t.set("extending", Tag::boolean(motion.extending));
            t.set("source", Tag::boolean(motion.source));
            break;
        }
        default: break;
        }
        std::optional<Tag> name;
        Tag components;
        if (auto ok = content_.save(be.components, std::nullopt, components, &name); !ok) ++refused_[ok.error()];
        t.set("components", std::move(components));
        if (name) t.set("CustomName", std::move(*name));
        t.set("keepPacked", Tag::boolean(false));
        t.set("x", Tag::int_(pos.x));
        t.set("y", Tag::int_(pos.y));
        t.set("z", Tag::int_(pos.z));
        return t;
    }

    Tag entity_chunk_tag(ChunkPos pos, const std::vector<const gameplay::Entity*>& entities) {
        auto root = Tag::compound();
        root.set("DataVersion", Tag::int_(supported_data_version));
        root.set("Position", Tag::int_array({pos.x, pos.z}));
        auto list = Tag::list(TagType::compound);
        for (const auto* e : entities) {
            const auto doubles = [](std::initializer_list<double> v) {
                auto l = Tag::list(TagType::double_);
                for (const auto d : v) l.push(Tag::double_(d));
                return l;
            };
            auto t = Tag::compound();
            const bool falling = e->kind == gameplay::EntityKind::falling_block;
            const bool tnt = e->kind == gameplay::EntityKind::primed_tnt;
            t.set("id", Tag::string(std::string(gameplay::entity_kind_name(e->kind))));
            t.set("Pos", doubles({e->position.x, e->position.y, e->position.z}));
            t.set("Motion", doubles({e->velocity.x, e->velocity.y, e->velocity.z}));
            auto rotation = Tag::list(TagType::float_);
            rotation.push(Tag::float_(e->kind == gameplay::EntityKind::minecart ? e->yaw : 0.0f));
            rotation.push(Tag::float_(0.0f));
            t.set("Rotation", std::move(rotation));
            t.set("OnGround", Tag::boolean(e->on_ground));
            t.set("fall_distance", Tag::double_(e->fall_distance));
            const auto hi = std::uint64_t{0x43697263756974ULL}, lo = e->id.value;
            if (e->kind == gameplay::EntityKind::minecart || e->kind == gameplay::EntityKind::boat) {
                // As the target versions save an empty minecart or boat.
                if (e->kind == gameplay::EntityKind::boat) t.set("id", Tag::string(std::string(gameplay::boat_types.at(e->variant))));
                t.set("Air", Tag::short_(300));
                t.set("Fire", Tag::short_(-1));
                t.set("Invulnerable", Tag::boolean(false));
                t.set("PortalCooldown", Tag::int_(0));
                t.set("UUID", Tag::int_array({static_cast<std::int32_t>(hi >> 32), static_cast<std::int32_t>(hi & 0xFFFFFFFF),
                                              static_cast<std::int32_t>(lo >> 32), static_cast<std::int32_t>(lo & 0xFFFFFFFF)}));
                list.push(std::move(t));
                ++report_.entities;
                continue;
            }
            if (e->kind == gameplay::EntityKind::arrow || e->kind == gameplay::EntityKind::snowball) {
                // As the target versions save a summoned arrow or snowball.
                if (e->kind == gameplay::EntityKind::arrow) {
                    t.set("life", Tag::short_(static_cast<std::int16_t>(e->life)));
                    t.set("inGround", Tag::boolean(e->in_ground));
                    t.set("shake", Tag::byte(0));
                    t.set("pickup", Tag::byte(0));
                    t.set("crit", Tag::boolean(false));
                    t.set("damage", Tag::double_(2.0));
                    t.set("PierceLevel", Tag::byte(0));
                    auto item = Tag::compound();
                    item.set("id", Tag::string("minecraft:arrow"));
                    item.set("count", Tag::int_(1));
                    t.set("item", std::move(item));
                }
                t.set("Air", Tag::short_(300));
                t.set("Fire", Tag::short_(static_cast<std::int16_t>(e->kind == gameplay::EntityKind::arrow && e->fire > 0 ? e->fire : -1)));
                t.set("UUID", Tag::int_array({static_cast<std::int32_t>(hi >> 32), static_cast<std::int32_t>(hi & 0xFFFFFFFF),
                                              static_cast<std::int32_t>(lo >> 32), static_cast<std::int32_t>(lo & 0xFFFFFFFF)}));
                list.push(std::move(t));
                ++report_.entities;
                continue;
            }
            if (tnt) {
                // As the target versions save TNT primed by redstone.
                t.set("fuse", Tag::short_(static_cast<std::int16_t>(std::clamp(e->fuse, -32768, 32767))));
                if (const auto block = gameplay::registry::find_block(bundle_.schema.content_release, "tnt"))
                    t.set("block_state", state_tag(content_.info(BlockState::registered(gameplay::registry::block(*block).default_state))));
                t.set("explosion_power", Tag::float_(4.0f));
                t.set("Air", Tag::short_(300));
                t.set("Fire", Tag::short_(-1));
                t.set("UUID", Tag::int_array({static_cast<std::int32_t>(hi >> 32), static_cast<std::int32_t>(hi & 0xFFFFFFFF),
                                              static_cast<std::int32_t>(lo >> 32), static_cast<std::int32_t>(lo & 0xFFFFFFFF)}));
                list.push(std::move(t));
                ++report_.entities;
                continue;
            }
            if (falling) {
                // As the target versions save a falling block of sand or gravel.
                t.set("BlockState", state_tag(content_.info(e->carried)));
                t.set("Time", Tag::int_(static_cast<std::int32_t>(std::min<std::uint32_t>(e->fall_time, INT32_MAX))));
                t.set("DropItem", Tag::boolean(true));
                t.set("HurtEntities", Tag::boolean(false));
                t.set("FallHurtMax", Tag::int_(40));
                t.set("FallHurtAmount", Tag::float_(0.0f));
                t.set("CancelDrop", Tag::boolean(false));
                t.set("Air", Tag::short_(300));
                t.set("Fire", Tag::short_(-1));
                t.set("UUID", Tag::int_array({static_cast<std::int32_t>(hi >> 32), static_cast<std::int32_t>(hi & 0xFFFFFFFF),
                                              static_cast<std::int32_t>(lo >> 32), static_cast<std::int32_t>(lo & 0xFFFFFFFF)}));
                list.push(std::move(t));
                ++report_.entities;
                continue;
            }
            t.set("Age", Tag::short_(static_cast<std::int16_t>(std::min(e->age, gameplay::item_despawn_age))));
            t.set("Health", Tag::short_(static_cast<std::int16_t>(e->health)));
            t.set("PickupDelay", Tag::short_(static_cast<std::int16_t>(e->pickup_delay)));
            t.set("Air", Tag::short_(300));
            t.set("Fire", Tag::short_(static_cast<std::int16_t>(e->fire > 0 ? e->fire : -1)));
            // A UUID derived from the Circuit entity id ("Circuit" in the high bits).
            t.set("UUID", Tag::int_array({static_cast<std::int32_t>(hi >> 32), static_cast<std::int32_t>(hi & 0xFFFFFFFF),
                                          static_cast<std::int32_t>(lo >> 32), static_cast<std::int32_t>(lo & 0xFFFFFFFF)}));
            auto item = Tag::compound();
            item.set("id", Tag::string(content_.item_name(e->stack.item)));
            item.set("count", Tag::int_(e->stack.count));
            components(item, e->stack);
            t.set("Item", std::move(item));
            list.push(std::move(t));
            ++report_.entities;
        }
        root.set("Entities", std::move(list));
        return root;
    }

    Tag level_tag() const {
        auto data = Tag::compound();
        data.set("DataVersion", Tag::int_(supported_data_version));
        data.set("version", Tag::int_(level_dat_version));
        auto version = Tag::compound();
        version.set("Id", Tag::int_(supported_data_version));
        version.set("Name", Tag::string("1.21.8"));
        version.set("Series", Tag::string("main"));
        version.set("Snapshot", Tag::boolean(false));
        data.set("Version", std::move(version));
        data.set("LevelName", Tag::string(options_.level_name));
        data.set("Time", Tag::long_(static_cast<std::int64_t>(world_.time)));
        data.set("DayTime", Tag::long_(world_.day_time));
        data.set("clearWeatherTime", Tag::int_(world_.weather.clear_time));
        data.set("rainTime", Tag::int_(world_.weather.rain_time));
        data.set("thunderTime", Tag::int_(world_.weather.thunder_time));
        data.set("raining", Tag::boolean(world_.weather.raining));
        data.set("thundering", Tag::boolean(world_.weather.thundering));
        auto rules = Tag::compound();
        for (const auto& rule : bundle_.schema.game_rules) {
            if (rule.experimental) continue;
            const auto v = gameplay::game_rule_value(bundle_, world_.game_rules, rule.name);
            rules.set(std::string(rule.name),
                      Tag::string(rule.type == contracts::GameRuleType::boolean ? (v != 0 ? "true" : "false") : std::to_string(v)));
        }
        data.set("GameRules", std::move(rules));
        data.set("GameType", Tag::int_(1));
        data.set("allowCommands", Tag::boolean(true));
        data.set("hardcore", Tag::boolean(false));
        data.set("initialized", Tag::boolean(true));
        data.set("Difficulty", Tag::byte(static_cast<std::int8_t>(world_.difficulty)));
        data.set("SpawnX", Tag::int_(options_.spawn.x));
        data.set("SpawnY", Tag::int_(options_.spawn.y + 1));
        data.set("SpawnZ", Tag::int_(options_.spawn.z));
        data.set("SpawnAngle", Tag::float_(0.0f));
        auto packs = Tag::compound();
        auto enabled = Tag::list(TagType::string);
        enabled.push(Tag::string("vanilla"));
        packs.set("Enabled", std::move(enabled));
        packs.set("Disabled", Tag::list(TagType::string));
        data.set("DataPacks", std::move(packs));
        data.set("WorldGenSettings", world_gen());
        // The End has not been visited: the game sets the fight up when it is.
        auto dragon = Tag::compound();
        dragon.set("NeedsStateScanning", Tag::boolean(true));
        dragon.set("DragonKilled", Tag::boolean(false));
        dragon.set("PreviouslyKilled", Tag::boolean(false));
        data.set("DragonFight", std::move(dragon));
        auto root = Tag::compound();
        root.set("Data", std::move(data));
        return root;
    }

    // A superflat overworld matching Circuit's generator: air below one layer of
    // stone at the floor height. The other dimensions use their presets.
    Tag world_gen() const {
        auto gen = Tag::compound();
        gen.set("seed", Tag::long_(static_cast<std::int64_t>(options_.seed)));
        gen.set("generate_features", Tag::boolean(false));
        gen.set("bonus_chest", Tag::boolean(false));
        auto layers = Tag::list(TagType::compound);
        const auto layer = [](std::string block, std::int32_t height) {
            auto l = Tag::compound();
            l.set("block", Tag::string(std::move(block)));
            l.set("height", Tag::int_(height));
            return l;
        };
        const auto air_below = options_.floor_y - bundle_.world.min_y;
        if (air_below > 0) layers.push(layer("minecraft:air", air_below));
        layers.push(layer("minecraft:stone", 1));
        auto settings = Tag::compound();
        settings.set("layers", std::move(layers));
        settings.set("biome", Tag::string("minecraft:plains"));
        settings.set("features", Tag::boolean(false));
        settings.set("lakes", Tag::boolean(false));
        settings.set("structure_overrides", Tag::list(TagType::string));
        auto generator = Tag::compound();
        generator.set("type", Tag::string("minecraft:flat"));
        generator.set("settings", std::move(settings));
        auto overworld = Tag::compound();
        overworld.set("type", Tag::string("minecraft:overworld"));
        overworld.set("generator", std::move(generator));
        const auto noise = [this](std::string type, std::string settings, std::string preset) {
            auto biome_source = Tag::compound();
            biome_source.set("type", Tag::string(preset == "minecraft:the_end" ? "minecraft:the_end" : "minecraft:multi_noise"));
            if (preset != "minecraft:the_end") biome_source.set("preset", Tag::string(preset));
            auto g = Tag::compound();
            g.set("type", Tag::string("minecraft:noise"));
            g.set("settings", Tag::string(std::move(settings)));
            g.set("biome_source", std::move(biome_source));
            auto d = Tag::compound();
            d.set("type", Tag::string(std::move(type)));
            d.set("generator", std::move(g));
            return d;
        };
        auto dimensions = Tag::compound();
        dimensions.set("minecraft:overworld", std::move(overworld));
        dimensions.set("minecraft:the_nether", noise("minecraft:the_nether", "minecraft:nether", "minecraft:nether"));
        dimensions.set("minecraft:the_end", noise("minecraft:the_end", "minecraft:end", "minecraft:the_end"));
        gen.set("dimensions", std::move(dimensions));
        return gen;
    }

    const WorldDump& world_;
    const gameplay::LevelDump& state_;
    const contracts::VersionBundle& bundle_;
    ExportOptions options_;
    Content content_;
    std::map<BlockPos, BlockState> blocks_;
    std::vector<protocol::BlockStateInfo> infos_;
    std::unordered_map<std::string, std::int32_t> ids_;
    ExportReport report_;
    std::map<std::string, std::size_t> refused_;
    void components(Tag& item, const contracts::ItemStack& stack) {
        if (auto ok = content_.set_components(item, stack); !ok) ++refused_[ok.error()];
    }
};

} // namespace

Tag default_block_entity(std::uint32_t state, std::string_view release) {
    const auto type = gameplay::registry::block_entity_type(state);
    const auto block = gameplay::registry::block_of(state);
    const auto name = block ? gameplay::registry::game_name(*block) : std::string_view{};
    auto t = Tag::compound();
    t.set("id", Tag::string("minecraft:" + std::string(type)));
    if (release != "1.21.4") t.set("components", Tag::compound());
    const auto is = [&](std::initializer_list<std::string_view> kinds) { return std::ranges::find(kinds, type) != kinds.end(); };
    if (is({"dispenser", "dropper", "chest", "trapped_chest", "furnace", "smoker", "blast_furnace", "brewing_stand", "hopper", "barrel",
            "chiseled_bookshelf", "campfire", "crafter"}))
        t.set("Items", Tag::list(TagType::end));
    if (is({"furnace", "smoker", "blast_furnace"})) {
        t.set("RecipesUsed", Tag::compound());
        for (const auto* field : {"cooking_time_spent", "cooking_total_time", "lit_time_remaining", "lit_total_time"}) t.set(field, Tag::short_(0));
    }
    // Sign texts: empty lines, plain strings from 1.21.5, JSON text before.
    if (is({"sign", "hanging_sign"})) {
        for (const auto* side : {"front_text", "back_text"}) {
            auto text = Tag::compound();
            text.set("color", Tag::string("black"));
            text.set("has_glowing_text", Tag::byte(0));
            auto lines = Tag::list(TagType::string);
            for (int i = 0; i < 4; ++i) lines.push(Tag::string(release == "1.21.4" ? "\"\"" : ""));
            text.set("messages", std::move(lines));
            t.set(side, std::move(text));
        }
        t.set("is_waxed", Tag::byte(0));
    }
    if (type == "chiseled_bookshelf") t.set("last_interacted_slot", Tag::int_(-1));
    if (type == "mob_spawner") {
        for (const auto& [field, value] : std::initializer_list<std::pair<const char*, std::int16_t>>{
                 {"Delay", 20}, {"MaxNearbyEntities", 6}, {"MaxSpawnDelay", 800}, {"MinSpawnDelay", 200}, {"RequiredPlayerRange", 16},
                 {"SpawnCount", 4}, {"SpawnRange", 4}})
            t.set(field, Tag::short_(value));
        t.set("SpawnPotentials", Tag::list(TagType::end));
    }
    if (type == "brewing_stand") {
        t.set("BrewTime", Tag::short_(0));
        t.set("Fuel", Tag::byte(0));
    }
    if (type == "beacon") t.set("Levels", Tag::int_(0));
    if (type == "comparator") t.set("OutputSignal", Tag::int_(0));
    if (type == "hopper") t.set("TransferCooldown", Tag::int_(-1));
    if (type == "end_gateway") t.set("Age", Tag::long_(0));
    if (type == "campfire") {
        t.set("CookingTimes", Tag::int_array({0, 0, 0, 0}));
        t.set("CookingTotalTimes", Tag::int_array({0, 0, 0, 0}));
    }
    if (type == "beehive") t.set("bees", Tag::list(TagType::end));
    if (type == "sculk_catalyst") t.set("cursors", Tag::list(TagType::end));
    const auto listener = [] {
        auto selector = Tag::compound();
        selector.set("tick", Tag::long_(-1));
        auto l = Tag::compound();
        l.set("event_delay", Tag::int_(0));
        l.set("selector", std::move(selector));
        return l;
    };
    if (is({"sculk_sensor", "calibrated_sculk_sensor"})) {
        t.set("last_vibration_frequency", Tag::int_(0));
        t.set("listener", listener());
    }
    if (type == "sculk_shrieker") {
        t.set("listener", listener());
        t.set("warning_level", Tag::int_(0));
    }
    if (type == "crafter") {
        t.set("crafting_ticks_remaining", Tag::int_(0));
        t.set("disabled_slots", Tag::int_array({}));
        t.set("triggered", Tag::int_(0));
    }
    if (type == "vault") {
        auto key = Tag::compound();
        key.set("count", Tag::int_(1));
        key.set("id", Tag::string("minecraft:trial_key"));
        auto config = Tag::compound();
        config.set("key_item", std::move(key));
        t.set("config", std::move(config));
        t.set("server_data", Tag::compound());
        t.set("shared_data", Tag::compound());
    }
    if (type == "command_block") {
        t.set("Command", Tag::string(""));
        t.set("SuccessCount", Tag::int_(0));
        t.set("TrackOutput", Tag::byte(1));
        t.set("UpdateLastExecution", Tag::byte(1));
        t.set("auto", Tag::byte(name == "chain_command_block" ? 1 : 0)); // chain command blocks start always active
        t.set("conditionMet", Tag::byte(0));
        t.set("powered", Tag::byte(0));
    }
    if (type == "structure_block") {
        for (const auto& [field, value] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"author", ""}, {"metadata", ""}, {"mirror", "NONE"}, {"mode", "LOAD"}, {"name", ""}, {"rotation", "NONE"}})
            t.set(field, Tag::string(value));
        t.set("ignoreEntities", Tag::byte(1));
        t.set("integrity", Tag::float_(1.0f));
        for (const auto* field : {"posX", "posZ", "sizeX", "sizeY", "sizeZ"}) t.set(field, Tag::int_(0));
        t.set("posY", Tag::int_(1));
        for (const auto* field : {"powered", "showair"}) t.set(field, Tag::byte(0));
        t.set("showboundingbox", Tag::byte(1));
        t.set("seed", Tag::long_(0));
        if (release != "1.21.4") t.set("strict", Tag::byte(0));
    }
    if (type == "jigsaw") {
        for (const auto& [field, value] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"final_state", "minecraft:air"}, {"joint", "rollable"}, {"name", "minecraft:empty"}, {"pool", "minecraft:empty"},
                 {"target", "minecraft:empty"}})
            t.set(field, Tag::string(value));
        t.set("placement_priority", Tag::int_(0));
        t.set("selection_priority", Tag::int_(0));
    }
    if (type == "test_block") {
        t.set("message", Tag::string(""));
        t.set("mode", Tag::string("start"));
        t.set("powered", Tag::byte(0));
    }
    if (type == "test_instance_block") {
        auto data = Tag::compound();
        data.set("ignore_entities", Tag::byte(0));
        data.set("rotation", Tag::string("none"));
        data.set("size", Tag::int_array({0, 0, 0}));
        data.set("status", Tag::string("cleared"));
        t.set("data", std::move(data));
    }
    return t;
}

std::expected<ImportResult, std::string> import_world(const fs::path& source, const contracts::VersionBundle& bundle,
                                                      const ImportOptions& options) {
    try {
        return Importer(bundle, options).run(source);
    } catch (const std::invalid_argument& e) {
        return std::unexpected(e.what());
    }
}

std::expected<ExportReport, std::string> export_world(const gameplay::WorldDump& state, const contracts::VersionBundle& bundle,
                                                      const fs::path& target, const ExportOptions& options) {
    try {
        return Exporter(state, bundle, options).run(target);
    } catch (const std::invalid_argument& e) {
        return std::unexpected(e.what());
    }
}

} // namespace circuit::server::anvil
