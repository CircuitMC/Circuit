// SPDX-License-Identifier: AGPL-3.0-only
// Content data that does not need a running server: the data compilers that
// turn the official jar and data packs into GameData, the synchronized
// registries, the block palette of both protocols and the login cryptography.
// Moved from server_tests when the server was removed for the restructuring.
#include "../oracle/fixture_executor.hpp"
#include "../support/check.hpp"
#include "../support/fixture_parts.hpp"
#include "../../src/server/registries.hpp"
#include "../../src/server/server_data.hpp"
#include "../../src/server/world_codec.hpp"
#include <circuit/auth/crypto.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/evidence/content.hpp>
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/evidence/trace_format.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/loot.hpp>
#include <circuit/gameplay/physics.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <circuit/gameplay/survival.hpp>
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/components.hpp>
#include <circuit/protocol/content.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace circuit;
using namespace circuit::test;
using protocol::Writer;

namespace {
std::filesystem::path temporary_directory() {
    std::string templ = (std::filesystem::temp_directory_path() / "circuit-content-XXXXXX").string();
    require(::mkdtemp(templ.data()) != nullptr, "temporary directory");
    return templ;
}
} // namespace

int main() {
    Suite suite("content_tests");

    suite.run("protocol encryption primitives", [] {
        const auto digest = [](std::string_view s) {
            return auth::minecraft_hex_digest(std::as_bytes(std::span(s.data(), s.size())));
        };
        // Documented examples (minecraft.wiki "Java Edition protocol/Encryption").
        require(digest("Notch") == "4ed1f46bbe04bc756bcb17c0c7ce3e4632f06a48", "positive digest");
        require(digest("jeb_") == "-7c9d5b0044c130109a5d7b5fb5c317c02b4e28c1", "negative digest");
        require(digest("simon") == "88e16a1019277b15d58faf0541e11910eb756f6", "leading zero dropped");
        const auto keys = auth::RsaKeyPair::generate();
        require(keys.public_der().size() > 100 && keys.public_der().size() < 200, "1024-bit SubjectPublicKeyInfo");
        const auto secret = auth::random_bytes(16);
        const auto sealed = auth::rsa_encrypt(keys.public_der(), secret);
        require(sealed && sealed->size() == 128 && keys.decrypt(*sealed).value() == secret, "RSA round trip");
        // OpenSSL rejects bad padding implicitly: garbage yields unrelated bytes,
        // never the secret, so the verify-token comparison refuses it.
        const auto garbage = keys.decrypt(auth::random_bytes(128));
        require(!garbage || *garbage != secret, "garbage does not yield the secret");
        // CFB8 is a stream cipher: chunking does not change the output.
        auto plain = auth::random_bytes(100);
        auto whole = plain, pieces = plain;
        auth::Cfb8 a(secret, auth::Cfb8::Direction::encrypt), b(secret, auth::Cfb8::Direction::encrypt);
        a.apply(whole);
        b.apply(std::span(pieces).first(7));
        b.apply(std::span(pieces).subspan(7));
        require(whole == pieces && whole != plain, "continuous stream");
        auth::Cfb8 d(secret, auth::Cfb8::Direction::decrypt);
        d.apply(whole);
        require(whole == plain, "decrypts");
    });

    suite.run("synchronized registries and game rules match the official servers", [] {
        // The tables are compiled from public lists; the official servers'
        // configuration and command trees (tests/fixtures/registries) check them.
        for (const auto* id : {"java-1.21.8", "java-1.21.4"}) {
            const auto& bundle = **contracts::BundleRegistry::find(id);
            std::map<std::string, std::set<std::string>> observed;
            std::set<std::string> rules;
            std::ifstream in(std::format("{}/tests/fixtures/registries/{}.txt", CIRCUIT_SOURCE_DIR, id));
            for (std::string line; std::getline(in, line);) {
                std::istringstream words(line);
                std::string kind, name, entry;
                words >> kind;
                if (kind == "registry") {
                    words >> name;
                    auto& set = observed[name];
                    while (words >> entry) set.insert(entry);
                } else if (kind == "game_rules") {
                    while (words >> entry) rules.insert(entry);
                }
            }
            require(!observed.empty() && !rules.empty(), "observations read");
            const auto* registries = server::synced_registries(bundle.schema.registries);
            std::set<std::string> sent;
            for (const auto& r : *registries) {
                const std::set<std::string> entries(r.entries.begin(), r.entries.end());
                require(observed.contains(std::string(r.registry)) && observed.at(std::string(r.registry)) == entries,
                        std::format("{}: {} matches the official server", id, r.registry));
                sent.insert(std::string(r.registry));
            }
            // Registries the official server sends that Circuit does not, yet.
            std::set<std::string> missing;
            for (const auto& [name, entries] : observed)
                if (!sent.contains(name)) missing.insert(name);
            const std::set<std::string> expected_missing = std::string_view(id) == "java-1.21.8"
                ? std::set<std::string>{"minecraft:dialog", "minecraft:enchantment", "minecraft:test_environment", "minecraft:test_instance"}
                : std::set<std::string>{"minecraft:enchantment"};
            require(missing == expected_missing, std::format("{}: {} registries not sent", id, missing.size()));
            std::set<std::string> names;
            for (const auto& r : bundle.schema.game_rules)
                if (!r.experimental) names.insert(std::string(r.name));
            require(names == rules, std::format("{}: {} game rules, {} observed", id, names.size(), rules.size()));
        }
    });

    suite.run("every block state of both protocols has a palette entry", [] {
        // (Every state's collision boxes: evidence_tests, against the official servers.)
        for (const auto& [protocol, version] : {std::pair{772, "1.21.8"}, std::pair{769, "1.21.4"}}) {
            const auto& tables = *protocol::ContentTables::for_protocol(protocol);
            const auto& mapping = *evidence::ContentMapping::for_game_version(version);
            const server::BlockPalette palette(tables, mapping);
            for (std::int32_t id = 0; id < tables.state_count(); ++id) {
                const auto s = palette.reference(id);
                require(s && palette.wire(*s) == id, std::format("{}: state {} maps both ways", version, id));
                require(mapping.block(*tables.notation(id)) == *s, std::format("{}: state {} is {}", version, id, *tables.notation(id)));
            }
        }
    });


    // A data pack directory: the 42 enchantments, a block tag that makes dirt
    // need an iron pickaxe, and an enchantment tag.
    const auto make_pack = [](const std::filesystem::path& root, std::string_view extra_tag) {
        const auto file = [&](const std::filesystem::path& rel, std::string_view text) {
            std::filesystem::create_directories((root / rel).parent_path());
            std::ofstream(root / rel) << text;
        };
        file("pack.mcmeta", R"({"pack": {"pack_format": 81, "description": "test"}})");
        for (const auto e : server::enchantment_registry().entries)
            file(std::format("data/minecraft/enchantment/{}.json", e.substr(10)), "{}");
        file("data/minecraft/tags/block/mineable/pickaxe.json", R"({"values": ["minecraft:dirt"]})");
        file("data/minecraft/tags/block/needs_iron_tool.json", R"({"values": ["minecraft:dirt"]})");
        file("data/minecraft/tags/enchantment/sharp.json", std::format(R"({{"values": ["minecraft:sharpness"{}]}})", extra_tag));
    };

    suite.run("data packs give registries, tags, tools and components", [&] {
        const auto dir = temporary_directory();
        make_pack(dir / "pack", "");
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        const auto d = server::load_server_data(bundle, {{}, {dir / "pack"}});
        require(d.has_value(), d ? "" : d.error());
        require(std::ranges::any_of(d->registries, [](const auto& r) { return r.registry == "minecraft:enchantment"; }),
                "enchantments are synchronized with their tags");
        const auto& content = *protocol::ContentTables::for_protocol(772);
        const auto find_tag = [&](std::string_view registry, std::string_view name) -> std::vector<std::int32_t> {
            for (const auto& r : d->tags)
                if (r.registry == registry)
                    for (const auto& [n, ids] : r.tags)
                        if (n == name) return ids;
            return {-1};
        };
        require(find_tag("minecraft:block", "minecraft:mineable/pickaxe") == std::vector<std::int32_t>{*content.block_id("minecraft:dirt")},
                "block tags by block id");
        const auto sharpness = server::registry_index(d->registries, "minecraft:enchantment", "minecraft:sharpness");
        require(find_tag("minecraft:enchantment", "minecraft:sharp") == std::vector<std::int32_t>{*sharpness}, "enchantment tags by id");
        // Enchantments are accepted only with the data.
        const auto& codec = *protocol::ComponentCodec::for_protocol(772);
        Writer w;
        w.varint(1).varint(0).varint(*codec.type_id("enchantments")).varint(1).varint(*sharpness).varint(5);
        protocol::Reader with(w.bytes()), without(w.bytes());
        require(codec.read(with, d->components).has_value() && !codec.read(without, server::component_context(bundle)).has_value(),
                "enchantment components");
        // Dirt now needs an iron pickaxe for its drops.
        const auto dirt = gameplay::registry::find_block("1.21.8", "minecraft:dirt").value();
        const auto& tool = d->game->block_tools.at(dirt);
        require(tool.tool == gameplay::registry::Tool::pickaxe && tool.tier == 3, "tool and tier from the tags");
        const auto dirt_state = *evidence::ContentMapping::for_game_version("1.21.8")->block("minecraft:dirt");
        const contracts::ItemStack iron{gameplay::items::iron_pickaxe, 1}, stone{gameplay::items::stone_pickaxe, 1};
        require(gameplay::harvests(dirt_state, iron, d->game.get()) && !gameplay::harvests(dirt_state, stone, d->game.get()) &&
                    gameplay::harvests(dirt_state, stone),
                "harvesting follows the data");
        // Packs must define the vanilla enchantments, no more and no fewer.
        std::filesystem::remove(dir / "pack/data/minecraft/enchantment/mending.json");
        const auto fewer = server::load_server_data(bundle, {{}, {dir / "pack"}});
        require(!fewer && fewer.error().find("add or remove enchantments") != std::string::npos, "changed enchantments refused");
        std::filesystem::remove_all(dir);
    });

    suite.run("recipes of several namespaces in the official order: by path, then namespace", [&] {
        const auto dir = temporary_directory();
        make_pack(dir / "pack", "");
        const auto recipe = [&](std::string_view id, std::string_view json) {
            const auto colon = id.find(':');
            const auto rel = std::filesystem::path("data") / id.substr(0, colon) / "recipe" / std::format("{}.json", id.substr(colon + 1));
            std::filesystem::create_directories((dir / "pack" / rel).parent_path());
            std::ofstream(dir / "pack" / rel) << json;
        };
        // The test pack of client/ns_crafting and client/ns_book, with two special recipes.
        const auto shapeless = [](std::string_view a, std::string_view b, std::string_view result) {
            return std::format(R"({{"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:{}", "minecraft:{}"], "result": {{"id": "minecraft:{}"}}}})",
                               a, b, result);
        };
        const auto smelt = [](std::string_view result) {
            return std::format(R"({{"type": "minecraft:smelting", "ingredient": "minecraft:dirt", "result": {{"id": "minecraft:{}"}}, "cookingtime": 40}})", result);
        };
        recipe("aaa:zzz_mix", shapeless("stick", "dirt", "oak_planks"));
        recipe("zzz:aaa_mix", shapeless("stick", "dirt", "cobblestone"));
        recipe("bbb:same_mix", shapeless("stick", "cobblestone", "birch_log"));
        recipe("aaa:same_mix", shapeless("stick", "cobblestone", "oak_log"));
        recipe("example:rocket", shapeless("paper", "gunpowder", "stick"));
        recipe("test:a_rocket", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:paper", "minecraft:gunpowder", "minecraft:gunpowder"], "result": {"id": "minecraft:bone"}})");
        recipe("aaa:zzz_smelt", smelt("oak_planks"));
        recipe("zzz:aaa_smelt", smelt("cobblestone"));
        recipe("minecraft:firework_rocket", R"({"type": "minecraft:crafting_special_firework_rocket"})");
        recipe("aaa:repair", R"({"type": "minecraft:crafting_special_repairitem"})");
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        const auto d = server::load_server_data(bundle, {{}, {dir / "pack"}});
        require(d.has_value() && d->recipe_book, d ? "" : d.error());
        const auto ids = [](const auto& list) {
            std::vector<std::string> out;
            for (const auto& r : list) out.push_back(r.id);
            return out;
        };
        require(ids(d->game->crafting) == std::vector<std::string>{"test:a_rocket", "zzz:aaa_mix", "example:rocket", "aaa:same_mix", "bbb:same_mix", "aaa:zzz_mix"},
                "crafting recipes by path, then namespace");
        require(ids(d->game->cooking) == std::vector<std::string>{"zzz:aaa_smelt", "aaa:zzz_smelt"}, "cooking recipes too");
        require(ids(d->game->special) == std::vector<std::string>{"minecraft:firework_rocket", "aaa:repair"}, "special recipes too");
        auto book = ids(d->recipe_book->recipes);
        std::erase_if(book, [](const std::string& id) { return id == "minecraft:firework_rocket" || id == "aaa:repair"; });
        require(book == std::vector<std::string>{"test:a_rocket", "zzz:aaa_mix", "zzz:aaa_smelt", "example:rocket", "aaa:same_mix", "bbb:same_mix", "aaa:zzz_mix", "aaa:zzz_smelt"},
                "the recipe book numbers them in that order (client/ns_book)");
        // The grids make the first in that order (client/ns_crafting).
        std::array<contracts::ItemStack, 4> grid{};
        grid[0] = {gameplay::items::stick, 1};
        grid[1] = {gameplay::items::dirt, 1};
        require(gameplay::crafted(*d->game, {2, 2, grid}) == contracts::ItemStack{gameplay::items::cobblestone, 1}, "zzz:aaa_mix before aaa:zzz_mix");
        grid[0] = {*gameplay::items::by_name("minecraft:paper"), 1};
        grid[1] = {*gameplay::items::by_name("minecraft:gunpowder"), 1};
        const auto rockets = gameplay::crafted(*d->game, {2, 2, grid});
        require(rockets.item == *gameplay::items::by_name("minecraft:firework_rocket") && rockets.count == 3, "minecraft:firework_rocket before example:rocket");
        grid[2] = grid[1];
        require(gameplay::crafted(*d->game, {2, 2, grid}) == contracts::ItemStack{*gameplay::items::by_name("minecraft:bone"), 1},
                "test:a_rocket before minecraft:firework_rocket");
        std::filesystem::remove_all(dir);
    });

    suite.run("data packs give block loot tables and recipes", [&] {
        const auto dir = temporary_directory();
        make_pack(dir / "pack", "");
        const auto file = [&](const std::filesystem::path& rel, std::string_view text) {
            std::filesystem::create_directories((dir / "pack" / rel).parent_path());
            std::ofstream(dir / "pack" / rel) << text;
        };
        const auto table = [&](std::string_view block, std::string_view pool) {
            file(std::format("data/minecraft/loot_table/blocks/{}.json", block),
                 std::format(R"({{"type": "minecraft:block", "pools": [{{"rolls": 1, "bonus_rolls": 0, {}}}]}})", pool));
        };
        table("bookshelf", R"("entries": [{"type": "minecraft:alternatives", "children": [
            {"type": "minecraft:item", "name": "minecraft:bookshelf", "conditions": [{"condition": "minecraft:match_tool",
              "predicate": {"predicates": {"minecraft:enchantments": [{"enchantments": "minecraft:silk_touch", "levels": {"min": 1}}]}}}]},
            {"type": "minecraft:item", "name": "minecraft:book", "functions": [{"function": "minecraft:set_count", "count": 3, "add": false},
              {"function": "minecraft:explosion_decay"}]}]}], "conditions": [{"condition": "minecraft:survives_explosion"}])");
        table("coal_ore", R"("entries": [{"type": "minecraft:item", "name": "minecraft:coal", "functions": [{"function": "minecraft:apply_bonus",
            "enchantment": "minecraft:fortune", "formula": "minecraft:ore_drops"}]}])");
        table("lilac", R"("entries": [{"type": "minecraft:item", "name": "minecraft:lilac", "conditions": [{"condition":
            "minecraft:block_state_property", "block": "minecraft:lilac", "properties": {"half": "lower"}}]}])");
        table("glowstone", R"("entries": [{"type": "minecraft:item", "name": "minecraft:glowstone_dust", "functions": [{"function":
            "minecraft:set_count", "count": {"type": "minecraft:uniform", "min": 2, "max": 4}}]}])");
        table("gravel", R"("entries": [{"type": "minecraft:item", "name": "minecraft:flint", "conditions": [{"condition": "minecraft:table_bonus",
            "enchantment": "minecraft:fortune", "chances": [0.1, 0.14]}]}])");
        table("clay", R"("entries": [{"type": "minecraft:dynamic", "name": "minecraft:contents"}])");
        file("data/minecraft/tags/item/test_logs.json", R"({"values": ["minecraft:oak_log", "minecraft:birch_log"]})");
        const auto recipe = [&](std::string_view name, std::string_view json) { file(std::format("data/test/recipe/{}.json", name), json); };
        recipe("planks", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["#minecraft:test_logs"], "result": {"id": "minecraft:oak_planks", "count": 4}})");
        recipe("shears", R"({"type": "minecraft:crafting_shaped", "category": "equipment", "pattern": [" #", "# "], "key": {"#": "minecraft:iron_ingot"},
            "result": {"id": "minecraft:shears"}})");
        recipe("box", R"({"type": "minecraft:crafting_transmute", "input": "minecraft:shulker_box", "material": "minecraft:red_dye",
            "result": {"id": "minecraft:red_shulker_box"}})");
        recipe("repair", R"({"type": "minecraft:crafting_special_repairitem", "category": "equipment"})");
        recipe("stew", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:bowl", ["minecraft:poppy", "minecraft:dandelion"]],
            "result": {"id": "minecraft:suspicious_stew", "components": {"minecraft:suspicious_stew_effects": []}}})");
        recipe("smelt", R"({"type": "minecraft:smelting", "category": "food", "ingredient": "minecraft:porkchop", "result": {"id": "minecraft:cooked_porkchop"},
            "experience": 0.35, "cookingtime": 200})");
        recipe("cut", R"({"type": "minecraft:stonecutting", "ingredient": "minecraft:stone", "result": {"id": "minecraft:stone_slab", "count": 2}})");
        recipe("widths", R"({"type": "minecraft:crafting_shaped", "pattern": ["##", "#"], "key": {"#": "minecraft:stick"}, "result": {"id": "minecraft:ladder"}})");
        recipe("unused", R"({"type": "minecraft:crafting_shaped", "pattern": ["#"], "key": {"#": "minecraft:stick", "X": "minecraft:coal"}, "result": {"id": "minecraft:torch"}})");
        recipe("unknown_item", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:no_such_item"], "result": {"id": "minecraft:stick"}})");
        recipe("unknown_tag", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["#minecraft:no_such_tag"], "result": {"id": "minecraft:stick"}})");
        recipe("unknown_type", R"({"type": "minecraft:crafting_magic", "result": {"id": "minecraft:stick"}})");
        recipe("too_many", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:stick"], "result": {"id": "minecraft:shears", "count": 2}})");
        recipe("air", R"({"type": "minecraft:crafting_shapeless", "ingredients": ["minecraft:air"], "result": {"id": "minecraft:stick"}})");
        recipe("trim", R"({"type": "minecraft:smithing_trim", "template": "minecraft:coast_armor_trim_smithing_template",
            "base": "#minecraft:test_logs", "addition": "minecraft:iron_ingot"})");
        const auto& bundle = **contracts::BundleRegistry::find("java-1.21.8");
        const auto d = server::load_server_data(bundle, {{}, {dir / "pack"}});
        require(d.has_value(), d ? "" : d.error());
        // Loot: five tables compile; the other names why not.
        require(d->loot.blocks == 6 && d->game->block_loot.size() == 5, std::format("{} tables, {} used", d->loot.blocks, d->game->block_loot.size()));
        require(d->loot.refused == std::map<std::string, std::size_t>{{"entry minecraft:dynamic", 1}}, "loot refusals by reason");
        // Recipes: every type checked; crafting recipes compiled; invalid ones skipped with a reason.
        std::vector<std::string> crafted;
        for (const auto& r : d->game->crafting) crafted.push_back(r.id);
        require(crafted == std::vector<std::string>{"test:box", "test:planks", "test:shears"}, "crafting recipes in id order");
        require(d->recipes.not_crafted == std::map<std::string, std::size_t>{{"result components", 1}}, "valid, not crafted");
        require((d->game->special == std::vector<gameplay::recipes::Special>{{gameplay::recipes::Special::Kind::repair_item, "test:repair"}}), "the special recipe");
        require(d->recipes.invalid == std::map<std::string, std::size_t>{{"air", 1}, {"key symbol not in the pattern", 1}, {"no trim pattern", 1},
                                                                         {"pattern rows of different widths", 1}, {"result count above the item's stack size", 1},
                                                                         {"unknown item", 1}, {"unknown item tag", 1}, {"unknown type minecraft:crafting_magic", 1}},
                "invalid recipes by reason");
        std::size_t valid = 0;
        for (const auto& [type, n] : d->recipes.valid) valid += n;
        require(valid == 7, "seven valid recipes");
        // The compiled shapeless recipe takes either log.
        std::array<contracts::ItemStack, 4> grid{};
        grid[3] = {gameplay::items::birch_log, 1};
        const auto made = gameplay::recipes::match(d->game->crafting, {2, 2, grid});
        require(made && made->result == contracts::ItemStack{gameplay::items::oak_planks, 4}, "planks from a tag");
        require(server::describe(*d).find("crafting recipes the rules match: 3") != std::string::npos, "the data report counts them");
        // 1.21.4 (data pack format 61): a transmute result is an item id.
        const auto old = dir / "old";
        std::filesystem::create_directories(old / "data/test/recipe");
        std::ofstream(old / "pack.mcmeta") << R"({"pack": {"pack_format": 61, "description": "test"}})";
        std::ofstream(old / "data/test/recipe/box.json")
            << R"({"type": "minecraft:crafting_transmute", "input": "minecraft:shulker_box", "material": "minecraft:red_dye", "result": "minecraft:red_shulker_box"})";
        std::ofstream(old / "data/test/recipe/box2.json")
            << R"({"type": "minecraft:crafting_transmute", "input": "minecraft:shulker_box", "material": "minecraft:red_dye", "result": {"id": "minecraft:red_shulker_box"}})";
        const auto d4 = server::load_server_data(**contracts::BundleRegistry::find("java-1.21.4"), {{}, {old}});
        require(d4 && d4->game->crafting.size() == 1 && d4->recipes.invalid.contains("result is not an item id"), "1.21.4 transmute results");
        std::filesystem::remove_all(dir);
    });

    // With the operator's jars (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4), the
    // rules run as a server with them does: every trace of the version
    // passes, those of loot tables among them, and their skeletons record
    // as the committed reference recordings. One case per version and part of
    // its directory (tests/support/fixture_parts.hpp); the first part of each
    // version also counts the directory.
    for (const auto& [variable_name, bundle_name, directory_name] : {std::tuple{"CIRCUIT_VANILLA_JAR_1_21_8", "java-1.21.8", "harness"},
                                                                    std::tuple{"CIRCUIT_VANILLA_JAR_1_21_4", "java-1.21.4", "harness-1.21.4"}})
      for (std::size_t part = 0; part < fixture_parts; ++part) {
        const char* const variable = variable_name;
        const char* const bundle_id = bundle_name;
        const char* const directory = directory_name;
        suite.run(std::format("every official trace passes with the official data packs ({}, part {} of {})", bundle_id, part + 1, fixture_parts),
                  [variable, bundle_id, directory, part] {
            namespace fs = std::filesystem;
            const fs::path fixtures = std::string(CIRCUIT_SOURCE_DIR) + "/tests/fixtures";
            const char* jar = official_jar(variable);
            if (!jar || !*jar) return;
            const auto& bundle = **contracts::BundleRegistry::find(bundle_id);
            const auto loaded = server::load_server_data(bundle, {jar, {}});
            require(loaded.has_value(), loaded ? "" : loaded.error());
            const auto game = loaded->game;
            const auto executor = oracle::oracle_executor([game](const contracts::VersionBundle&) { return game; });
            const auto text = [](const fs::path& p) {
                std::ifstream in(p, std::ios::binary);
                return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            };
            const auto of_loot = [](const evidence::Fixture& f) { return std::ranges::find(f.mechanisms, "loot") != f.mechanisms.end(); };
            const auto all = sorted_entries({fixtures / directory});
            for (const auto& entry : part_of(all, part, fixture_parts)) {
                const auto trace = evidence::parse_fixture(text(entry.path()));
                require(trace.has_value(), entry.path().string());
                const auto r = evidence::evaluate_fixture(*trace, executor);
                require(r.outcome == evidence::FixtureReport::Outcome::passed, std::format("{}: {}", entry.path().string(), r.detail));
                if (!of_loot(*trace)) continue;
                if (std::string_view(bundle_id) != "java-1.21.8") continue;
                const auto skeleton = evidence::parse_fixture(text(fixtures / "skeletons" / entry.path().filename()));
                const auto recorded = evidence::record_fixture(*skeleton, executor);
                require(recorded && evidence::write_fixture(*recorded) == text(fixtures / "reference" / entry.path().filename()),
                        std::format("{}: the reference recording no longer matches", entry.path().filename().string()));
            }
            if (part == 0) {
                const auto loot = std::ranges::count_if(all, [&](const fs::directory_entry& e) {
                    const auto trace = evidence::parse_fixture(text(e.path()));
                    return trace && of_loot(*trace);
                });
                require(all.size() >= 140 && loot >= 3, std::format("{}: {} traces, {} of loot", bundle_id, all.size(), loot));
            }
        });
      }

    suite.run("loot tables and recipes of the official packs agree with the official servers", [] {
        // The operator's jars, when given (CIRCUIT_VANILLA_JAR_1_21_8, _1_21_4).
        struct Expected {
            const char* variable;
            const char* bundle;
            std::string_view release;
            std::size_t loot_blocks, loot_used, recipes, crafted;
        };
        std::size_t checked = 0;
        // Tables of blocks with rules of their own are not counted: since
        // rules v11 also the redstone torch's, the 14 buttons', the 16
        // pressure plates', the target block's, the 54 doors', trapdoors'
        // and fence gates', the honey block's, the daylight detector's, the
        // note block's, the four rails', the ten coral fans', the
        // dispenser's and dropper's, the furnace's, smoker's and blast
        // furnace's, the crafter's, the brewing stand's, the lectern's and the
        // beacon's (whose table copies components, so it was never used); but wheat's,
        // which its rules use. The 49
        // wall torches, signs, hanging signs, banners and heads count with the
        // standing ones' tables (the banners' and the player head's are
        // refused: they copy components).
        for (const auto& e : {Expected{"CIRCUIT_VANILLA_JAR_1_21_8", "java-1.21.8", "1.21.8", 925, 887, 1407, 984},
                              Expected{"CIRCUIT_VANILLA_JAR_1_21_4", "java-1.21.4", "1.21.4", 917, 879, 1370, 948}}) {
            const char* jar = official_jar(e.variable);
            if (!jar || !*jar) continue;
            const auto& bundle = **contracts::BundleRegistry::find(e.bundle);
            auto d = server::load_server_data(bundle, {jar, {}});
            require(d.has_value(), d ? "" : d.error());
            std::size_t valid = 0;
            for (const auto& [type, n] : d->recipes.valid) valid += n;
            require(d->loot.blocks == e.loot_blocks && d->game->block_loot.size() == e.loot_used && valid == e.recipes &&
                        d->recipes.invalid.empty() && d->game->crafting.size() == e.crafted,
                    std::format("{}: {} tables, {} used; {} recipes, {} invalid, {} crafted", e.bundle, d->loot.blocks,
                                d->game->block_loot.size(), valid, d->recipes.invalid.size(), d->game->crafting.size()));
            // Block drops (tests/fixtures/loot): what the rules drop (drops.cpp)
            // for each block and tool the official servers were shown.
            const auto& mapping = *evidence::ContentMapping::for_game_version(e.release);
            const auto& codec = *protocol::ComponentCodec::for_protocol(bundle.schema.component_schema);
            std::ifstream drops_in(std::string(CIRCUIT_SOURCE_DIR) + "/tests/fixtures/loot/block-drops.tsv");
            std::size_t cases = 0;
            for (std::string line; std::getline(drops_in, line);) {
                if (line.empty() || line[0] == '#') continue;
                std::vector<std::string> f;
                for (std::size_t i = 0;;) {
                    const auto tab = line.find('\t', i);
                    f.push_back(line.substr(i, tab == std::string::npos ? std::string::npos : tab - i));
                    if (tab == std::string::npos) break;
                    i = tab + 1;
                }
                if (f.size() != 6 || f[0] != e.release) continue;
                const auto state = mapping.block(f[1]);
                require(state.has_value(), f[1]);
                contracts::ItemStack tool{};
                if (f[2] != "-") tool = {gameplay::items::by_name(f[2]).value(), 1};
                if (f[3] != "-") {
                    const auto eq = f[3].find('=');
                    const auto id = server::registry_index(d->registries, "minecraft:enchantment", f[3].substr(0, eq)).value();
                    Writer w;
                    w.varint(1).varint(id).varint(std::stoi(f[3].substr(eq + 1)));
                    if (bundle.schema.component_schema == 769) w.boolean(true); // 1.21.4: shown in the tooltip
                    tool.components = contracts::ComponentPatch::make(bundle.schema.component_schema,
                                                                      {{*codec.type_id("enchantments"), w.take()}}, {}, {});
                }
                std::string got;
                // A block with rules of its own (an ender chest) by the registry state it stands for.
                const auto registered = gameplay::physics::registry_state_of(*state);
                const auto block = registered ? gameplay::registry::block_of(*registered) : std::nullopt;
                const auto table = block ? d->game->block_loot.find(*block) : d->game->block_loot.end();
                if (gameplay::harvests(*state, tool, d->game.get()) && table != d->game->block_loot.end()) {
                    const gameplay::loot::BreakContext context{gameplay::registry::properties(*registered), tool,
                                                               [&](std::uint32_t en) { return d->game->enchantment_level(tool, en); },
                                                               nullptr, {}};
                    if (const auto made = gameplay::loot::drops(table->second, context))
                        for (const auto& s : *made) got += std::format("{}{}*{}", got.empty() ? "" : " ", gameplay::items::info(s.item)->name, s.count);
                }
                if (got.empty()) got = "-";
                // Rolls that draw random numbers have no sequence here (the
                // calibration world's are not known) and drop nothing.
                require(got == f[4] || (f[5] == "random" && got == "-"), std::format("{} {} {} {}: {} (official {})", f[0], f[1], f[2], f[3], got, f[4]));
                ++cases;
            }
            require(cases == 23, std::format("{} drop cases", cases));
            // Crafting in the own window (tests/fixtures/crafting) is replayed
            // by the player work, which owns window clicks.
            ++checked;
        }
        if (checked == 0) std::puts("  (no official server jar given: the official loot and recipe checks did not run)");
    });

    return suite.finish();
}
