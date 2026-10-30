// SPDX-License-Identifier: AGPL-3.0-only
#include "recipe_compiler.hpp"
#include <circuit/auth/json.hpp>
#include <circuit/gameplay/items.hpp>
#include <circuit/protocol/components.hpp>
#include <circuit/protocol/content.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <set>
#include <utility>

namespace circuit::server {
namespace {
using auth::Json;
namespace recipes = gameplay::recipes;

// A recipe the target version would refuse; the reason names the part.
struct Invalid {
    std::string reason;
};
[[noreturn]] void invalid(std::string reason) { throw Invalid{std::move(reason)}; }

// A valid recipe the rules do not craft yet.
struct NotCrafted {
    std::string reason;
};

std::string with_namespace(std::string_view id) {
    return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}

// The recipes whose behavior is code of the target version: they name only
// their type (minecraft.wiki "Recipe").
constexpr std::array<std::string_view, 12> special_types{
    "minecraft:crafting_special_armordye",     "minecraft:crafting_special_bannerduplicate", "minecraft:crafting_special_bookcloning",
    "minecraft:crafting_special_firework_rocket", "minecraft:crafting_special_firework_star",
    "minecraft:crafting_special_firework_star_fade", "minecraft:crafting_special_mapcloning",
    "minecraft:crafting_special_mapextending", "minecraft:crafting_special_repairitem",
    "minecraft:crafting_special_shielddecoration", "minecraft:crafting_special_tippedarrow", "minecraft:crafting_decorated_pot"};
constexpr std::array<std::string_view, 4> crafting_categories{"building", "redstone", "equipment", "misc"};
constexpr std::array<std::string_view, 3> cooking_categories{"food", "blocks", "misc"};
// Data pack format of 1.21.5, whose transmute results and trims changed.
constexpr std::int32_t format_1_21_5 = 71;

class Compiler {
public:
    Compiler(const data::DataPacks& packs, const contracts::VersionBundle& bundle, const std::vector<RegistryEntries>& registries)
        : packs_(packs), bundle_(bundle), registries_(registries), tables_(protocol::ContentTables::for_protocol(bundle.protocol)),
          components_(protocol::ComponentCodec::for_protocol(bundle.schema.component_schema)) {}

    // Checks one recipe; returns the crafting recipe the rules match, if any.
    std::optional<recipes::Recipe> recipe(const std::string& id, const Json& j, std::string& type) {
        if (j.kind() != Json::Kind::object) invalid("not an object");
        const auto* t = j.string("type");
        if (!t) invalid("no type");
        type = with_namespace(*t);
        recipes::Recipe r;
        r.id = id;
        if (type == "minecraft:crafting_shaped") {
            category(j, crafting_categories);
            optional_string(j, "group");
            shaped(j, r);
            r.result = crafted(j.find("result"));
            return r;
        }
        if (type == "minecraft:crafting_shapeless") {
            category(j, crafting_categories);
            optional_string(j, "group");
            r.kind = recipes::Recipe::Kind::shapeless;
            const auto* list = j.find("ingredients");
            if (!list || list->kind() != Json::Kind::array) invalid("no ingredients");
            if (list->items().empty() || list->items().size() > 9) invalid("1 to 9 ingredients");
            for (const auto& i : list->items()) r.ingredients.push_back(ingredient(&i));
            r.result = crafted(j.find("result"));
            return r;
        }
        if (type == "minecraft:crafting_transmute") {
            category(j, crafting_categories);
            optional_string(j, "group");
            r.kind = recipes::Recipe::Kind::transmute;
            r.ingredients.push_back(ingredient(j.find("input")));
            r.ingredients.push_back(ingredient(j.find("material")));
            // An item id before 1.21.5, then a stack template.
            const auto* res = j.find("result");
            if (bundle_.schema.data_pack_format >= format_1_21_5) {
                r.result = crafted(res);
            } else {
                if (!res || res->kind() != Json::Kind::string) invalid("result is not an item id");
                r.result = {item(res->as_string()), 1};
            }
            return r;
        }
        if (std::ranges::find(special_types, type) != special_types.end()) {
            category(j, crafting_categories);
            using Kind = gameplay::recipes::Special::Kind;
            static constexpr std::array<std::pair<std::string_view, Kind>, 7> made{{
                {"minecraft:crafting_special_repairitem", Kind::repair_item}, {"minecraft:crafting_special_armordye", Kind::armor_dye},
                {"minecraft:crafting_special_bannerduplicate", Kind::banner_duplicate},
                {"minecraft:crafting_special_shielddecoration", Kind::shield_decoration},
                {"minecraft:crafting_special_firework_rocket", Kind::firework_rocket},
                {"minecraft:crafting_special_firework_star", Kind::firework_star},
                {"minecraft:crafting_special_firework_star_fade", Kind::firework_star_fade}}};
            const auto it = std::ranges::find(made, type, &std::pair<std::string_view, Kind>::first);
            if (it == made.end()) throw NotCrafted{"special recipe"};
            if (special_) special_->push_back({it->second, std::string(id)});
            return std::nullopt;
        }
        if (type == "minecraft:smelting" || type == "minecraft:blasting" || type == "minecraft:smoking" || type == "minecraft:campfire_cooking") {
            category(j, cooking_categories);
            optional_string(j, "group");
            gameplay::recipes::Cooking c;
            c.id = std::string(id);
            c.kind = type == "minecraft:smelting" ? gameplay::recipes::Cooking::Kind::smelting
                   : type == "minecraft:blasting" ? gameplay::recipes::Cooking::Kind::blasting
                   : type == "minecraft:smoking" ? gameplay::recipes::Cooking::Kind::smoking : gameplay::recipes::Cooking::Kind::campfire;
            c.ingredient = ingredient(j.find("ingredient"));
            c.result = result(j.find("result")).first;
            if (const auto* xp = j.find("experience"); xp && xp->kind() != Json::Kind::number) invalid("experience is not a number");
            else if (xp) c.experience = static_cast<float>(xp->as_number());
            if (const auto* time = j.find("cookingtime"); time && !whole(*time, 1, INT32_MAX)) invalid("cooking time");
            else c.time = time ? static_cast<std::int32_t>(time->as_number()) : c.kind == gameplay::recipes::Cooking::Kind::smelting ? 200 : 100;
            if (cooking_) cooking_->push_back(std::move(c));
            return std::nullopt;
        }
        if (type == "minecraft:stonecutting") {
            optional_string(j, "group");
            gameplay::recipes::Stonecutting c;
            c.id = std::string(id);
            c.ingredient = ingredient(j.find("ingredient"));
            c.result = result(j.find("result")).first;
            if (stonecutting_) stonecutting_->push_back(std::move(c));
            return std::nullopt;
        }
        if (type == "minecraft:smithing_transform" || type == "minecraft:smithing_trim") {
            // The template and the addition may be absent (minecraft.wiki).
            const auto* template_ = j.find("template");
            const auto* addition = j.find("addition");
            const auto taken = template_ ? ingredient(template_) : recipes::Ingredient{};
            const auto added = addition ? ingredient(addition) : recipes::Ingredient{};
            const auto base = ingredient(j.find("base"));
            gameplay::recipes::Smithing s;
            s.id = std::string(id);
            if (template_) s.template_item = taken;
            if (addition) s.addition = added;
            s.base = base;
            // A transform's result with components of its own is not made.
            bool made = true;
            if (type == "minecraft:smithing_transform") {
                const auto [stack, has_components] = result(j.find("result"));
                s.result = stack;
                made = !has_components;
            } else {
                s.kind = gameplay::recipes::Smithing::Kind::trim;
            }
            if (type == "minecraft:smithing_trim" && bundle_.schema.data_pack_format >= format_1_21_5) {
                const auto* pattern = j.string("pattern");
                if (!pattern) invalid("no trim pattern");
                if (!registry_index(registries_, "minecraft:trim_pattern", with_namespace(*pattern))) invalid("unknown trim pattern");
                s.pattern = with_namespace(*pattern);
            }
            // A valid recipe's items join the property sets.
            const auto add = [](std::vector<std::uint16_t>* to, const recipes::Ingredient& i) {
                if (to) to->insert(to->end(), i.items.begin(), i.items.end());
            };
            add(smithing_template_, taken);
            add(smithing_addition_, added);
            add(smithing_base_, base);
            if (smithing_ && made) smithing_->push_back(std::move(s));
            return std::nullopt;
        }
        invalid(std::format("unknown type {}", type));
    }

private:
    static bool whole(const Json& j, double min, double max) {
        return j.kind() == Json::Kind::number && j.as_number() == std::floor(j.as_number()) && j.as_number() >= min && j.as_number() <= max;
    }
    static void optional_string(const Json& j, std::string_view key) {
        if (const auto* v = j.find(key); v && v->kind() != Json::Kind::string) invalid(std::format("{} is not a string", key));
    }
    template <std::size_t N>
    static void category(const Json& j, const std::array<std::string_view, N>& allowed) {
        const auto* v = j.find("category");
        if (!v) return; // misc
        if (v->kind() != Json::Kind::string || std::ranges::find(allowed, v->as_string()) == allowed.end()) invalid("unknown category");
    }

    // An item of the version's registry, other than air.
    std::uint16_t item(std::string_view name) const {
        const auto full = with_namespace(name);
        if (full == "minecraft:air") invalid("air");
        const auto id = tables_->item_id(full) ? gameplay::items::by_name(full) : std::nullopt;
        if (!id) invalid("unknown item");
        return *id;
    }

    // An item id, an item tag ("#minecraft:planks") or a list of item ids.
    recipes::Ingredient ingredient(const Json* j) const {
        if (!j) invalid("missing ingredient");
        recipes::Ingredient out;
        if (j->kind() == Json::Kind::string) {
            const auto& v = j->as_string();
            if (v.starts_with('#')) {
                const auto* tag = packs_.tag("item", v.substr(1));
                if (!tag) invalid("unknown item tag");
                for (const auto& n : *tag) out.items.push_back(item(n));
            } else {
                out.items.push_back(item(v));
            }
        } else if (j->kind() == Json::Kind::array) {
            if (j->items().empty()) invalid("empty ingredient list");
            for (const auto& v : j->items()) {
                if (v.kind() != Json::Kind::string || v.as_string().starts_with('#')) invalid("ingredient list entry");
                out.items.push_back(item(v.as_string()));
            }
        } else {
            invalid("ingredient form");
        }
        std::ranges::sort(out.items);
        const auto [first, last] = std::ranges::unique(out.items);
        out.items.erase(first, last);
        return out;
    }

    // A stack template: {"id": ..., "count": 1-99, "components": {...}}.
    // Returns the stack without components, and whether it has some.
    std::pair<contracts::ItemStack, bool> result(const Json* j) const {
        if (!j || j->kind() != Json::Kind::object) invalid("no result");
        const auto* id = j->string("id");
        if (!id) invalid("result without an id");
        contracts::ItemStack out{item(*id), 1};
        if (const auto* count = j->find("count")) {
            if (!whole(*count, 1, 99)) invalid("result count");
            out.count = static_cast<std::uint8_t>(count->as_number());
        }
        if (out.count > gameplay::items::max_stack(out.item)) invalid("result count above the item's stack size");
        bool with_components = false;
        if (const auto* components = j->find("components")) {
            if (components->kind() != Json::Kind::object) invalid("result components");
            for (const auto& [name, value] : components->members()) {
                const std::string_view type = name.starts_with('!') ? std::string_view(name).substr(1) : std::string_view(name);
                if (!components_ || !components_->type_id(type)) invalid("unknown result component");
            }
            with_components = !components->members().empty();
        }
        return {out, with_components};
    }
    // The result of a crafting recipe the rules match. Component values in
    // results are not read yet: such a recipe is valid but not crafted.
    contracts::ItemStack crafted(const Json* j) const {
        const auto [stack, with_components] = result(j);
        if (with_components) throw NotCrafted{"result components"};
        return stack;
    }

    void shaped(const Json& j, recipes::Recipe& r) const {
        const auto* pattern = j.find("pattern");
        const auto* key = j.find("key");
        if (!pattern || pattern->kind() != Json::Kind::array || !key || key->kind() != Json::Kind::object) invalid("no pattern or key");
        const auto& rows = pattern->items();
        if (rows.empty() || rows.size() > 3) invalid("1 to 3 pattern rows");
        std::vector<std::string> grid;
        for (const auto& row : rows) {
            if (row.kind() != Json::Kind::string || row.as_string().empty() || row.as_string().size() > 3) invalid("pattern row");
            if (!grid.empty() && row.as_string().size() != grid.front().size()) invalid("pattern rows of different widths");
            grid.push_back(row.as_string());
        }
        std::map<char, recipes::Ingredient> symbols;
        for (const auto& [symbol, value] : key->members()) {
            if (symbol.size() != 1 || symbol == " ") invalid("key symbol");
            symbols.emplace(symbol[0], ingredient(&value));
        }
        std::set<char> used;
        for (const auto& row : grid)
            for (const char c : row) {
                if (c == ' ') continue;
                if (!symbols.contains(c)) invalid("pattern symbol without a key");
                used.insert(c);
            }
        if (used.size() != symbols.size()) invalid("key symbol not in the pattern");
        // Without empty outer rows and columns.
        std::size_t x0 = 3, x1 = 0, y0 = 3, y1 = 0;
        bool any = false;
        for (std::size_t y = 0; y < grid.size(); ++y)
            for (std::size_t x = 0; x < grid[y].size(); ++x)
                if (grid[y][x] != ' ') {
                    any = true;
                    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                }
        if (!any) invalid("empty pattern");
        r.kind = recipes::Recipe::Kind::shaped;
        r.width = static_cast<std::uint8_t>(x1 - x0 + 1);
        r.height = static_cast<std::uint8_t>(y1 - y0 + 1);
        for (std::size_t y = y0; y <= y1; ++y)
            for (std::size_t x = x0; x <= x1; ++x)
                r.ingredients.push_back(grid[y][x] == ' ' ? recipes::Ingredient{} : symbols.at(grid[y][x]));
        if (const auto* n = j.find("show_notification"); n && n->kind() != Json::Kind::boolean) invalid("show_notification");
    }

public:
    std::vector<gameplay::recipes::Cooking>* cooking_ = nullptr;
    std::vector<gameplay::recipes::Stonecutting>* stonecutting_ = nullptr;
    std::vector<gameplay::recipes::Smithing>* smithing_ = nullptr;
    std::vector<gameplay::recipes::Special>* special_ = nullptr;
    std::vector<std::uint16_t>* smithing_template_ = nullptr;
    std::vector<std::uint16_t>* smithing_base_ = nullptr;
    std::vector<std::uint16_t>* smithing_addition_ = nullptr;

private:
    const data::DataPacks& packs_;
    const contracts::VersionBundle& bundle_;
    const std::vector<RegistryEntries>& registries_;
    const protocol::ContentTables* tables_;
    const protocol::ComponentCodec* components_;
};
// An item of the version's registry by its id, if it has one.
std::optional<std::uint16_t> known_item(const contracts::VersionBundle& bundle, std::string_view name) {
    const auto* tables = protocol::ContentTables::for_protocol(bundle.protocol);
    const auto full = with_namespace(name);
    return tables && tables->item_id(full) ? gameplay::items::by_name(full) : std::nullopt;
}

// Trim materials by the item that gives them: before 1.21.5 each material's
// `ingredient`; since, the items' provides_trim_material component, which
// is no data pack's, so the vanilla materials' items (minecraft.wiki
// "Smithing template") for those the registry has.
std::unordered_map<std::uint16_t, std::string> trim_materials(const data::DataPacks& packs, const contracts::VersionBundle& bundle,
                                                              const std::vector<RegistryEntries>& registries) {
    std::unordered_map<std::uint16_t, std::string> out;
    if (bundle.schema.data_pack_format < format_1_21_5) {
        if (const auto* all = packs.resources("trim_material"))
            for (const auto& [id, text] : *all) {
                const auto json = Json::parse(text);
                const auto* ingredient = json && json->kind() == Json::Kind::object ? json->string("ingredient") : nullptr;
                if (const auto item = ingredient ? known_item(bundle, *ingredient) : std::nullopt) out.emplace(*item, id);
            }
        return out;
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 11> vanilla{{
        {"amethyst_shard", "amethyst"}, {"copper_ingot", "copper"}, {"diamond", "diamond"}, {"emerald", "emerald"},
        {"gold_ingot", "gold"}, {"iron_ingot", "iron"}, {"lapis_lazuli", "lapis"}, {"netherite_ingot", "netherite"},
        {"quartz", "quartz"}, {"redstone", "redstone"}, {"resin_brick", "resin"}}};
    for (const auto& [item, material] : vanilla) {
        const auto id = std::string("minecraft:") + std::string(material);
        if (const auto i = known_item(bundle, item); i && registry_index(registries, "minecraft:trim_material", id)) out.emplace(*i, id);
    }
    return out;
}
// Before 1.21.5, trim patterns by their `template_item`.
std::unordered_map<std::uint16_t, std::string> trim_patterns(const data::DataPacks& packs, const contracts::VersionBundle& bundle) {
    std::unordered_map<std::uint16_t, std::string> out;
    if (bundle.schema.data_pack_format >= format_1_21_5) return out;
    if (const auto* all = packs.resources("trim_pattern"))
        for (const auto& [id, text] : *all) {
            const auto json = Json::parse(text);
            const auto* item = json && json->kind() == Json::Kind::object ? json->string("template_item") : nullptr;
            if (const auto i = item ? known_item(bundle, *item) : std::nullopt) out.emplace(*i, id);
        }
    return out;
}
} // namespace

RecipeCompilation compile_recipes(const data::DataPacks& packs, const contracts::VersionBundle& bundle,
                                  const std::vector<RegistryEntries>& registries) {
    RecipeCompilation out;
    const auto* all = packs.resources("recipe");
    if (!all || !protocol::ContentTables::for_protocol(bundle.protocol)) return out;
    Compiler compiler(packs, bundle, registries);
    compiler.cooking_ = &out.cooking;
    compiler.stonecutting_ = &out.stonecutting;
    compiler.smithing_ = &out.smithing;
    compiler.special_ = &out.special;
    compiler.smithing_template_ = &out.smithing_template;
    compiler.smithing_base_ = &out.smithing_base;
    compiler.smithing_addition_ = &out.smithing_addition;
    for (const auto& [id, text] : *all) {
        std::string type;
        const auto fail = [&](const std::string& reason) {
            ++out.invalid[reason];
            if (out.invalid_ids.size() < 20) out.invalid_ids.push_back(std::format("{}: {}", id, reason));
        };
        const auto json = Json::parse(text);
        if (!json) {
            fail("invalid JSON");
            continue;
        }
        try {
            auto r = compiler.recipe(id, *json, type);
            ++out.valid[type];
            if (r) out.crafting.push_back(std::move(*r));
        } catch (const Invalid& e) {
            fail(e.reason);
        } catch (const NotCrafted& e) {
            ++out.valid[type];
            ++out.not_crafted[e.reason];
        }
    }
    // Both official servers keep recipes by the id's path, then its
    // namespace (recipes::id_before): every list is in that order, so that
    // grids and furnaces take the first recipe in it and the stonecutter
    // lists its recipes in it.
    const auto by_id = [](const auto& a, const auto& b) { return recipes::id_before(a.id, b.id); };
    std::ranges::stable_sort(out.crafting, by_id);
    std::ranges::stable_sort(out.special, by_id);
    std::ranges::stable_sort(out.cooking, by_id);
    std::ranges::stable_sort(out.stonecutting, by_id);
    std::ranges::stable_sort(out.smithing, by_id);
    for (auto* items : {&out.smithing_template, &out.smithing_base, &out.smithing_addition}) {
        std::ranges::sort(*items);
        const auto [first, last] = std::ranges::unique(*items);
        items->erase(first, last);
    }
    out.trim_materials = trim_materials(packs, bundle, registries);
    out.trim_patterns = trim_patterns(packs, bundle);
    return out;
}

} // namespace circuit::server
