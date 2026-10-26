// SPDX-License-Identifier: AGPL-3.0-only
#include "server_data.hpp"
#include <circuit/server/data_report.hpp>
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/protocol/content.hpp>
#include <algorithm>
#include <array>
#include <format>
#include <set>

namespace circuit::server {
namespace {
// The fluid registry in id order (minecraft.wiki "Fluid", data values).
constexpr std::array<std::string_view, 5> fluids{"minecraft:empty", "minecraft:flowing_water", "minecraft:water",
                                                 "minecraft:flowing_lava", "minecraft:lava"};

const RegistryEntries* find_registry(const std::vector<RegistryEntries>& registries, std::string_view tag_registry) {
    for (const auto& r : registries)
        if (r.registry.substr(r.registry.find(':') + 1) == tag_registry) return &r;
    return nullptr;
}

// The registry id of `entry` in the registry whose tags live in `registry`.
std::optional<std::int32_t> number(std::string_view registry, std::string_view entry, const protocol::ContentTables& tables,
                                   const std::vector<RegistryEntries>& registries) {
    if (registry == "block") return tables.block_id(entry);
    if (registry == "item") return tables.item_id(entry);
    if (registry == "entity_type") return tables.entity_type(entry);
    if (registry == "fluid") {
        const auto it = std::ranges::find(fluids, entry);
        return it == fluids.end() ? std::nullopt : std::optional(static_cast<std::int32_t>(it - fluids.begin()));
    }
    if (const auto* r = find_registry(registries, registry))
        for (std::size_t i = 0; i < r->entries.size(); ++i)
            if (r->entries[i] == entry) return static_cast<std::int32_t>(i);
    return std::nullopt;
}

// Tool and harvest tier of the blocks without rules of their own, from the
// mineable and needs-tool block tags. A block that needs a tool of another
// kind (a sword, shears) keeps that from the public data; otherwise the
// tier is the higher of the tags' and the public data's.
std::unordered_map<std::uint32_t, gameplay::BlockTool> block_tools(const data::DataPacks& packs, std::string_view release) {
    namespace registry = gameplay::registry;
    std::unordered_map<std::uint32_t, gameplay::BlockTool> out;
    const auto tagged = [&](std::string_view tag) {
        std::set<std::string> names;
        if (const auto* t = packs.tag("block", tag)) names.insert(t->begin(), t->end());
        return names;
    };
    const std::array<std::pair<registry::Tool, std::set<std::string>>, 4> mineable{{
        {registry::Tool::pickaxe, tagged("minecraft:mineable/pickaxe")},
        {registry::Tool::axe, tagged("minecraft:mineable/axe")},
        {registry::Tool::shovel, tagged("minecraft:mineable/shovel")},
        {registry::Tool::hoe, tagged("minecraft:mineable/hoe")},
    }};
    const std::array<std::pair<std::uint8_t, std::set<std::string>>, 3> needs{{
        {4, tagged("minecraft:needs_diamond_tool")}, {3, tagged("minecraft:needs_iron_tool")}, {2, tagged("minecraft:needs_stone_tool")},
    }};
    for (std::uint32_t b = 0; b < registry::block_count(); ++b) {
        if (gameplay::modeled_block(b)) continue;
        const auto name = "minecraft:" + std::string(registry::game_name(b));
        if (registry::find_block(release, name) != b) continue;
        const auto info = registry::block(b);
        gameplay::BlockTool t;
        for (const auto& [tool, names] : mineable)
            if (names.contains(name)) {
                t.tool = tool;
                break;
            }
        for (const auto& [tier, names] : needs)
            if (names.contains(name)) {
                t.tier = tier;
                break;
            }
        t.tier = info.tier == registry::other_tool_tier ? info.tier : std::max(t.tier, info.tier);
        out.emplace(b, t);
    }
    return out;
}
} // namespace

const std::string& ServerData::fingerprint() const {
    static const std::string none{"none"};
    return packs ? packs->fingerprint() : none;
}

std::expected<ServerData, std::string> load_server_data(const contracts::VersionBundle& bundle, const data::PackOptions& options) {
    ServerData out;
    const auto* base = synced_registries(bundle.schema.registries);
    const auto* tables = protocol::ContentTables::for_protocol(bundle.protocol);
    if (!base || !tables) return std::unexpected(std::format("no registries for {}", bundle.id));
    out.registries = *base;
    if (!options.vanilla_jar.empty() || !options.packs.empty()) {
        // Tags may list entries of these registries; others are not loaded.
        auto listed = out.registries;
        listed.push_back(enchantment_registry());
        const data::RegistryLookup lookup = [&](std::string_view registry, std::string_view entry) -> std::optional<bool> {
            if (registry == "block" || registry == "item" || registry == "entity_type" || registry == "fluid" ||
                find_registry(listed, registry))
                return entry.empty() || number(registry, entry, *tables, listed).has_value();
            return std::nullopt;
        };
        auto packs = data::DataPacks::load(bundle, options, lookup);
        if (!packs) return std::unexpected(packs.error());
        // Enchantments are synchronized by name: the packs must define the
        // same ones the client's vanilla pack has.
        if (const auto* defined = packs->resources("enchantment"); defined && !defined->empty()) {
            std::set<std::string_view> names, expected(enchantment_registry().entries.begin(), enchantment_registry().entries.end());
            for (const auto& [id, json] : *defined) names.insert(id);
            if (names != expected) return std::unexpected("data packs that add or remove enchantments are not supported yet");
            out.registries.push_back(enchantment_registry());
        }
        for (const auto& [registry, tags] : packs->tags()) {
            const std::string full = "minecraft:" + registry;
            if (registry != "block" && registry != "item" && registry != "entity_type" && registry != "fluid" &&
                !find_registry(out.registries, registry))
                continue; // a registry the client does not receive from Circuit
            NumberedTags numbered{full, {}};
            for (const auto& [name, entries] : tags) {
                std::vector<std::int32_t> ids;
                for (const auto& e : entries)
                    if (const auto id = number(registry, e, *tables, out.registries)) ids.push_back(*id);
                std::ranges::sort(ids);
                numbered.tags.emplace_back(name, std::move(ids));
            }
            out.tags.push_back(std::move(numbered));
        }
        auto game = std::make_shared<gameplay::GameData>();
        game->fingerprint = packs->fingerprint();
        game->block_tools = block_tools(*packs, bundle.schema.content_release);
        out.loot = compile_block_loot(*packs, bundle, out.registries);
        game->block_loot = std::move(out.loot.tables);
        out.loot.tables.clear();
        out.recipes = compile_recipes(*packs, bundle, out.registries);
        game->crafting = std::move(out.recipes.crafting);
        out.recipes.crafting.clear();
        game->cooking = std::move(out.recipes.cooking);
        out.recipes.cooking.clear();
        game->stonecutting = std::move(out.recipes.stonecutting);
        out.recipes.stonecutting.clear();
        if (const auto* codec = protocol::ComponentCodec::for_protocol(bundle.schema.component_schema)) {
            game->enchantment_level = [codec](const contracts::ItemStack& stack, std::uint32_t enchantment) {
                return codec->enchantment_level(stack.components, enchantment);
            };
            game->enchanted = [codec, ctx = component_context(bundle, out.registries)](
                                  const std::vector<std::pair<std::string, std::int32_t>>& levels) -> std::optional<contracts::ComponentPatch> {
                std::vector<std::pair<std::uint32_t, std::int32_t>> ids;
                for (const auto& [name, level] : levels) {
                    const auto id = ctx.id_of("enchantment", name);
                    if (!id) return std::nullopt;
                    ids.emplace_back(*id, level);
                }
                auto patch = codec->enchanted(ids, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
        }
        if (const auto* keeps = packs->tag("enchantment", "minecraft:prevents_ice_melting")) {
            const auto ctx = component_context(bundle, out.registries);
            for (const auto& name : *keeps)
                if (const auto id = ctx.id_of("enchantment", name)) game->keeps_ice.push_back(*id);
        }
        out.game = std::move(game);
        out.packs = std::move(*packs);
    } else {
        // Without data packs: the hand-made tags the registries need.
        for (const auto& r : synced_tags(bundle.schema.registries)) {
            NumberedTags numbered{std::string(r.registry), {}};
            const auto registry = r.registry.substr(r.registry.find(':') + 1);
            for (const auto& [name, entries] : r.tags) {
                std::vector<std::int32_t> ids;
                for (const auto e : entries) ids.push_back(number(registry, e, *tables, out.registries).value());
                numbered.tags.emplace_back(std::string(name), std::move(ids));
            }
            out.tags.push_back(std::move(numbered));
        }
    }
    out.components = component_context(bundle, out.registries);
    return out;
}

std::string describe(const ServerData& data) {
    std::string out;
    if (!data.packs) return "no data packs: the public tables only\n";
    for (const auto& p : data.packs->packs()) out += std::format("pack {}: {} files from {} (SHA-256 {})\n", p.name, p.files, p.origin, p.sha256);
    out += std::format("fingerprint {}\n", data.fingerprint());
    for (const auto& [registry, tags] : data.packs->tags()) out += std::format("tags {}: {}\n", registry, tags.size());
    for (const auto* kind : {"enchantment", "loot_table", "recipe"})
        if (const auto* r = data.packs->resources(kind)) out += std::format("resources {}: {}\n", kind, r->size());
    out += std::format("registries sent: {}; tag registries sent: {}\n", data.registries.size(), data.tags.size());
    // Where the tags disagree with the public registry data about tools.
    constexpr std::string_view tool_names[]{"none", "pickaxe", "axe", "shovel", "hoe"};
    std::size_t tools = 0, tiers = 0;
    std::vector<std::string> differences;
    for (const auto& [block, t] : data.game->block_tools) {
        const auto info = gameplay::registry::block(block);
        if (info.tool == t.tool && info.tier == t.tier) continue;
        tools += info.tool != t.tool;
        tiers += info.tier != t.tier;
        differences.push_back(std::format("{} ({} {} -> {} {})", info.name, tool_names[static_cast<int>(info.tool)], info.tier,
                                          tool_names[static_cast<int>(t.tool)], t.tier));
    }
    std::ranges::sort(differences);
    out += std::format("blocks without rules: {}; tool differs from the public data for {}, tier for {}\n",
                       data.game->block_tools.size(), tools, tiers);
    for (const auto& d : differences) out += "  " + d + "\n";
    // Loot tables and recipes the rules use, and why not the others.
    out += std::format("block loot tables of blocks without rules: {}; used: {}\n", data.loot.blocks, data.game->block_loot.size());
    for (const auto& [reason, n] : data.loot.refused) out += std::format("  not used ({}): {}\n", reason, n);
    std::size_t valid = 0;
    for (const auto& [type, n] : data.recipes.valid) valid += n;
    out += std::format("recipes: {} valid; crafting recipes the rules match: {}\n", valid, data.game->crafting.size());
    for (const auto& [type, n] : data.recipes.valid) out += std::format("  {}: {}\n", type, n);
    for (const auto& [reason, n] : data.recipes.not_crafted) out += std::format("  not crafted ({}): {}\n", reason, n);
    for (const auto& [reason, n] : data.recipes.invalid) out += std::format("  invalid ({}): {}\n", reason, n);
    for (const auto& id : data.recipes.invalid_ids) out += "  invalid: " + id + "\n";
    for (const auto& n : data.packs->notes()) out += "note: " + n + "\n";
    return out;
}

std::expected<std::shared_ptr<const gameplay::GameData>, std::string> load_game_data(const contracts::VersionBundle& bundle,
                                                                                      const data::PackOptions& options) {
    auto loaded = load_server_data(bundle, options);
    if (!loaded) return std::unexpected(loaded.error());
    return loaded->game;
}

std::expected<std::string, std::string> data_report(const contracts::VersionBundle& bundle, const data::PackOptions& options) {
    auto data = load_server_data(bundle, options);
    if (!data) return std::unexpected(data.error());
    return describe(*data);
}

} // namespace circuit::server
