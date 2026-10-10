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
            throw NotCrafted{"special recipe"};
        }
        if (type == "minecraft:smelting" || type == "minecraft:blasting" || type == "minecraft:smoking" || type == "minecraft:campfire_cooking") {
            category(j, cooking_categories);
            optional_string(j, "group");
            ingredient(j.find("ingredient"));
            result(j.find("result"));
            if (const auto* xp = j.find("experience"); xp && xp->kind() != Json::Kind::number) invalid("experience is not a number");
            if (const auto* time = j.find("cookingtime"); time && !whole(*time, 1, INT32_MAX)) invalid("cooking time");
            return std::nullopt;
        }
        if (type == "minecraft:stonecutting") {
            optional_string(j, "group");
            ingredient(j.find("ingredient"));
            result(j.find("result"));
            return std::nullopt;
        }
        if (type == "minecraft:smithing_transform" || type == "minecraft:smithing_trim") {
            // The template and the addition may be absent (minecraft.wiki).
            for (const auto* key : {"template", "addition"})
                if (const auto* i = j.find(key)) ingredient(i);
            ingredient(j.find("base"));
            if (type == "minecraft:smithing_transform") result(j.find("result"));
            if (type == "minecraft:smithing_trim" && bundle_.schema.data_pack_format >= format_1_21_5) {
                const auto* pattern = j.string("pattern");
                if (!pattern) invalid("no trim pattern");
                if (!registry_index(registries_, "minecraft:trim_pattern", with_namespace(*pattern))) invalid("unknown trim pattern");
            }
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

    const data::DataPacks& packs_;
    const contracts::VersionBundle& bundle_;
    const std::vector<RegistryEntries>& registries_;
    const protocol::ContentTables* tables_;
    const protocol::ComponentCodec* components_;
};
} // namespace

RecipeCompilation compile_recipes(const data::DataPacks& packs, const contracts::VersionBundle& bundle,
                                  const std::vector<RegistryEntries>& registries) {
    RecipeCompilation out;
    const auto* all = packs.resources("recipe");
    if (!all || !protocol::ContentTables::for_protocol(bundle.protocol)) return out;
    Compiler compiler(packs, bundle, registries);
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
    return out;
}

} // namespace circuit::server
