// SPDX-License-Identifier: AGPL-3.0-only
// ZIP archives and data packs: reading, precedence, tag merging and
// flattening, format checks and refusals.
#include "../support/check.hpp"
#include <circuit/data/packs.hpp>
#include <circuit/data/zip.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <zlib.h>

using circuit::test::require;
using circuit::test::Suite;
namespace fs = std::filesystem;
namespace data = circuit::data;

namespace {
void put(std::vector<std::byte>& out, std::uint32_t v, int n) {
    for (int i = 0; i < n; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
// A ZIP archive of `files`, each stored or deflated.
std::vector<std::byte> make_zip(const std::vector<std::pair<std::string, std::string>>& files, bool compress) {
    std::vector<std::byte> out, central;
    for (const auto& [name, text] : files) {
        std::vector<std::byte> body(text.size() + 64);
        std::uint32_t size = static_cast<std::uint32_t>(text.size());
        if (compress) {
            z_stream z{};
            deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
            z.avail_in = static_cast<uInt>(text.size());
            z.next_out = reinterpret_cast<Bytef*>(body.data());
            z.avail_out = static_cast<uInt>(body.size());
            ::deflate(&z, Z_FINISH);
            body.resize(z.total_out);
            deflateEnd(&z);
        } else {
            body.assign(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size());
        }
        const auto crc = static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(text.data()), static_cast<uInt>(text.size())));
        const auto offset = static_cast<std::uint32_t>(out.size());
        const std::uint32_t method = compress ? 8 : 0;
        put(out, 0x04034b50, 4); put(out, 20, 2); put(out, 0, 2); put(out, method, 2); put(out, 0, 4);
        put(out, crc, 4); put(out, static_cast<std::uint32_t>(body.size()), 4); put(out, size, 4);
        put(out, static_cast<std::uint32_t>(name.size()), 2); put(out, 0, 2);
        for (const auto c : name) out.push_back(static_cast<std::byte>(c));
        out.insert(out.end(), body.begin(), body.end());
        put(central, 0x02014b50, 4); put(central, 20, 2); put(central, 20, 2); put(central, 0, 2); put(central, method, 2);
        put(central, 0, 4); put(central, crc, 4); put(central, static_cast<std::uint32_t>(body.size()), 4); put(central, size, 4);
        put(central, static_cast<std::uint32_t>(name.size()), 2); put(central, 0, 2); put(central, 0, 2); put(central, 0, 2);
        put(central, 0, 2); put(central, 0, 4); put(central, offset, 4);
        for (const auto c : name) central.push_back(static_cast<std::byte>(c));
    }
    const auto cd_offset = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put(out, 0x06054b50, 4); put(out, 0, 2); put(out, 0, 2);
    put(out, static_cast<std::uint32_t>(files.size()), 2); put(out, static_cast<std::uint32_t>(files.size()), 2);
    put(out, static_cast<std::uint32_t>(central.size()), 4); put(out, cd_offset, 4); put(out, 0, 2);
    return out;
}

struct TempDir {
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "circuit-data-XXXXXX").string();
        path = mkdtemp(pattern.data());
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

void write(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string meta(int format, std::string extra = "") {
    return std::format(R"({{"pack": {{"pack_format": {}, "description": "test"{}}}}})", format, extra);
}

const circuit::contracts::VersionBundle& bundle() { return **circuit::contracts::BundleRegistry::select_for_simulation("java-1.21.8"); }

// Blocks and items the tags in these tests may list.
const data::RegistryLookup lookup = [](std::string_view registry, std::string_view entry) -> std::optional<bool> {
    static const std::set<std::string_view> blocks{"minecraft:stone", "minecraft:dirt", "minecraft:oak_log", "minecraft:sand"};
    static const std::set<std::string_view> items{"minecraft:stick", "minecraft:diamond"};
    if (registry == "block") return entry.empty() || blocks.contains(entry);
    if (registry == "item") return entry.empty() || items.contains(entry);
    return std::nullopt;
};
} // namespace

int main() {
    Suite suite("data_tests");

    suite.run("ZIP archives stored and deflated", [] {
        for (const bool compress : {false, true}) {
            const auto bytes = make_zip({{"a.txt", "hello"}, {"dir/b.json", std::string(1000, 'x')}}, compress);
            const auto zip = data::ZipArchive::open(bytes).value();
            require(zip.entries().size() == 2 && zip.find("dir/b.json"), "entries");
            const auto b = zip.read(*zip.find("dir/b.json")).value();
            require(b.size() == 1000 && std::to_integer<char>(b[999]) == 'x', "contents");
            require(!zip.read(*zip.find("dir/b.json"), 10).has_value(), "size limit");
        }
        auto bad = make_zip({{"a.txt", "hello"}}, false);
        bad[30 + 5] = std::byte{'H'}; // the stored data: the CRC no longer matches
        const auto zip = data::ZipArchive::open(bad).value();
        require(zip.read(zip.entries()[0]).error().find("CRC") != std::string::npos, "CRC checked");
        const std::vector<std::byte> junk(100, std::byte{7});
        require(!data::ZipArchive::open(junk).has_value(), "not an archive");
        for (std::size_t cut = 1; cut < 40; ++cut) {
            const auto whole = make_zip({{"a.txt", "hello"}}, true);
            const std::span<const std::byte> part(whole.data(), whole.size() - cut);
            if (const auto z = data::ZipArchive::open(part); z)
                for (const auto& e : z->entries()) (void)z->read(e);
        }
    });

    suite.run("packs merge by precedence and tags flatten", [] {
        TempDir dir;
        const auto a = dir.path / "a", b = dir.path / "b";
        write(a / "pack.mcmeta", meta(81));
        write(a / "data/minecraft/tags/block/soft.json", R"({"values": ["minecraft:dirt", "#minecraft:sandy"]})");
        write(a / "data/minecraft/tags/block/sandy.json", R"({"values": ["minecraft:sand", {"id": "minecraft:gravel", "required": false}]})");
        write(a / "data/minecraft/recipe/stick.json", R"({"type": "a"})");
        write(a / "data/minecraft/tags/item/gems.json", R"({"values": ["minecraft:diamond"]})");
        write(a / "data/minecraft/tags/game_event/loud.json", R"({"values": ["minecraft:x"]})");
        write(b / "pack.mcmeta", meta(80, R"(, "supported_formats": [80, 90])"));
        write(b / "data/minecraft/tags/block/soft.json", R"({"values": ["minecraft:oak_log"]})");
        write(b / "data/minecraft/tags/item/gems.json", R"({"replace": true, "values": ["minecraft:stick"]})");
        write(b / "data/minecraft/recipe/stick.json", R"({"type": "b"})");
        // An archive pack after both.
        const auto zip = make_zip({{"pack.mcmeta", meta(81)}, {"data/custom/tags/block/hard.json", R"({"values": ["stone"]})"}}, true);
        std::ofstream(dir.path / "c.zip", std::ios::binary).write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
        const auto packs = data::DataPacks::load(bundle(), {{}, {a, b, dir.path / "c.zip"}}, lookup);
        require(packs.has_value(), packs ? "" : packs.error());
        require(packs->packs().size() == 3 && packs->packs()[2].name == "c.zip", "three packs in order");
        require(*packs->tag("block", "minecraft:soft") == std::vector<std::string>{"minecraft:dirt", "minecraft:oak_log", "minecraft:sand"},
                "merged, flattened, optional entry skipped");
        require(*packs->ordered_tag("block", "minecraft:soft") == std::vector<std::string>{"minecraft:dirt", "minecraft:sand", "minecraft:oak_log"},
                "in the files' order, a referenced tag's entries in its place");
        require(*packs->tag("item", "gems") == std::vector<std::string>{"minecraft:stick"}, "replace");
        require(*packs->tag("block", "custom:hard") == std::vector<std::string>{"minecraft:stone"}, "namespace defaults");
        require(packs->resources("recipe")->at("minecraft:stick") == R"({"type": "b"})", "a later pack replaces files");
        require(!packs->tag("game_event", "loud") && packs->notes().size() == 1 && packs->notes()[0].find("game_event") != std::string::npos,
                "registries Circuit cannot list are noted, not loaded");
        const auto again = data::DataPacks::load(bundle(), {{}, {a, b, dir.path / "c.zip"}}, lookup).value();
        require(again.fingerprint() == packs->fingerprint() && packs->fingerprint().starts_with("data-"), "fingerprint is stable");
        write(b / "data/minecraft/recipe/stick.json", R"({"type": "c"})");
        require(data::DataPacks::load(bundle(), {{}, {a, b, dir.path / "c.zip"}}, lookup)->fingerprint() != packs->fingerprint(),
                "fingerprint follows the contents");
        require(data::DataPacks::load(bundle(), {}, lookup)->fingerprint() == "none", "no packs");
    });

    suite.run("invalid packs are refused", [] {
        TempDir dir;
        const auto refused = [&](std::string_view what, const std::vector<std::pair<std::string, std::string>>& files) {
            const auto p = dir.path / std::format("p{}", std::hash<std::string_view>{}(what));
            for (const auto& [path, text] : files) write(p / path, text);
            const auto r = data::DataPacks::load(bundle(), {{}, {p}}, lookup);
            require(!r && r.error().find(what) != std::string::npos, std::format("expected `{}`, got `{}`", what, r ? "loaded" : r.error()));
        };
        refused("no pack.mcmeta", {{"data/minecraft/tags/block/x.json", R"({"values": []})"}});
        refused("does not include 1.21.8's format 81", {{"pack.mcmeta", meta(61)}});
        refused("filters are not supported", {{"pack.mcmeta", R"({"pack": {"pack_format": 81}, "filter": {"block": []}})"}});
        refused("overlays are not supported", {{"pack.mcmeta", R"({"pack": {"pack_format": 81}, "overlays": {"entries": []}})"}});
        refused("lists minecraft:bedrock", {{"pack.mcmeta", meta(81)}, {"data/minecraft/tags/block/x.json", R"({"values": ["minecraft:bedrock"]})"}});
        refused("refers to itself", {{"pack.mcmeta", meta(81)},
                                     {"data/minecraft/tags/block/x.json", R"({"values": ["#minecraft:y"]})"},
                                     {"data/minecraft/tags/block/y.json", R"({"values": ["#minecraft:x"]})"}});
        refused("does not exist", {{"pack.mcmeta", meta(81)}, {"data/minecraft/tags/block/x.json", R"({"values": ["#minecraft:none"]})"}});
        refused("invalid JSON", {{"pack.mcmeta", meta(81)}, {"data/minecraft/tags/block/x.json", R"({"values": [)"}});
        // A jar that is not the official one is refused by its SHA-1.
        const auto jar = make_zip({{"version.json", "{}"}}, false);
        std::ofstream(dir.path / "server.jar", std::ios::binary).write(reinterpret_cast<const char*>(jar.data()), static_cast<std::streamsize>(jar.size()));
        const auto r = data::DataPacks::load(bundle(), {dir.path / "server.jar", {}}, lookup);
        require(!r && r.error().find("is not the official 1.21.8 server jar") != std::string::npos, r ? "loaded" : r.error());
    });

    // With the operator's official jars (optional: CIRCUIT_VANILLA_JAR_1_21_8,
    // CIRCUIT_VANILLA_JAR_1_21_4), the built-in pack loads.
    suite.run("the official built-in pack, when a jar is given", [] {
        for (const auto& [id, variable] : {std::pair{"java-1.21.8", "CIRCUIT_VANILLA_JAR_1_21_8"}, std::pair{"java-1.21.4", "CIRCUIT_VANILLA_JAR_1_21_4"}}) {
            const char* jar = circuit::test::official_jar(variable);
            if (!jar) {
                std::cout << std::format("  {} not set: the official {} pack is not checked\n", variable, id);
                continue;
            }
            const auto& b = **circuit::contracts::BundleRegistry::find(id);
            const auto all = [](std::string_view, std::string_view) -> std::optional<bool> { return true; };
            const auto packs = data::DataPacks::load(b, {jar, {}}, all);
            require(packs.has_value(), packs ? "" : packs.error());
            require(packs->resources("enchantment")->size() == 42 && packs->tag("block", "minecraft:mineable/pickaxe"), "contents");
            // The official server sends as many tags of each registry Circuit numbers (tests/fixtures/registries).
            std::ifstream in(std::format("{}/tests/fixtures/registries/{}.txt", CIRCUIT_SOURCE_DIR, id));
            std::size_t compared = 0;
            for (std::string line; std::getline(in, line);) {
                if (!line.starts_with("tags ")) continue;
                const auto first = line.find(' ', 5);
                const auto registry = line.substr(15, first - 15); // after "tags minecraft:"
                const auto count = std::stoul(line.substr(first + 1));
                const auto it = packs->tags().find(registry);
                if (it == packs->tags().end()) continue; // not a registry the server can number
                require(it->second.size() == count, std::format("{}: {} tags of {}, the official server sends {}", id, it->second.size(), registry, count));
                ++compared;
            }
            require(compared >= 10, std::format("{} tag registries compared", compared));
            std::cout << std::format("  {}: {} files, {} tag registries\n", id, packs->packs()[0].files, packs->tags().size());
        }
    });

    return suite.finish();
}
