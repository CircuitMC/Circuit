// SPDX-License-Identifier: AGPL-3.0-only
#include "registries.hpp"
#include "world_codec.hpp"
#include <algorithm>
#include <bit>
#include <circuit/evidence/content.hpp>
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
        // Blocks without rules of their own have no scheduled behavior to resume.
        const auto s = block(*tables_->notation(*id));
        return s && !s->is(BlockType::other) ? std::optional<BlockType>(s->type()) : std::nullopt;
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
    std::string block_name(BlockType t) const {
        return protocol::parse_notation(mapping_->notation(BlockState::of(t)))->first;
    }

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
        if (fs::exists(source / "data")) note("saved data (maps, raids, scoreboards, world border) is not imported");
        note("light and heightmaps are not imported; Circuit has no lighting engine");
        // Containers that lost their data to unsupported content still need a block entity.
        for (const auto& [pos, raw] : result_.state.blocks) {
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
        result_.state.chunks.push_back({c.pos, 0, gameplay::ChunkLevel::unloaded});
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
            result_.state.blocks.push_back({BlockPos{chunk.x * 16 + static_cast<std::int32_t>(i & 15),
                                                     static_cast<std::int32_t>(*y) * 16 + static_cast<std::int32_t>(i >> 8),
                                                     chunk.z * 16 + static_cast<std::int32_t>((i >> 4) & 15)},
                                            s.raw()});
            ++report().blocks;
        }
        return {};
    }

    std::optional<BlockState> block_at(BlockPos p) const {
        // Blocks of the current chunk were appended last; search from the end.
        for (auto it = result_.state.blocks.rbegin(); it != result_.state.blocks.rend(); ++it) {
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
        } else if (id == "minecraft:comparator" && expect(BlockType::comparator)) {
            out = {BlockType::comparator, 0, {}, 0};
            out.signal = static_cast<std::uint8_t>(std::clamp<std::int64_t>(integer(be, "OutputSignal").value_or(0), 0, 15));
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
        if (fluid) type = id == "minecraft:water" || id == "minecraft:flowing_water" ? std::optional(BlockType::water) : std::nullopt;
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
        result_.state.ticks.push_back(p);
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
            if (id != "minecraft:item") {
                unsupported("entity " + id);
                continue;
            }
            const auto pos = numbers(e, "Pos", 3);
            const auto motion = numbers(e, "Motion", 3).value_or(std::vector<double>{0, 0, 0});
            const auto* item = child(e, "Item", TagType::compound);
            if (!pos || !item || !std::isfinite((*pos)[0]) || !std::isfinite((*pos)[1]) || !std::isfinite((*pos)[2])) {
                unsupported("malformed item entity");
                continue;
            }
            auto stack = content_.stack(*item);
            if (!stack) {
                unsupported(stack.error());
                continue;
            }
            if (e.find("Passengers")) unsupported("entity passengers");
            // Checked before conversion: inside the border, and no further below or
            // above the world than the rules let items fall or fly.
            if (std::abs((*pos)[0]) >= contracts::world_border || std::abs((*pos)[2]) >= contracts::world_border
                || (*pos)[1] < bundle_.world.min_y - 64.0 || (*pos)[1] >= bundle_.world.min_y + bundle_.world.height + 4096.0) {
                unsupported("item entity outside the world");
                continue;
            }
            const auto fixed = [](double v) { return static_cast<std::int64_t>(std::llround(v * contracts::fixed_one)); };
            gameplay::Entity out;
            out.position = {fixed((*pos)[0]), fixed((*pos)[1]), fixed((*pos)[2])};
            // The rules move items less than a block per tick on each axis.
            const auto speed = [&](double v) {
                const auto f = std::isfinite(v) ? fixed(std::clamp(v, -10.0, 10.0)) : 0;
                return std::clamp(f, -gameplay::max_entity_speed, gameplay::max_entity_speed);
            };
            out.velocity = {speed(motion[0]), speed(motion[1]), speed(motion[2])};
            const auto block = contracts::block_of(out.position);
            if (!imported_.contains(contracts::chunk_of(block))) {
                unsupported("item entity outside the imported chunks");
                continue;
            }
            out.stack = std::move(*stack);
            const auto age = integer(e, "Age").value_or(0);
            if (age < 0) ++approximated_; // never-despawning items age normally in Circuit
            out.age = static_cast<std::uint32_t>(std::max<std::int64_t>(age, 0));
            out.on_ground = integer(e, "OnGround").value_or(0) != 0;
            out.pickup_delay = static_cast<std::uint16_t>(std::clamp<std::int64_t>(integer(e, "PickupDelay").value_or(0), 0, 32767));
            out.last_tick = time_;
            entities_.push_back(out);
        }
        return {};
    }

    void finish() {
        auto& s = result_.state;
        s.time = time_;
        s.next_suborder = next_suborder_;
        std::uint64_t order = 0;
        for (auto& [pos, be] : block_entities_) {
            be.order = order++;
            s.block_entities.push_back({pos, std::move(be)});
        }
        s.next_block_entity_order = order;
        std::uint64_t id = 1;
        for (auto& e : entities_) {
            e.id = contracts::EntityId{id++};
            s.entities.push_back(e);
        }
        report().entities = entities_.size();
        s.next_entity_id = id;
        gameplay::canonicalize(s);
        if (proto_chunks_ > 0) note(std::format("{} chunks with incomplete generation were skipped", proto_chunks_));
        if (post_processing_ > 0) note(std::format("{} chunks had pending post-processing updates, which are dropped", post_processing_));
        if (approximated_ > 0)
            note(std::format("{} moving-piston progress or item-age values were rounded to Circuit's model", approximated_));
        note("block entity creation order and the order of entity ids follow chunk position, not the source world's history");
        note(std::format("positions and motion are stored in 1/{} block steps", contracts::fixed_one));
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
    Exporter(const WorldDump& state, const contracts::VersionBundle& bundle, const ExportOptions& options)
        : state_(state), bundle_(bundle), options_(options), content_(bundle, options.components) {
        for (const auto& [p, raw] : state.blocks) blocks_.emplace(p, BlockState::decode(raw).value());
    }

    std::expected<ExportReport, std::string> run(const fs::path& target) {
        if (!state_.block_events.empty())
            return std::unexpected("the state has pending block events, which the Anvil format cannot hold; export between ticks");
        std::error_code ec;
        if (fs::exists(target, ec)) return std::unexpected(std::format("{} already exists", target.string()));
        const auto partial = fs::path(target.string() + ".partial");
        fs::remove_all(partial, ec);
        if (!fs::create_directories(partial / "region", ec) || !fs::create_directories(partial / "entities", ec))
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
        return write_file(dir / "level.dat", gzip(named_nbt(level_tag())));
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
        root.set("LastUpdate", Tag::long_(static_cast<std::int64_t>(state_.time)));
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
            tag.set("i", Tag::string(fluid ? (at(t->tick.pos).is(BlockType::water) && at(t->tick.pos).level() == 0 && !at(t->tick.pos).flag()
                                                  ? "minecraft:water"
                                                  : "minecraft:flowing_water")
                                           : content_.block_name(t->tick.block)));
            tag.set("p", Tag::int_(t->tick.priority));
            tag.set("t", Tag::int_(static_cast<std::int32_t>(static_cast<std::int64_t>(t->tick.due) - static_cast<std::int64_t>(state_.time))));
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
            // Saved data of a block without rules of its own, as imported.
            protocol::Reader r(be.saved.bytes());
            if (auto saved = protocol::nbt::read_network(r); saved && saved->type() == TagType::compound) t = std::move(*saved);
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
        case BlockType::comparator:
            t.set("id", Tag::string("minecraft:comparator"));
            t.set("OutputSignal", Tag::int_(be.signal));
            break;
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
            const auto real = [](std::int64_t v) { return static_cast<double>(v) / contracts::fixed_one; };
            const auto doubles = [](std::initializer_list<double> v) {
                auto l = Tag::list(TagType::double_);
                for (const auto d : v) l.push(Tag::double_(d));
                return l;
            };
            auto t = Tag::compound();
            t.set("id", Tag::string("minecraft:item"));
            t.set("Pos", doubles({real(e->position.x), real(e->position.y), real(e->position.z)}));
            t.set("Motion", doubles({real(e->velocity.x), real(e->velocity.y), real(e->velocity.z)}));
            auto rotation = Tag::list(TagType::float_);
            rotation.push(Tag::float_(0.0f));
            rotation.push(Tag::float_(0.0f));
            t.set("Rotation", std::move(rotation));
            t.set("OnGround", Tag::boolean(e->on_ground));
            t.set("Age", Tag::short_(static_cast<std::int16_t>(std::min<std::uint32_t>(e->age, 6000))));
            t.set("Health", Tag::short_(5));
            t.set("PickupDelay", Tag::short_(static_cast<std::int16_t>(e->pickup_delay)));
            t.set("Air", Tag::short_(300));
            t.set("Fire", Tag::short_(-1));
            // A UUID derived from the Circuit entity id ("Circuit" in the high bits).
            const auto hi = std::uint64_t{0x43697263756974ULL}, lo = e->id.value;
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
        data.set("Time", Tag::long_(static_cast<std::int64_t>(state_.time)));
        data.set("DayTime", Tag::long_(6000));
        data.set("GameType", Tag::int_(1));
        data.set("allowCommands", Tag::boolean(true));
        data.set("hardcore", Tag::boolean(false));
        data.set("initialized", Tag::boolean(true));
        data.set("Difficulty", Tag::byte(2));
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

    const WorldDump& state_;
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
