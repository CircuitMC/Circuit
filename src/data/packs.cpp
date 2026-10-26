// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/auth/json.hpp>
#include <circuit/data/packs.hpp>
#include <circuit/data/zip.hpp>
#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <openssl/evp.h>
#include <set>

namespace circuit::data {
namespace fs = std::filesystem;
using auth::Json;

namespace {
constexpr std::array<std::string_view, 13> known_tag_registries{
    "block", "item", "entity_type", "fluid", "game_event", "damage_type", "enchantment", "banner_pattern",
    "instrument", "painting_variant", "point_of_interest_type", "cat_variant", "dialog",
};

std::string hex_digest(std::span<const std::byte> bytes, const EVP_MD* md) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(bytes.data(), bytes.size(), out, &length, md, nullptr);
    std::string hex;
    for (unsigned i = 0; i < length; ++i) hex += std::format("{:02x}", out[i]);
    return hex;
}

std::expected<std::vector<std::byte>, std::string> read_file(const fs::path& p, std::size_t max = std::size_t{512} << 20) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot read {}", p.string()));
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    if (size > max) return std::unexpected(std::format("{} is larger than {} bytes", p.string(), max));
    in.seekg(0);
    std::vector<std::byte> out(size);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!in) return std::unexpected(std::format("cannot read {}", p.string()));
    return out;
}

std::string text_of(std::span<const std::byte> b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

// The files of one pack: path under the pack root ("data/...") -> contents.
struct PackFiles {
    PackInfo info;
    std::map<std::string, std::string> files;
    std::string language; // the jar's English language file (JSON), when it has one
};

// pack.mcmeta must declare the bundle's data pack format, or a supported
// range that includes it; filters and overlays are not supported yet.
std::expected<void, std::string> check_meta(std::string_view text, const contracts::VersionBundle& bundle, const std::string& origin) {
    const auto meta = Json::parse(text);
    if (!meta || meta->kind() != Json::Kind::object) return std::unexpected(std::format("{}: invalid pack.mcmeta", origin));
    const auto* pack = meta->find("pack");
    const auto* format = pack ? pack->find("pack_format") : nullptr;
    if (!format || format->kind() != Json::Kind::number) return std::unexpected(std::format("{}: pack.mcmeta has no pack_format", origin));
    for (const auto* field : {"filter", "overlays"})
        if (meta->find(field)) return std::unexpected(std::format("{}: pack {} are not supported yet", origin, field == std::string_view("filter") ? "filters" : "overlays"));
    const int want = bundle.schema.data_pack_format;
    bool ok = static_cast<int>(format->as_number()) == want;
    if (const auto* range = pack->find("supported_formats")) {
        int lo = 0, hi = -1;
        if (range->kind() == Json::Kind::number) lo = hi = static_cast<int>(range->as_number());
        else if (range->kind() == Json::Kind::array && range->items().size() == 2) {
            lo = static_cast<int>(range->items()[0].as_number());
            hi = static_cast<int>(range->items()[1].as_number());
        } else if (range->kind() == Json::Kind::object) {
            const auto* a = range->find("min_inclusive");
            const auto* b = range->find("max_inclusive");
            if (a && b) {
                lo = static_cast<int>(a->as_number());
                hi = static_cast<int>(b->as_number());
            }
        }
        ok = ok || (want >= lo && want <= hi);
    }
    if (!ok)
        return std::unexpected(std::format("{}: data pack format {} does not include {}'s format {}", origin,
                                           static_cast<int>(format->as_number()), bundle.game_version, want));
    return {};
}

std::expected<PackFiles, std::string> vanilla_pack(const fs::path& jar, const contracts::VersionBundle& bundle) {
    auto bytes = read_file(jar);
    if (!bytes) return std::unexpected(bytes.error());
    const auto sha1 = hex_digest(*bytes, EVP_sha1());
    if (sha1 != bundle.schema.server_jar_sha1)
        return std::unexpected(std::format("{}: SHA-1 {} is not the official {} server jar ({})", jar.string(), sha1, bundle.game_version,
                                           bundle.schema.server_jar_sha1));
    auto outer = ZipArchive::open(*bytes);
    if (!outer) return std::unexpected(std::format("{}: {}", jar.string(), outer.error()));
    // The server jar bundles the game in META-INF/versions/<id>/server-<id>.jar.
    const auto inner_name = std::format("META-INF/versions/{0}/server-{0}.jar", bundle.game_version);
    const auto* inner_entry = outer->find(inner_name);
    if (!inner_entry) return std::unexpected(std::format("{}: no {}", jar.string(), inner_name));
    auto inner_bytes = outer->read(*inner_entry);
    if (!inner_bytes) return std::unexpected(inner_bytes.error());
    auto inner = ZipArchive::open(*inner_bytes);
    if (!inner) return std::unexpected(std::format("{}: {}", inner_name, inner.error()));
    // version.json must name the bundle's version, protocol, save version and pack format.
    const auto* version_entry = inner->find("version.json");
    if (!version_entry) return std::unexpected(std::format("{}: no version.json", inner_name));
    const auto version_text = inner->read(*version_entry);
    const auto version = version_text ? Json::parse(text_of(*version_text)) : std::unexpected(std::string("unreadable"));
    const auto number = [&](std::string_view key) {
        const auto* v = version ? version->find(key) : nullptr;
        return v && v->kind() == Json::Kind::number ? static_cast<int>(v->as_number()) : -1;
    };
    const auto* packs = version ? version->find("pack_version") : nullptr;
    const auto* data_format = packs ? packs->find("data") : nullptr;
    const auto* id = version ? version->string("id") : nullptr;
    if (!id || *id != bundle.game_version || number("protocol_version") != bundle.protocol || number("world_version") != bundle.data_version
        || !data_format || static_cast<int>(data_format->as_number()) != bundle.schema.data_pack_format)
        return std::unexpected(std::format("{}: version.json does not describe {}", jar.string(), bundle.id));
    PackFiles out;
    out.info = {"vanilla", jar.string(), hex_digest(*bytes, EVP_sha256()), 0};
    for (const auto& e : inner->entries()) {
        // The built-in pack; the experimental packs inside it are profiles.
        if (!e.name.starts_with("data/") || e.name.starts_with("data/minecraft/datapacks/") || !e.name.ends_with(".json")) continue;
        auto content = inner->read(e, std::size_t{16} << 20);
        if (!content) return std::unexpected(content.error());
        out.files.emplace(e.name, text_of(*content));
    }
    out.info.files = out.files.size();
    if (const auto* lang = inner->find("assets/minecraft/lang/en_us.json"))
        if (auto content = inner->read(*lang, std::size_t{16} << 20)) out.language = text_of(*content);
    return out;
}

std::expected<PackFiles, std::string> archive_pack(const fs::path& path, const contracts::VersionBundle& bundle) {
    auto bytes = read_file(path);
    if (!bytes) return std::unexpected(bytes.error());
    auto zip = ZipArchive::open(*bytes);
    if (!zip) return std::unexpected(std::format("{}: {}", path.string(), zip.error()));
    const auto* meta = zip->find("pack.mcmeta");
    if (!meta) return std::unexpected(std::format("{}: no pack.mcmeta", path.string()));
    const auto meta_text = zip->read(*meta);
    if (!meta_text) return std::unexpected(meta_text.error());
    if (auto ok = check_meta(text_of(*meta_text), bundle, path.string()); !ok) return std::unexpected(ok.error());
    PackFiles out;
    out.info = {path.filename().string(), path.string(), hex_digest(*bytes, EVP_sha256()), 0};
    for (const auto& e : zip->entries()) {
        if (!e.name.starts_with("data/") || !e.name.ends_with(".json")) continue;
        auto content = zip->read(e, std::size_t{16} << 20);
        if (!content) return std::unexpected(content.error());
        out.files.emplace(e.name, text_of(*content));
    }
    out.info.files = out.files.size();
    return out;
}

std::expected<PackFiles, std::string> directory_pack(const fs::path& root, const contracts::VersionBundle& bundle) {
    const auto meta = read_file(root / "pack.mcmeta", 1 << 20);
    if (!meta) return std::unexpected(std::format("{}: no pack.mcmeta", root.string()));
    if (auto ok = check_meta(text_of(*meta), bundle, root.string()); !ok) return std::unexpected(ok.error());
    PackFiles out;
    std::error_code ec;
    if (fs::is_directory(root / "data", ec))
        for (auto it = fs::recursive_directory_iterator(root / "data", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file() || it->path().extension() != ".json") continue;
            auto content = read_file(it->path(), std::size_t{16} << 20);
            if (!content) return std::unexpected(content.error());
            out.files.emplace(fs::relative(it->path(), root).generic_string(), text_of(*content));
        }
    if (ec) return std::unexpected(std::format("{}: {}", root.string(), ec.message()));
    // A directory's digest covers its files in path order.
    std::vector<std::byte> all;
    for (const auto& [path, text] : out.files) {
        for (const auto c : path) all.push_back(static_cast<std::byte>(c));
        all.push_back(std::byte{0});
        for (const auto c : text) all.push_back(static_cast<std::byte>(c));
        all.push_back(std::byte{0});
    }
    out.info = {root.filename().string(), root.string(), hex_digest(all, EVP_sha256()), out.files.size()};
    return out;
}

std::string with_namespace(std::string_view id) {
    return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}

// Splits "data/<namespace>/<kind...>/<path>.json" into kind and id.
struct Located {
    bool tag{};
    std::string kind; // tag registry, or resource kind
    std::string id;
};
std::optional<Located> locate(std::string_view path) {
    if (!path.starts_with("data/") || !path.ends_with(".json")) return std::nullopt;
    path.remove_prefix(5);
    path.remove_suffix(5);
    const auto slash = path.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const auto ns = path.substr(0, slash);
    auto rest = path.substr(slash + 1);
    Located out;
    if (rest.starts_with("tags/")) {
        out.tag = true;
        rest.remove_prefix(5);
    }
    // Worldgen registries have two segments ("worldgen/biome").
    std::size_t kind_end = rest.find('/');
    if (kind_end != std::string_view::npos && rest.substr(0, kind_end) == "worldgen") kind_end = rest.find('/', kind_end + 1);
    if (kind_end == std::string_view::npos) return std::nullopt;
    out.kind = std::string(rest.substr(0, kind_end));
    out.id = std::string(ns) + ":" + std::string(rest.substr(kind_end + 1));
    return out;
}

// A tag file's values, before flattening: entries and "#tag" references,
// each required or not.
struct TagValue {
    std::string id;
    bool required{true};
};
std::expected<std::pair<bool, std::vector<TagValue>>, std::string> parse_tag(const std::string& text, const std::string& where) {
    const auto json = Json::parse(text);
    if (!json || json->kind() != Json::Kind::object) return std::unexpected(std::format("{}: invalid JSON", where));
    const auto* values = json->find("values");
    if (!values || values->kind() != Json::Kind::array) return std::unexpected(std::format("{}: no values", where));
    const auto* replace = json->find("replace");
    std::vector<TagValue> out;
    for (const auto& v : values->items()) {
        if (v.kind() == Json::Kind::string) {
            out.push_back({v.as_string(), true});
        } else if (v.kind() == Json::Kind::object && v.string("id")) {
            const auto* required = v.find("required");
            out.push_back({*v.string("id"), !required || required->as_bool()});
        } else {
            return std::unexpected(std::format("{}: invalid tag value", where));
        }
    }
    return std::pair{replace && replace->kind() == Json::Kind::boolean && replace->as_bool(), std::move(out)};
}
} // namespace

std::span<const std::string_view> tag_registries() noexcept { return known_tag_registries; }

std::expected<DataPacks, std::string> DataPacks::load(const contracts::VersionBundle& bundle, const PackOptions& options,
                                                      const RegistryLookup& registries) {
    std::vector<PackFiles> packs;
    if (!options.vanilla_jar.empty()) {
        auto v = vanilla_pack(options.vanilla_jar, bundle);
        if (!v) return std::unexpected(v.error());
        packs.push_back(std::move(*v));
    }
    for (const auto& p : options.packs) {
        std::error_code ec;
        auto pack = fs::is_directory(p, ec) ? directory_pack(p, bundle) : archive_pack(p, bundle);
        if (!pack) return std::unexpected(pack.error());
        packs.push_back(std::move(*pack));
    }
    DataPacks out;
    // Precedence: later packs replace earlier files; tag files merge.
    std::map<std::pair<std::string, std::string>, std::vector<TagValue>> raw_tags;
    std::set<std::string> skipped;
    for (auto& pack : packs) {
        for (auto& [path, text] : pack.files) {
            const auto at = locate(path);
            if (!at) continue;
            if (!at->tag) {
                out.resources_[at->kind][at->id] = std::move(text);
                continue;
            }
            if (!registries || !registries(at->kind, "")) {
                skipped.insert(at->kind);
                continue;
            }
            auto parsed = parse_tag(text, std::format("{} ({})", path, pack.info.name));
            if (!parsed) return std::unexpected(parsed.error());
            auto& values = raw_tags[{at->kind, at->id}];
            if (parsed->first) values.clear();
            values.insert(values.end(), parsed->second.begin(), parsed->second.end());
        }
        out.packs_.push_back(pack.info);
        if (!pack.language.empty())
            if (const auto json = Json::parse(pack.language); json && json->kind() == Json::Kind::object)
                for (const auto& [key, value] : json->members())
                    if (value.kind() == Json::Kind::string) out.language_[key] = value.as_string();
    }
    for (const auto& s : skipped) out.notes_.push_back(std::format("tags of {} are not loaded: Circuit cannot list that registry", s));
    // Flattening: references resolve recursively; a cycle or a missing
    // required entry fails the load, as it fails a reload in the target version.
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> resolved;
    // The same entries in the order the files list them, each at its first place.
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> in_order;
    std::set<std::pair<std::string, std::string>> resolving;
    std::function<std::expected<const std::vector<std::string>*, std::string>(const std::pair<std::string, std::string>&)> resolve;
    resolve = [&](const std::pair<std::string, std::string>& key) -> std::expected<const std::vector<std::string>*, std::string> {
        if (const auto it = resolved.find(key); it != resolved.end()) return &it->second;
        const auto raw = raw_tags.find(key);
        if (raw == raw_tags.end()) return std::unexpected(std::format("tag #{} of {} does not exist", key.second, key.first));
        if (!resolving.insert(key).second) return std::unexpected(std::format("tag #{} of {} refers to itself", key.second, key.first));
        std::set<std::string> entries;
        std::vector<std::string> ordered;
        const auto add = [&](const std::string& id) {
            if (entries.insert(id).second) ordered.push_back(id);
        };
        for (const auto& v : raw->second) {
            if (v.id.starts_with('#')) {
                const auto sub_key = std::pair{key.first, with_namespace(v.id.substr(1))};
                const auto sub = resolve(sub_key);
                if (!sub) {
                    if (v.required) return std::unexpected(sub.error());
                    continue;
                }
                for (const auto& id : in_order[sub_key]) add(id);
            } else {
                const auto id = with_namespace(v.id);
                const auto known = registries(key.first, id);
                if (known && *known) add(id);
                else if (v.required)
                    return std::unexpected(std::format("tag #{} of {} lists {}, which the registry does not have", key.second, key.first, id));
            }
        }
        resolving.erase(key);
        in_order[key] = std::move(ordered);
        return &(resolved[key] = std::vector<std::string>(entries.begin(), entries.end()));
    };
    for (const auto& [key, values] : raw_tags) {
        auto r = resolve(key);
        if (!r) return std::unexpected(r.error());
        out.tags_[key.first][key.second] = **r;
        out.ordered_tags_[key.first][key.second] = in_order[key];
    }
    if (!out.packs_.empty()) {
        std::vector<std::byte> ids;
        for (const auto& p : out.packs_)
            for (const auto c : p.name + ":" + p.sha256 + ";") ids.push_back(static_cast<std::byte>(c));
        out.fingerprint_ = "data-" + hex_digest(ids, EVP_sha256()).substr(0, 16);
    }
    return out;
}

const std::vector<std::string>* DataPacks::tag(std::string_view registry, std::string_view name) const {
    const auto r = tags_.find(std::string(registry));
    if (r == tags_.end()) return nullptr;
    const auto t = r->second.find(with_namespace(name));
    return t == r->second.end() ? nullptr : &t->second;
}

const std::vector<std::string>* DataPacks::ordered_tag(std::string_view registry, std::string_view name) const {
    const auto r = ordered_tags_.find(std::string(registry));
    if (r == ordered_tags_.end()) return nullptr;
    const auto t = r->second.find(with_namespace(name));
    return t == r->second.end() ? nullptr : &t->second;
}

const std::map<std::string, std::string>* DataPacks::resources(std::string_view kind) const {
    const auto it = resources_.find(std::string(kind));
    return it == resources_.end() ? nullptr : &it->second;
}

} // namespace circuit::data
