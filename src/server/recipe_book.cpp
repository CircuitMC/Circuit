// SPDX-License-Identifier: AGPL-3.0-only
// The recipe book (client/recipes, both official servers; minecraft.wiki
// "Recipe book", "Recipe" and "Advancement/JSON format").
#include "recipe_book.hpp"
#include <circuit/auth/json.hpp>
#include <circuit/gameplay/effects.hpp>
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/components.hpp>
#include <circuit/protocol/content.hpp>
#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace circuit::server {
namespace {
using auth::Json;

std::string with_namespace(std::string_view id) {
    return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}

// The special recipes, whose behavior is code: none has a display but map
// extending, which no book shows (both official servers number it).
constexpr std::array<std::string_view, 12> special_types{
    "minecraft:crafting_special_armordye",     "minecraft:crafting_special_bannerduplicate", "minecraft:crafting_special_bookcloning",
    "minecraft:crafting_special_firework_rocket", "minecraft:crafting_special_firework_star",
    "minecraft:crafting_special_firework_star_fade", "minecraft:crafting_special_mapcloning",
    "minecraft:crafting_special_mapextending", "minecraft:crafting_special_repairitem",
    "minecraft:crafting_special_shielddecoration", "minecraft:crafting_special_tippedarrow", "minecraft:crafting_decorated_pot"};

// recipe_book_category ids (both protocols).
enum Category : std::int32_t {
    crafting_building_blocks, crafting_redstone, crafting_equipment, crafting_misc, furnace_food, furnace_blocks, furnace_misc,
    blast_furnace_blocks, blast_furnace_misc, smoker_food, stonecutter, smithing, campfire
};

struct Bad {};

class Reader {
public:
    Reader(const data::DataPacks& packs, const contracts::VersionBundle& bundle)
        : packs_(packs), tables_(protocol::ContentTables::for_protocol(bundle.protocol)),
          codec_(protocol::ComponentCodec::for_protocol(bundle.schema.component_schema)), protocol_(bundle.schema.component_schema) {}

    std::uint16_t item(std::string_view name) const {
        const auto full = with_namespace(name);
        const auto id = tables_ && tables_->item_id(full) ? gameplay::items::by_name(full) : std::nullopt;
        if (!id) throw Bad{};
        return *id;
    }
    // The items of an item id, a tag ("#minecraft:planks") or a list of ids.
    std::vector<std::uint16_t> items(const Json& j) const {
        std::vector<std::uint16_t> out;
        if (j.kind() == Json::Kind::string && j.as_string().starts_with('#')) {
            const auto* tag = packs_.tag("item", j.as_string().substr(1));
            if (!tag) throw Bad{};
            for (const auto& n : *tag) out.push_back(item(n));
        } else if (j.kind() == Json::Kind::string) {
            out.push_back(item(j.as_string()));
        } else if (j.kind() == Json::Kind::array) {
            for (const auto& v : j.items())
                if (v.kind() == Json::Kind::string) out.push_back(item(v.as_string()));
                else throw Bad{};
        } else {
            throw Bad{};
        }
        return out;
    }
    // An ingredient as a slot shows it: a tag, else its items one by one.
    SlotDisplay ingredient(const Json* j) const {
        if (!j) throw Bad{};
        if (j->kind() == Json::Kind::string && j->as_string().starts_with('#'))
            return {.kind = SlotDisplay::Kind::tag, .tag = with_namespace(j->as_string().substr(1))};
        // An item that leaves something in the grid shows it.
        SlotDisplay out{.kind = SlotDisplay::Kind::composite};
        for (const auto i : items(*j)) {
            SlotDisplay one{.kind = SlotDisplay::Kind::item, .item = i};
            if (const auto left = gameplay::recipes::remainder(i); !left.empty())
                one = {.kind = SlotDisplay::Kind::with_remainder, .parts = {one, {.kind = SlotDisplay::Kind::item_stack, .stack = left}}};
            out.parts.push_back(std::move(one));
        }
        return out;
    }
    ItemSet requirement(const Json* j) const {
        if (j->kind() == Json::Kind::string && j->as_string().starts_with('#')) return {with_namespace(j->as_string().substr(1)), {}};
        return {{}, items(*j)};
    }
    // A result: a stack template ({"id", "count"}), or before 1.21.5 a
    // transmute's item id alone.
    SlotDisplay result(const Json* j) const {
        if (!j) throw Bad{};
        if (j->kind() == Json::Kind::string) return {.kind = SlotDisplay::Kind::item, .item = item(j->as_string())};
        const auto* id = j->string("id");
        if (!id) throw Bad{};
        SlotDisplay out{.kind = SlotDisplay::Kind::item_stack, .stack = {item(*id), 1}};
        if (const auto* count = j->find("count"); count && count->kind() == Json::Kind::number)
            out.stack.count = static_cast<std::uint8_t>(count->as_number());
        // Of result components, suspicious stews' effects (the only ones the
        // official packs give) are shown; others are left out.
        if (const auto* components = j->find("components"); components && components->kind() == Json::Kind::object && codec_)
            for (const auto& [name, value] : components->members()) {
                const auto type = codec_->type_id(with_namespace(name));
                if (with_namespace(name) != "minecraft:suspicious_stew_effects" || !type || value.kind() != Json::Kind::array) continue;
                protocol::Writer w;
                w.varint(static_cast<std::int32_t>(value.items().size()));
                for (const auto& e : value.items()) {
                    const auto* id = e.string("id");
                    // Suspicious stews' results name effects by their registry id.
                    const auto effect = id ? gameplay::effects::id(with_namespace(*id)) : std::nullopt;
                    if (!effect) throw Bad{};
                    const auto* duration = e.find("duration");
                    w.varint(*effect)
                        .varint(duration && duration->kind() == Json::Kind::number ? static_cast<std::int32_t>(duration->as_number()) : 160);
                }
                const auto bytes = w.take();
                out.stack.components = contracts::ComponentPatch::make(protocol_, {{*type, {bytes.begin(), bytes.end()}}}, {}, {});
            }
        return out;
    }

private:
    const data::DataPacks& packs_;
    const protocol::ContentTables* tables_;
    const protocol::ComponentCodec* codec_;
    std::int32_t protocol_;
};

SlotDisplay station(std::string_view block) { return {.kind = SlotDisplay::Kind::item, .item = *gameplay::items::by_name(with_namespace(block))}; }

std::string category_of(const Json& j) {
    const auto* c = j.string("category");
    return c ? *c : "misc";
}

// A recipe's display, category and requirements; nullopt for a recipe
// without a display.
std::optional<BookRecipe> read_recipe(const Reader& r, const std::string& id, const Json& j, const std::vector<RegistryEntries>& registries) {
    const auto* t = j.string("type");
    if (!t) throw Bad{};
    const auto type = with_namespace(*t);
    BookRecipe out;
    out.id = id;
    if (const auto* n = j.find("show_notification"); n && n->kind() == Json::Kind::boolean) out.notification = n->as_bool();
    auto& d = out.display;
    const auto crafting = [&] {
        const auto c = category_of(j);
        out.category = c == "building" ? crafting_building_blocks : c == "redstone" ? crafting_redstone : c == "equipment" ? crafting_equipment : crafting_misc;
        d.station = station("crafting_table");
    };
    const auto require = [&](const Json* ingredient) { out.requirements.push_back(r.requirement(ingredient)); };
    if (type == "minecraft:crafting_special_mapextending") {
        out.in_book = false;
        return out;
    }
    if (std::ranges::find(special_types, type) != special_types.end()) return std::nullopt;
    if (type == "minecraft:crafting_shaped") {
        crafting();
        d.kind = RecipeDisplay::Kind::shaped;
        const auto* pattern = j.find("pattern");
        const auto* key = j.find("key");
        if (!pattern || !key || pattern->kind() != Json::Kind::array) throw Bad{};
        std::vector<std::string> grid;
        for (const auto& row : pattern->items()) grid.push_back(row.as_string());
        std::size_t x0 = 3, x1 = 0, y0 = 3, y1 = 0;
        for (std::size_t y = 0; y < grid.size(); ++y)
            for (std::size_t x = 0; x < grid[y].size(); ++x)
                if (grid[y][x] != ' ') x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        if (x0 > x1) throw Bad{};
        d.width = static_cast<std::uint8_t>(x1 - x0 + 1);
        d.height = static_cast<std::uint8_t>(y1 - y0 + 1);
        for (std::size_t y = y0; y <= y1; ++y)
            for (std::size_t x = x0; x <= x1; ++x) {
                if (grid[y][x] == ' ') {
                    d.slots.push_back({});
                    continue;
                }
                const auto* ingredient = key->find(std::string(1, grid[y][x]));
                d.slots.push_back(r.ingredient(ingredient));
                require(ingredient);
            }
        d.result = r.result(j.find("result"));
    } else if (type == "minecraft:crafting_shapeless") {
        crafting();
        d.kind = RecipeDisplay::Kind::shapeless;
        const auto* ingredients = j.find("ingredients");
        if (!ingredients || ingredients->kind() != Json::Kind::array) throw Bad{};
        for (const auto& i : ingredients->items()) {
            d.slots.push_back(r.ingredient(&i));
            require(&i);
        }
        d.result = r.result(j.find("result"));
    } else if (type == "minecraft:crafting_transmute") {
        crafting();
        d.kind = RecipeDisplay::Kind::shapeless;
        for (const auto* part : {j.find("input"), j.find("material")}) {
            d.slots.push_back(r.ingredient(part));
            require(part);
        }
        d.result = r.result(j.find("result"));
    } else if (type == "minecraft:smelting" || type == "minecraft:blasting" || type == "minecraft:smoking" || type == "minecraft:campfire_cooking") {
        const auto c = category_of(j);
        d.kind = RecipeDisplay::Kind::furnace;
        if (type == "minecraft:smelting") {
            out.category = c == "food" ? furnace_food : c == "blocks" ? furnace_blocks : furnace_misc;
            d.station = station("furnace");
            d.duration = 200;
        } else if (type == "minecraft:blasting") {
            out.category = c == "blocks" ? blast_furnace_blocks : blast_furnace_misc;
            d.station = station("blast_furnace");
            d.duration = 100;
        } else if (type == "minecraft:smoking") {
            out.category = smoker_food;
            d.station = station("smoker");
            d.duration = 100;
        } else {
            out.category = campfire;
            d.station = station("campfire");
            d.duration = 100;
        }
        if (const auto* time = j.find("cookingtime"); time && time->kind() == Json::Kind::number) d.duration = static_cast<std::int32_t>(time->as_number());
        if (const auto* xp = j.find("experience"); xp && xp->kind() == Json::Kind::number) d.experience = static_cast<float>(xp->as_number());
        const auto* ingredient = j.find("ingredient");
        d.slots.push_back(r.ingredient(ingredient));
        require(ingredient);
        d.result = r.result(j.find("result"));
    } else if (type == "minecraft:stonecutting") {
        out.category = stonecutter;
        d.kind = RecipeDisplay::Kind::stonecutter;
        d.station = station("stonecutter");
        const auto* ingredient = j.find("ingredient");
        d.slots.push_back(r.ingredient(ingredient));
        require(ingredient);
        d.result = r.result(j.find("result"));
    } else if (type == "minecraft:smithing_transform" || type == "minecraft:smithing_trim") {
        out.category = smithing;
        d.kind = RecipeDisplay::Kind::smithing;
        d.station = station("smithing_table");
        for (const auto* part : {j.find("template"), j.find("base"), j.find("addition")}) {
            d.slots.push_back(part ? r.ingredient(part) : SlotDisplay{});
            if (part) require(part);
        }
        if (type == "minecraft:smithing_transform") {
            d.result = r.result(j.find("result"));
        } else {
            // The trimmed armor: its pattern by registry entry (1.21.5 on),
            // else the template's display.
            d.result = {.kind = SlotDisplay::Kind::smithing_trim, .parts = {d.slots[1], d.slots[2]}};
            if (const auto* pattern = j.string("pattern")) {
                for (const auto& reg : registries)
                    if (reg.registry == "minecraft:trim_pattern")
                        if (const auto it = std::ranges::find(reg.entries, with_namespace(*pattern)); it != reg.entries.end())
                            d.result.pattern = static_cast<std::int32_t>(it - reg.entries.begin());
                if (d.result.pattern < 0) throw Bad{};
            } else {
                d.result.parts.push_back(d.slots[0]);
            }
        }
    } else {
        throw Bad{};
    }
    return out;
}

// An advancement's criterion as the rules trigger it.
BookCriterion read_criterion(const Reader& r, const RecipeBook& book, const std::string& name, const Json& j) {
    BookCriterion c{.name = name};
    const auto* trigger = j.string("trigger");
    const auto* conditions = j.find("conditions");
    const auto only = [&](std::initializer_list<std::string_view> keys) {
        if (!conditions) return true;
        return std::ranges::all_of(conditions->members(), [&](const auto& m) { return std::ranges::find(keys, m.first) != keys.end(); });
    };
    if (!trigger) return c;
    const auto t = with_namespace(*trigger);
    try {
        if (t == "minecraft:tick" && !conditions) {
            c.kind = BookCriterion::Kind::tick;
        } else if (t == "minecraft:impossible") {
            c.kind = BookCriterion::Kind::never;
        } else if (t == "minecraft:recipe_unlocked" && conditions && only({"recipe"}) && conditions->string("recipe")) {
            if (const auto it = book.by_id.find(with_namespace(*conditions->string("recipe"))); it != book.by_id.end()) {
                c.kind = BookCriterion::Kind::recipe_unlocked;
                c.recipe = it->second;
            }
        } else if (t == "minecraft:enter_block" && conditions && only({"block"}) && conditions->string("block")
                   && with_namespace(*conditions->string("block")) == "minecraft:water") {
            c.kind = BookCriterion::Kind::in_water;
        } else if (t == "minecraft:inventory_changed" && conditions && only({"items", "slots"})) {
            const auto* items = conditions->find("items");
            const auto* slots = conditions->find("slots");
            if (items && !slots && items->kind() == Json::Kind::array && items->items().size() == 1 && items->items()[0].kind() == Json::Kind::object
                && items->items()[0].members().size() == 1 && items->items()[0].find("items")) {
                c.items = r.items(*items->items()[0].find("items"));
                std::ranges::sort(c.items);
                c.kind = BookCriterion::Kind::inventory_changed;
            } else if (!items && slots && slots->members().size() == 1 && slots->find("occupied")) {
                const auto* occupied = slots->find("occupied");
                const auto* min = occupied->kind() == Json::Kind::object ? occupied->find("min") : nullptr;
                if (min && min->kind() == Json::Kind::number && occupied->members().size() == 1) {
                    c.kind = BookCriterion::Kind::slots_occupied;
                    c.occupied = static_cast<std::int32_t>(min->as_number());
                }
            }
        }
    } catch (const Bad&) {
        c.kind = BookCriterion::Kind::never;
    }
    return c;
}

} // namespace

RecipeBook compile_recipe_book(const data::DataPacks& packs, const contracts::VersionBundle& bundle, const std::vector<RegistryEntries>& registries) {
    RecipeBook book;
    const Reader reader(packs, bundle);
    if (const auto* all = packs.resources("recipe")) {
        // In the official order of ids (display ids and groups follow it).
        std::vector<const std::pair<const std::string, std::string>*> ordered;
        for (const auto& entry : *all) ordered.push_back(&entry);
        std::ranges::stable_sort(ordered, [](const auto* a, const auto* b) { return gameplay::recipes::id_before(a->first, b->first); });
        std::map<std::string, std::int32_t> groups;
        for (const auto* entry : ordered) {
            const auto& [id, text] = *entry;
            const auto j = Json::parse(text);
            if (!j) continue;
            try {
                auto recipe = read_recipe(reader, id, *j, registries);
                ++book.all;
                if (!recipe) continue;
                if (const auto* g = j->string("group"); g && !g->empty() && recipe->in_book)
                    recipe->group = groups.try_emplace(*g, static_cast<std::int32_t>(groups.size()) + 1).first->second;
                book.by_id.emplace(id, book.recipes.size());
                book.recipes.push_back(std::move(*recipe));
            } catch (const Bad&) {
                // A recipe the target version would not load.
            }
        }
    }
    book.by_recipe.resize(book.recipes.size());
    if (const auto* all = packs.resources("advancement")) {
        for (const auto& [id, text] : *all) {
            const auto j = Json::parse(text);
            if (!j || j->kind() != Json::Kind::object) continue;
            const auto* rewards = j->find("rewards");
            const auto* recipes = rewards && rewards->kind() == Json::Kind::object ? rewards->find("recipes") : nullptr;
            if (!recipes || recipes->kind() != Json::Kind::array || recipes->items().empty()) continue;
            BookAdvancement a{.id = id};
            for (const auto& r : recipes->items())
                if (r.kind() == Json::Kind::string)
                    if (const auto it = book.by_id.find(with_namespace(r.as_string())); it != book.by_id.end() && book.recipes[it->second].in_book)
                        a.rewards.push_back(it->second);
            const auto* criteria = j->find("criteria");
            if (!criteria || criteria->kind() != Json::Kind::object) continue;
            for (const auto& [name, c] : criteria->members()) {
                a.criteria.push_back(read_criterion(reader, book, name, c));
                if (a.criteria.back().kind == BookCriterion::Kind::never) ++book.unsupported;
            }
            const auto index_of = [&](const std::string& name) -> std::optional<std::size_t> {
                for (std::size_t k = 0; k < a.criteria.size(); ++k)
                    if (a.criteria[k].name == name) return k;
                return std::nullopt;
            };
            if (const auto* req = j->find("requirements"); req && req->kind() == Json::Kind::array) {
                for (const auto& group : req->items()) {
                    std::vector<std::size_t> any;
                    for (const auto& n : group.items())
                        if (const auto k = n.kind() == Json::Kind::string ? index_of(n.as_string()) : std::nullopt) any.push_back(*k);
                    a.requirements.push_back(std::move(any));
                }
            } else {
                // Without requirements every criterion is needed.
                for (std::size_t k = 0; k < a.criteria.size(); ++k) a.requirements.push_back({k});
            }
            book.advancement_ids.emplace(id, book.advancements.size());
            book.advancements.push_back(std::move(a));
        }
    }
    for (std::size_t a = 0; a < book.advancements.size(); ++a)
        for (std::size_t k = 0; k < book.advancements[a].criteria.size(); ++k) {
            const auto& c = book.advancements[a].criteria[k];
            switch (c.kind) {
            case BookCriterion::Kind::inventory_changed:
                for (const auto item : c.items) book.by_item[item].push_back({a, k});
                break;
            case BookCriterion::Kind::slots_occupied: book.occupied.push_back({a, k}); break;
            case BookCriterion::Kind::recipe_unlocked: book.by_recipe[c.recipe].push_back({a, k}); break;
            case BookCriterion::Kind::tick: book.on_tick.push_back({a, k}); break;
            case BookCriterion::Kind::in_water: book.in_water.push_back({a, k}); break;
            case BookCriterion::Kind::never: break;
            }
        }
    for (auto& [item, refs] : book.by_item) {
        std::ranges::sort(refs);
        refs.erase(std::ranges::unique(refs).begin(), refs.end());
    }
    return book;
}

Unlocks award(const RecipeBook& book, PlayerBook& player, std::span<const std::size_t> recipes) {
    std::vector<std::size_t> fresh;
    for (const auto r : recipes)
        if (r < book.recipes.size() && book.recipes[r].in_book && player.known.insert(r).second) {
            player.highlighted.insert(r);
            fresh.push_back(r);
        }
    Unlocks out;
    if (fresh.empty()) return out;
    out.push_back(fresh);
    std::vector<CriterionRef> unlocked;
    for (const auto r : fresh) unlocked.insert(unlocked.end(), book.by_recipe[r].begin(), book.by_recipe[r].end());
    auto more = trigger(book, player, std::move(unlocked));
    out.insert(out.end(), more.begin(), more.end());
    return out;
}

Unlocks trigger(const RecipeBook& book, PlayerBook& player, std::vector<CriterionRef> criteria) {
    std::ranges::sort(criteria);
    Unlocks out;
    for (const auto& [a, k] : criteria) {
        if (player.done.contains(a)) continue;
        auto& progress = player.progress[a];
        progress.insert(k);
        const auto& adv = book.advancements[a];
        const bool complete = std::ranges::all_of(adv.requirements, [&](const auto& any) {
            return std::ranges::any_of(any, [&](std::size_t c) { return progress.contains(c); });
        });
        if (!complete) continue;
        player.progress.erase(a);
        player.done.insert(a);
        auto more = award(book, player, adv.rewards);
        out.insert(out.end(), more.begin(), more.end());
    }
    return out;
}

std::vector<CriterionRef> inventory_criteria(const RecipeBook& book, const PlayerBook& player, std::span<const std::uint16_t> changed,
                                             std::int32_t occupied) {
    std::vector<CriterionRef> out;
    for (const auto item : changed)
        if (const auto it = book.by_item.find(item); it != book.by_item.end())
            for (const auto& ref : it->second)
                if (!player.done.contains(ref.first)) out.push_back(ref);
    for (const auto& ref : book.occupied)
        if (!player.done.contains(ref.first) && occupied >= book.advancements[ref.first].criteria[ref.second].occupied) out.push_back(ref);
    return out;
}

std::vector<std::size_t> forget(PlayerBook& player, std::span<const std::size_t> recipes) {
    std::vector<std::size_t> out;
    for (const auto r : recipes)
        if (player.known.erase(r)) {
            player.highlighted.erase(r);
            out.push_back(r);
        }
    std::ranges::sort(out);
    return out;
}

} // namespace circuit::server
