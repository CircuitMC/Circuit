// SPDX-License-Identifier: AGPL-3.0-only
#include "server_data.hpp"
#include <circuit/server/data_report.hpp>
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/enchantments.hpp>
#include <circuit/gameplay/equipment.hpp>
#include <circuit/auth/json.hpp>
#include <circuit/gameplay/potions.hpp>
#include <circuit/protocol/content.hpp>
#include <algorithm>
#include <functional>
#include <array>
#include <cctype>
#include <format>
#include <initializer_list>
#include <set>
#include <tuple>
#include <unordered_map>

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

namespace {
// A level-based value of an enchantment's data (minecraft.wiki
// "Enchantment definition"): a number, or a compound by its type.
std::optional<gameplay::enchantments::LevelValue> level_value(const auth::Json& json) {
    using gameplay::enchantments::LevelValue;
    using Kind = LevelValue::Kind;
    LevelValue v;
    if (json.kind() == auth::Json::Kind::number) {
        v.a = static_cast<float>(json.as_number());
        return v;
    }
    const auto* type = json.find("type");
    if (!type || type->kind() != auth::Json::Kind::string) return std::nullopt;
    const auto number = [&](const char* key) -> std::optional<float> {
        const auto* f = json.find(key);
        if (!f || f->kind() != auth::Json::Kind::number) return std::nullopt;
        return static_cast<float>(f->as_number());
    };
    const auto nested = [&](const char* key) -> bool {
        const auto* f = json.find(key);
        auto part = f ? level_value(*f) : std::nullopt;
        if (!part) return false;
        v.parts.push_back(std::move(*part));
        return true;
    };
    const auto t = type->as_string();
    if (t == "minecraft:constant" || t == "constant") {
        const auto value = number("value");
        if (!value) return std::nullopt;
        v.a = *value;
    } else if (t == "minecraft:linear" || t == "linear") {
        const auto base = number("base"), per = number("per_level_above_first");
        if (!base || !per) return std::nullopt;
        v.kind = Kind::linear;
        v.a = *base;
        v.b = *per;
    } else if (t == "minecraft:levels_squared" || t == "levels_squared") {
        const auto added = number("added");
        if (!added) return std::nullopt;
        v.kind = Kind::levels_squared;
        v.a = *added;
    } else if (t == "minecraft:fraction" || t == "fraction") {
        v.kind = Kind::fraction;
        if (!nested("numerator") || !nested("denominator")) return std::nullopt;
    } else if (t == "minecraft:clamped" || t == "clamped") {
        const auto min = number("min"), max = number("max");
        v.kind = Kind::clamped;
        if (!min || !max || !nested("value")) return std::nullopt;
        v.a = *min;
        v.b = *max;
    } else if (t == "minecraft:lookup" || t == "lookup") {
        v.kind = Kind::lookup;
        const auto* values = json.find("values");
        if (!values || values->kind() != auth::Json::Kind::array || !nested("fallback")) return std::nullopt;
        for (const auto& e : values->items()) {
            if (e.kind() != auth::Json::Kind::number) return std::nullopt;
            v.values.push_back(static_cast<float>(e.as_number()));
        }
    } else if (t == "minecraft:exponent" || t == "exponent") {
        v.kind = Kind::exponent;
        if (!nested("base") || !nested("power")) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return v;
}

// A damage effect's requirement (its `requirements`), of the forms
// DamageRequirement knows; nullopt for another form, a predicate on more
// than the damage type's tags and (where `direct_known`, for attacks)
// whether it is direct, an entity predicate on more than the type of
// `this`, the attacker or the direct attacker, a tag the packs do not
// define, or a random chance where Circuit draws none (`chance_known`: for
// post-attack effects) or of a chance other than a number, a constant or
// the enchantment's level-based value (enchantment_level).
std::optional<gameplay::enchantments::DamageRequirement> damage_requirement(const auth::Json& json, const data::DataPacks& packs,
                                                                            bool direct_known, bool chance_known) {
    using gameplay::enchantments::DamageRequirement;
    using Kind = DamageRequirement::Kind;
    const auto* condition = json.find("condition");
    if (!condition || condition->kind() != auth::Json::Kind::string) return std::nullopt;
    auto c = condition->as_string();
    if (c.starts_with("minecraft:")) c.erase(0, 10);
    DamageRequirement r;
    const auto term = [&](const auth::Json& t) {
        auto part = damage_requirement(t, packs, direct_known, chance_known);
        if (part) r.terms.push_back(std::move(*part));
        return part.has_value();
    };
    const auto named = [](std::string id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; };
    if (c == "damage_source_properties") {
        const auto* predicate = json.find("predicate");
        if (!predicate || predicate->kind() != auth::Json::Kind::object) return std::nullopt;
        for (const auto& [key, value] : predicate->members()) {
            if (key == "is_direct" && direct_known && value.kind() == auth::Json::Kind::boolean) {
                r.direct = value.as_bool();
                continue;
            }
            if (key != "tags" || value.kind() != auth::Json::Kind::array) return std::nullopt;
            for (const auto& t : value.items()) {
                const auto* id = t.find("id");
                const auto* expected = t.find("expected");
                if (!id || id->kind() != auth::Json::Kind::string || !expected || expected->kind() != auth::Json::Kind::boolean)
                    return std::nullopt;
                const auto* types = packs.tag("damage_type", id->as_string());
                if (!types) return std::nullopt;
                r.tags.push_back({*types, expected->as_bool()});
            }
        }
    } else if (c == "entity_properties") {
        r.kind = Kind::entity;
        const auto* entity = json.find("entity");
        const auto* predicate = json.find("predicate");
        if (!entity || entity->kind() != auth::Json::Kind::string || !predicate || predicate->kind() != auth::Json::Kind::object
            || predicate->members().size() != 1)
            return std::nullopt;
        using Subject = DamageRequirement::Subject;
        if (entity->as_string() == "this") r.subject = Subject::self;
        else if (entity->as_string() == "attacker") r.subject = Subject::attacker;
        else if (entity->as_string() == "direct_attacker") r.subject = Subject::direct_attacker;
        else return std::nullopt;
        const auto* type = predicate->find("type");
        if (!type) return std::nullopt;
        if (type->kind() == auth::Json::Kind::string && type->as_string().starts_with("#")) {
            const auto* types = packs.tag("entity_type", type->as_string().substr(1));
            if (!types) return std::nullopt;
            r.entities = *types;
        } else if (type->kind() == auth::Json::Kind::string) {
            r.entities.push_back(named(type->as_string()));
        } else if (type->kind() == auth::Json::Kind::array) {
            for (const auto& e : type->items()) {
                if (e.kind() != auth::Json::Kind::string || e.as_string().starts_with("#")) return std::nullopt;
                r.entities.push_back(named(e.as_string()));
            }
        } else {
            return std::nullopt;
        }
        std::ranges::sort(r.entities);
    } else if (c == "random_chance") {
        r.kind = Kind::chance;
        const auto* chance = json.find("chance");
        if (!chance_known || !chance) return std::nullopt;
        if (chance->kind() == auth::Json::Kind::number) {
            r.chance.a = static_cast<float>(chance->as_number());
        } else {
            const auto* type = chance->find("type");
            if (!type || type->kind() != auth::Json::Kind::string) return std::nullopt;
            auto t = type->as_string();
            if (t.starts_with("minecraft:")) t.erase(0, 10);
            const auto* value = t == "enchantment_level" ? chance->find("amount") : t == "constant" ? chance->find("value") : nullptr;
            auto read = value && (t == "enchantment_level" || value->kind() == auth::Json::Kind::number) ? level_value(*value) : std::nullopt;
            if (!read) return std::nullopt;
            r.chance = std::move(*read);
        }
    } else if (c == "all_of" || c == "any_of") {
        r.kind = c == "all_of" ? Kind::all_of : Kind::any_of;
        const auto* terms = json.find("terms");
        if (!terms || terms->kind() != auth::Json::Kind::array) return std::nullopt;
        for (const auto& t : terms->items())
            if (!term(t)) return std::nullopt;
    } else if (c == "inverted") {
        r.kind = Kind::inverted;
        const auto* t = json.find("term");
        if (!t || !term(*t)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return r;
}

// An item effect's requirement (its `requirements`), of the forms
// ItemRequirement knows: match_tool with an item predicate on its items
// alone (an item, a list of items or an item tag the packs define),
// all_of, any_of and inverted; nullopt for any other.
std::optional<gameplay::enchantments::ItemRequirement> item_requirement(const auth::Json& json, const data::DataPacks& packs) {
    using gameplay::enchantments::ItemRequirement;
    using Kind = ItemRequirement::Kind;
    const auto* condition = json.find("condition");
    if (!condition || condition->kind() != auth::Json::Kind::string) return std::nullopt;
    auto c = condition->as_string();
    if (c.starts_with("minecraft:")) c.erase(0, 10);
    ItemRequirement r;
    const auto term = [&](const auth::Json& t) {
        auto part = item_requirement(t, packs);
        if (part) r.terms.push_back(std::move(*part));
        return part.has_value();
    };
    const auto named = [](std::string id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; };
    if (c == "match_tool") {
        const auto* predicate = json.find("predicate");
        const auto* items = predicate && predicate->kind() == auth::Json::Kind::object && predicate->members().size() == 1 ? predicate->find("items")
                                                                                                                          : nullptr;
        if (!items) return std::nullopt;
        if (items->kind() == auth::Json::Kind::string && items->as_string().starts_with("#")) {
            const auto* tagged = packs.tag("item", items->as_string().substr(1));
            if (!tagged) return std::nullopt;
            r.items = *tagged;
        } else if (items->kind() == auth::Json::Kind::string) {
            r.items.push_back(named(items->as_string()));
        } else if (items->kind() == auth::Json::Kind::array) {
            for (const auto& i : items->items()) {
                if (i.kind() != auth::Json::Kind::string || i.as_string().starts_with("#")) return std::nullopt;
                r.items.push_back(named(i.as_string()));
            }
        } else {
            return std::nullopt;
        }
        std::ranges::sort(r.items);
    } else if (c == "all_of" || c == "any_of") {
        r.kind = c == "all_of" ? Kind::all_of : Kind::any_of;
        const auto* terms = json.find("terms");
        if (!terms || terms->kind() != auth::Json::Kind::array) return std::nullopt;
        for (const auto& t : terms->items())
            if (!term(t)) return std::nullopt;
    } else if (c == "inverted") {
        r.kind = Kind::inverted;
        const auto* t = json.find("term");
        if (!t || !term(*t)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return r;
}

// An enchantment's slots, attribute modifiers (its minecraft:attributes
// effects), damage protection, damage immunity, the damage and knockback it
// adds to attacks, and its post-attack effects of the entity effects ignite,
// damage_entity of the damage type thorns and change_item_damage (alone or
// in all_of; the attacker or the damaging entity, which a melee attack's
// attacker is, or the victim enchanted and affected): refused when it names
// a slot group, attribute, value form or requirement Circuit does not know,
// or such an effect it cannot read. Other post-attack effects (other entity
// effects, or damage of other types, alone or among others) are not acted
// on yet (bane of arthropods' slowness, whose requirement no player meets;
// channeling's lightning and wind burst's explosion). Its item_damage
// effects of the form remove_binomial (unbreaking) with requirements on the
// item are read; another form refuses the packs.
std::expected<gameplay::enchantments::Effects, std::string> enchantment_effects(std::string_view name, const auth::Json& json,
                                                                               const data::DataPacks& packs) {
    namespace en = gameplay::enchantments;
    namespace at = gameplay::attributes;
    en::Effects out;
    if (const auto* slots = json.find("slots")) {
        if (slots->kind() != auth::Json::Kind::array) return std::unexpected(std::format("enchantment {}: slots are not a list", name));
        for (const auto& group : slots->items()) {
            const auto bits = group.kind() == auth::Json::Kind::string ? en::slot_group(group.as_string()) : std::nullopt;
            if (!bits) return std::unexpected(std::format("enchantment {}: an unknown slot group", name));
            out.slots = static_cast<std::uint8_t>(out.slots | *bits);
        }
    }
    const auto* effects = json.find("effects");
    // An effect's requirement, if it has one (refused when unreadable);
    // whether the damage is direct is known for attacks alone.
    const auto requirement = [&](const auth::Json& e, bool attack,
                                 bool post_attack = false) -> std::expected<std::optional<en::DamageRequirement>, std::string> {
        const auto* r = e.find("requirements");
        if (!r) return std::nullopt;
        auto read = damage_requirement(*r, packs, attack, post_attack);
        if (!read) return std::unexpected(std::format("enchantment {}: a damage requirement Circuit cannot read", name));
        return std::optional<en::DamageRequirement>(std::move(*read));
    };
    // Value effects that add a level-based value (the vanilla packs' only form).
    const auto values = [&](const char* component, const char* what, bool attack,
                            std::vector<en::ValueEffect>& into) -> std::expected<void, std::string> {
        const auto* list = effects ? effects->find(component) : nullptr;
        if (!list) return {};
        if (list->kind() != auth::Json::Kind::array) return std::unexpected(std::format("enchantment {}: {} is not a list", name, what));
        for (const auto& e : list->items()) {
            const auto* effect = e.find("effect");
            const auto* type = effect ? effect->find("type") : nullptr;
            const auto* amount = effect ? effect->find("value") : nullptr;
            auto value = amount ? level_value(*amount) : std::nullopt;
            if (!type || type->kind() != auth::Json::Kind::string || (type->as_string() != "minecraft:add" && type->as_string() != "add") || !value)
                return std::unexpected(std::format("enchantment {}: a {} effect Circuit cannot read", name, what));
            auto r = requirement(e, attack);
            if (!r) return std::unexpected(r.error());
            into.push_back({std::move(*value), std::move(*r)});
        }
        return {};
    };
    if (auto v = values("minecraft:damage_protection", "damage protection", false, out.protection); !v) return std::unexpected(v.error());
    if (auto v = values("minecraft:damage", "damage", true, out.damage); !v) return std::unexpected(v.error());
    if (auto v = values("minecraft:knockback", "knockback", true, out.knockback); !v) return std::unexpected(v.error());
    if (const auto* list = effects ? effects->find("minecraft:damage_immunity") : nullptr) {
        if (list->kind() != auth::Json::Kind::array) return std::unexpected(std::format("enchantment {}: damage immunity is not a list", name));
        for (const auto& e : list->items()) {
            auto r = requirement(e, false);
            if (!r) return std::unexpected(r.error());
            out.immunity.push_back(std::move(*r));
        }
    }
    if (const auto* list = effects ? effects->find("minecraft:post_attack") : nullptr) {
        if (list->kind() != auth::Json::Kind::array) return std::unexpected(std::format("enchantment {}: post-attack effects are not a list", name));
        const auto unreadable = [&] { return std::unexpected(std::format("enchantment {}: a post-attack effect Circuit cannot read", name)); };
        const auto named = [](const auth::Json* j) {
            std::string s = j && j->kind() == auth::Json::Kind::string ? j->as_string() : std::string{};
            if (s.starts_with("minecraft:")) s.erase(0, 10);
            return s;
        };
        // An entity effect into `into` (all_of's in order): false when
        // Circuit does not act on it; nullopt when it cannot read it.
        std::function<std::optional<bool>(const auth::Json&, std::vector<en::EntityEffect>&)> entity_effect =
            [&](const auth::Json& effect, std::vector<en::EntityEffect>& into) -> std::optional<bool> {
            const auto* type = effect.find("type");
            if (!type || type->kind() != auth::Json::Kind::string) return std::nullopt;
            const auto t = named(type);
            if (t == "all_of") {
                const auto* parts = effect.find("effects");
                if (!parts || parts->kind() != auth::Json::Kind::array) return std::nullopt;
                bool acted = true;
                for (const auto& part : parts->items()) {
                    const auto read = entity_effect(part, into);
                    if (!read) return std::nullopt;
                    acted = acted && *read;
                }
                return acted;
            }
            en::EntityEffect step;
            if (t == "ignite") {
                const auto* duration = effect.find("duration");
                auto seconds = duration ? level_value(*duration) : std::nullopt;
                if (!seconds) return std::nullopt;
                step.kind = en::EntityEffect::Kind::ignite;
                step.a = std::move(*seconds);
            } else if (t == "damage_entity") {
                const auto* least = effect.find("min_damage");
                const auto* most = effect.find("max_damage");
                const auto* damage_type = effect.find("damage_type");
                auto low = least ? level_value(*least) : std::nullopt;
                auto high = most ? level_value(*most) : std::nullopt;
                if (!low || !high || !damage_type || damage_type->kind() != auth::Json::Kind::string) return std::nullopt;
                step.kind = en::EntityEffect::Kind::damage;
                step.a = std::move(*low);
                step.b = std::move(*high);
                step.damage_type = "minecraft:" + named(damage_type);
                if (step.damage_type != "minecraft:thorns") return false; // the damage types Circuit deals this way
            } else if (t == "change_item_damage") {
                const auto* amount = effect.find("amount");
                auto value = amount ? level_value(*amount) : std::nullopt;
                if (!value) return std::nullopt;
                step.kind = en::EntityEffect::Kind::wear;
                step.a = std::move(*value);
            } else {
                return false; // not acted on yet
            }
            into.push_back(std::move(step));
            return true;
        };
        const auto party = [&](const auth::Json* j) -> std::optional<en::Party> {
            const auto p = j && j->kind() == auth::Json::Kind::string ? j->as_string() : std::string{};
            if (p == "attacker" || p == "damaging_entity") return en::Party::attacker;
            if (p == "victim") return en::Party::victim;
            return std::nullopt;
        };
        for (const auto& e : list->items()) {
            const auto* effect = e.find("effect");
            if (!effect || effect->kind() != auth::Json::Kind::object) return unreadable();
            en::PostAttackEffect effect_read;
            const auto enchanted = party(e.find("enchanted")), affected = party(e.find("affected"));
            if (!enchanted || !affected) return unreadable();
            effect_read.enchanted = *enchanted;
            effect_read.affected = *affected;
            const auto read = entity_effect(*effect, effect_read.effects);
            if (!read) return unreadable();
            if (!*read) continue; // not acted on yet, nor its requirement read (channeling's weather and sky)
            auto r = requirement(e, true, true);
            if (!r) return std::unexpected(r.error());
            effect_read.requirement = std::move(*r);
            out.post_attack.push_back(std::move(effect_read));
        }
    }
    if (const auto* list = effects ? effects->find("minecraft:item_damage") : nullptr) {
        if (list->kind() != auth::Json::Kind::array) return std::unexpected(std::format("enchantment {}: item damage effects are not a list", name));
        for (const auto& e : list->items()) {
            const auto* effect = e.find("effect");
            const auto* type = effect ? effect->find("type") : nullptr;
            const auto* chance = effect ? effect->find("chance") : nullptr;
            auto value = chance ? level_value(*chance) : std::nullopt;
            if (!type || type->kind() != auth::Json::Kind::string
                || (type->as_string() != "minecraft:remove_binomial" && type->as_string() != "remove_binomial") || !value)
                return std::unexpected(std::format("enchantment {}: an item damage effect Circuit cannot read", name));
            en::ItemDamageEffect read{std::move(*value), std::nullopt};
            if (const auto* r = e.find("requirements")) {
                auto requirement = item_requirement(*r, packs);
                if (!requirement) return std::unexpected(std::format("enchantment {}: an item requirement Circuit cannot read", name));
                read.requirement = std::move(*requirement);
            }
            out.item_damage.push_back(std::move(read));
        }
    }
    const auto* list = effects ? effects->find("minecraft:attributes") : nullptr;
    if (!list) return out;
    if (list->kind() != auth::Json::Kind::array) return std::unexpected(std::format("enchantment {}: attributes are not a list", name));
    for (const auto& e : list->items()) {
        const auto* attribute = e.find("attribute");
        const auto* id = e.find("id");
        const auto* amount = e.find("amount");
        const auto* operation = e.find("operation");
        const auto a = attribute && attribute->kind() == auth::Json::Kind::string ? at::by_name(attribute->as_string()) : std::nullopt;
        if (!a) return std::unexpected(std::format("enchantment {}: an attribute Circuit does not model", name));
        auto value = amount ? level_value(*amount) : std::nullopt;
        if (!value || !id || id->kind() != auth::Json::Kind::string || !operation || operation->kind() != auth::Json::Kind::string)
            return std::unexpected(std::format("enchantment {}: an attribute effect Circuit cannot read", name));
        en::AttributeEffect effect;
        effect.attribute = *a;
        effect.amount = std::move(*value);
        const auto op = operation->as_string();
        if (op == "add_value") effect.operation = at::Operation::add_value;
        else if (op == "add_multiplied_base") effect.operation = at::Operation::add_multiplied_base;
        else if (op == "add_multiplied_total") effect.operation = at::Operation::add_multiplied_total;
        else return std::unexpected(std::format("enchantment {}: an unknown attribute operation", name));
        auto base = id->as_string();
        if (base.find(':') != std::string::npos && !base.starts_with("minecraft:"))
            return std::unexpected(std::format("enchantment {}: an attribute modifier outside the minecraft namespace", name));
        if (base.starts_with("minecraft:")) base.erase(0, 10);
        for (std::size_t s = 0; s < gameplay::equipment::slot_count; ++s)
            effect.ids[s] = base + "/" + std::string(gameplay::equipment::slot_name(static_cast<gameplay::equipment::Slot>(s)));
        out.attributes.push_back(std::move(effect));
    }
    return out;
}
} // namespace

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
        out.recipe_book = std::make_shared<const RecipeBook>(compile_recipe_book(*packs, bundle, out.registries));
        game->crafting = std::move(out.recipes.crafting);
        out.recipes.crafting.clear();
        game->cooking = std::move(out.recipes.cooking);
        game->special = std::move(out.recipes.special);
        out.recipes.cooking.clear();
        game->stonecutting = std::move(out.recipes.stonecutting);
        out.recipes.stonecutting.clear();
        game->smithing = std::move(out.recipes.smithing);
        out.recipes.smithing.clear();
        game->smithing_template = out.recipes.smithing_template;
        game->smithing_base = out.recipes.smithing_base;
        game->smithing_addition = out.recipes.smithing_addition;
        game->trim_materials = out.recipes.trim_materials;
        game->trim_patterns = out.recipes.trim_patterns;
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
        if (const auto* codec = protocol::ComponentCodec::for_protocol(bundle.schema.component_schema)) {
            game->trimmed = [codec, ctx = component_context(bundle, out.registries)](const contracts::ComponentPatch& base, std::string_view material,
                                                                                     std::string_view pattern) -> std::optional<contracts::ComponentPatch> {
                const auto m = ctx.id_of("trim_material", material);
                const auto p = ctx.id_of("trim_pattern", pattern);
                if (!m || !p) return std::nullopt;
                auto patch = codec->with_trim(base, *m, *p, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
        }
        if (const auto* codec = protocol::ComponentCodec::for_protocol(bundle.schema.component_schema)) {
            game->enchantments_of = [codec](const contracts::ComponentPatch& patch, bool stored) { return codec->enchantments_of(patch, stored); };
            game->with_enchantments = [codec, ctx = component_context(bundle, out.registries)](
                                          const contracts::ComponentPatch& base, bool stored,
                                          const std::vector<std::pair<std::uint32_t, std::int32_t>>& levels, bool keep_empty,
                                          std::int32_t repair_cost) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_enchantments(base, stored, levels, keep_empty, repair_cost, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
        }
        game->keeps_empty_book_enchantments = bundle.protocol == 769;
        if (const auto* free = packs->ordered_tag("banner_pattern", "minecraft:no_item_required")) game->loom_patterns = *free;
        for (std::uint16_t item = 1; const auto* info = gameplay::items::info(item); ++item) {
            const std::string_view name = info->name;
            if (!name.ends_with("_banner_pattern")) continue;
            const auto tag = std::format("minecraft:pattern_item/{}", name.substr(10, name.size() - 10 - 15));
            if (const auto* patterns = packs->ordered_tag("banner_pattern", tag)) game->pattern_item_patterns.emplace(item, *patterns);
        }
        if (const auto* codec = protocol::ComponentCodec::for_protocol(bundle.schema.component_schema)) {
            game->with_banner_layer = [codec, ctx = component_context(bundle, out.registries)](
                                          const contracts::ComponentPatch& base, std::string_view pattern,
                                          std::uint32_t color) -> std::optional<contracts::ComponentPatch> {
                const auto id = ctx.id_of("banner_pattern", pattern);
                if (!id) return std::nullopt;
                auto patch = codec->with_banner_layer(base, *id, color, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
        }
        if (const auto* codec = protocol::ComponentCodec::for_protocol(bundle.schema.component_schema)) {
            game->potion_of = [codec](const contracts::ComponentPatch& patch) -> std::optional<std::string_view> {
                const auto id = codec->potion_of(patch);
                const auto names = gameplay::potions::names();
                if (!id || *id >= names.size()) return std::nullopt;
                return names[*id];
            };
            game->with_potion = [codec, ctx = component_context(bundle, out.registries)](
                                    const contracts::ComponentPatch& base, std::string_view potion) -> std::optional<contracts::ComponentPatch> {
                const auto id = gameplay::potions::id(potion);
                if (!id) return std::nullopt;
                auto patch = codec->with_potion(base, *id, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
        }
        if (const auto* fuel = packs->tag("item", "minecraft:brewing_fuel")) {
            for (const auto& name : *fuel)
                if (const auto item = gameplay::items::by_name(name)) game->brewing_fuel.push_back(*item);
            std::ranges::sort(game->brewing_fuel);
        }
        // Enchantments that set a broken block's experience (Silk Touch),
        // and every enchantment's minimum cost (grindstones).
        if (const auto* defined = packs->resources("enchantment")) {
            const auto ctx = component_context(bundle, out.registries);
            for (const auto& [name, text] : *defined) {
                const auto json = auth::Json::parse(text);
                if (const auto* cost = json ? json->find("min_cost") : nullptr) {
                    const auto* base = cost->find("base");
                    const auto* per = cost->find("per_level_above_first");
                    if (const auto id = ctx.id_of("enchantment", name); id && base && per)
                        game->enchantment_min_cost[*id] = {static_cast<std::int32_t>(base->as_number()), static_cast<std::int32_t>(per->as_number())};
                }
                const auto* effects = json ? json->find("effects") : nullptr;
                const auto* list = effects ? effects->find("minecraft:block_experience") : nullptr;
                if (!list || list->kind() != auth::Json::Kind::array) continue;
                for (const auto& entry : list->items()) {
                    const auto* effect = entry.find("effect");
                    const auto* type = effect ? effect->find("type") : nullptr;
                    const auto* value = effect ? effect->find("value") : nullptr;
                    if (!type || type->as_string() != "minecraft:set" || !value || value->kind() != auth::Json::Kind::number) continue;
                    if (const auto id = ctx.id_of("enchantment", name))
                        game->block_experience_set.emplace_back(*id, static_cast<float>(value->as_number()));
                }
            }
        }
        // Anvils: each enchantment's cost, highest level, items and exclusions;
        // each item's repair materials (the material's item tag).
        const auto items_of = [&](const auth::Json* spec) {
            std::vector<std::uint16_t> ids;
            const auto add = [&](const std::string& name) {
                if (name.starts_with("#")) {
                    if (const auto* tag = packs->tag("item", name.substr(1)))
                        for (const auto& member : *tag)
                            if (const auto id = gameplay::items::by_name(member)) ids.push_back(*id);
                } else if (const auto id = gameplay::items::by_name(name)) {
                    ids.push_back(*id);
                }
            };
            if (spec && spec->kind() == auth::Json::Kind::string) add(spec->as_string());
            if (spec && spec->kind() == auth::Json::Kind::array)
                for (const auto& e : spec->items())
                    if (e.kind() == auth::Json::Kind::string) add(e.as_string());
            std::ranges::sort(ids);
            ids.erase(std::ranges::unique(ids).begin(), ids.end());
            return ids;
        };
        if (const auto* defined = packs->resources("enchantment")) {
            const auto ctx = component_context(bundle, out.registries);
            for (const auto& [name, text] : *defined) {
                const auto json = auth::Json::parse(text);
                const auto id = ctx.id_of("enchantment", name);
                if (!json || !id) continue;
                gameplay::GameData::EnchantmentRules rules;
                if (const auto* cost = json->find("anvil_cost")) rules.anvil_cost = static_cast<std::int32_t>(cost->as_number());
                if (const auto* level = json->find("max_level")) rules.max_level = static_cast<std::int32_t>(level->as_number());
                rules.supported = items_of(json->find("supported_items"));
                if (const auto* set = json->find("exclusive_set")) {
                    std::vector<std::string> names;
                    if (set->kind() == auth::Json::Kind::string && set->as_string().starts_with("#")) {
                        if (const auto* tag = packs->tag("enchantment", set->as_string().substr(1))) names = *tag;
                    } else if (set->kind() == auth::Json::Kind::string) {
                        names.push_back(set->as_string());
                    } else if (set->kind() == auth::Json::Kind::array) {
                        for (const auto& e : set->items()) names.push_back(e.as_string());
                    }
                    for (const auto& other : names)
                        if (const auto o = ctx.id_of("enchantment", other)) rules.exclusive.push_back(*o);
                    std::ranges::sort(rules.exclusive);
                }
                game->enchantment_rules.emplace(*id, std::move(rules));
                // What it does to players: its slots and attribute modifiers
                // (refused when the data asks for what Circuit cannot do).
                auto effects = enchantment_effects(name, *json, *packs);
                if (!effects) return std::unexpected(effects.error());
                if (!effects->empty()) game->enchantment_effects.emplace(*id, std::move(*effects));
            }
        }
        for (const auto* tag : {"minecraft:bypasses_effects", "minecraft:bypasses_enchantments"})
            if (const auto* bypassing = packs->tag("damage_type", tag))
                game->damage_bypassing_enchantments.insert(game->damage_bypassing_enchantments.end(), bypassing->begin(), bypassing->end());
        std::ranges::sort(game->damage_bypassing_enchantments);
        // Enchanting tables: the enchantments they offer, in the tag's order,
        // and the blocks that power them and let the power through.
        if (const auto* offered = packs->ordered_tag("enchantment", "minecraft:in_enchanting_table"))
            if (const auto* defined = packs->resources("enchantment")) {
                const auto ctx = component_context(bundle, out.registries);
                const auto pair_of = [](const auth::Json* cost) {
                    const auto* base = cost ? cost->find("base") : nullptr;
                    const auto* per = cost ? cost->find("per_level_above_first") : nullptr;
                    return std::pair<std::int32_t, std::int32_t>{base ? static_cast<std::int32_t>(base->as_number()) : 0,
                                                                 per ? static_cast<std::int32_t>(per->as_number()) : 0};
                };
                for (const auto& name : *offered) {
                    const auto it = defined->find(name);
                    const auto id = ctx.id_of("enchantment", name);
                    if (it == defined->end() || !id) continue;
                    const auto json = auth::Json::parse(it->second);
                    if (!json) continue;
                    gameplay::GameData::TableEnchantment e;
                    e.id = *id;
                    if (const auto* weight = json->find("weight")) e.weight = static_cast<std::int32_t>(weight->as_number());
                    if (const auto* level = json->find("max_level")) e.max_level = static_cast<std::int32_t>(level->as_number());
                    e.min_cost = pair_of(json->find("min_cost"));
                    e.max_cost = pair_of(json->find("max_cost"));
                    const auto* primary = json->find("primary_items");
                    e.primary = items_of(primary ? primary : json->find("supported_items"));
                    game->table_enchantments.push_back(std::move(e));
                }
            }
        const auto blocks_of = [&](std::string_view tag) {
            std::vector<std::uint32_t> blocks;
            if (const auto* members = packs->tag("block", tag))
                for (const auto& member : *members)
                    if (const auto b = gameplay::registry::find_block(bundle.schema.content_release, member)) blocks.push_back(*b);
            std::ranges::sort(blocks);
            return blocks;
        };
        // Armor dye: the items it takes.
        if (const auto* dyeable = packs->tag("item", "minecraft:dyeable"))
            for (const auto& member : *dyeable)
                if (const auto id = gameplay::items::by_name(member)) game->dyeable.push_back(*id);
        std::ranges::sort(game->dyeable);
        // Decorated pots: the items they take.
        if (const auto* ingredients = packs->tag("item", "minecraft:decorated_pot_ingredients"))
            for (const auto& member : *ingredients)
                if (const auto id = gameplay::items::by_name(member)) game->pot_ingredients.push_back(*id);
        std::ranges::sort(game->pot_ingredients);
        // Lecterns: the books they take, and a book's pages.
        if (const auto* books = packs->tag("item", "minecraft:lectern_books"))
            for (const auto& member : *books)
                if (const auto id = gameplay::items::by_name(member)) game->lectern_books.push_back(*id);
        std::ranges::sort(game->lectern_books);
        if (const auto* books = packs->tag("item", "minecraft:bookshelf_books"))
            for (const auto& member : *books)
                if (const auto id = gameplay::items::by_name(member)) game->bookshelf_books.push_back(*id);
        std::ranges::sort(game->bookshelf_books);
        // Beacons: their pyramid blocks and payments.
        game->beacon_base_blocks = blocks_of("minecraft:beacon_base_blocks");
        if (const auto* payments = packs->tag("item", "minecraft:beacon_payment_items"))
            for (const auto& member : *payments)
                if (const auto id = gameplay::items::by_name(member)) game->beacon_payment_items.push_back(*id);
        std::ranges::sort(game->beacon_payment_items);
        if (const auto* codec = protocol::ComponentCodec::for_protocol(bundle.schema.component_schema))
        {
            game->book_pages = [codec](const contracts::ItemStack& book) { return codec->book_pages_of(book.components); };
            game->writable_book = [codec, ctx = component_context(bundle, out.registries)](std::int32_t pages) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->writable_book(pages, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            // Shulker box items' contents: their stacks' protocol item ids
            // are the version's.
            const auto item_of = [tables](std::int32_t wire) -> std::optional<std::uint16_t> {
                const auto name = tables->item_name(wire);
                return name ? gameplay::items::by_name(*name) : std::nullopt;
            };
            const auto wire_of = [tables](std::uint16_t item) -> std::optional<std::int32_t> {
                const auto* info = gameplay::items::info(item);
                return info ? tables->item_id(info->name) : std::nullopt;
            };
            game->container_of = [codec, item_of, ctx = component_context(bundle, out.registries)](const contracts::ComponentPatch& patch) {
                auto stacks = codec->container_of(patch, ctx, item_of);
                return stacks ? std::move(*stacks) : std::vector<contracts::ItemStack>{};
            };
            game->with_container = [codec, wire_of, ctx = component_context(bundle, out.registries)](
                                       const contracts::ComponentPatch& base, std::span<const contracts::ItemStack> stacks) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_container(base, stacks, ctx, wire_of);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            // Decorated pots' decorations: the version's item ids too.
            game->pot_decorations = [codec, wire_of, ctx = component_context(bundle, out.registries)](
                                        std::span<const std::uint16_t, 4> items) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->pot_decorations(items, ctx, wire_of);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
        }
        game->power_providers = blocks_of("minecraft:enchantment_power_provider");
        game->power_transmitters = blocks_of("minecraft:enchantment_power_transmitter");
        for (std::uint16_t item = 1; const auto* info = gameplay::items::info(item); ++item) {
            std::string_view n = info->name;
            n.remove_prefix(10);
            const auto tool = n.ends_with("_sword") || n.ends_with("_shovel") || n.ends_with("_pickaxe") || n.ends_with("_axe") || n.ends_with("_hoe");
            const auto armor = n.ends_with("_helmet") || n.ends_with("_chestplate") || n.ends_with("_leggings") || n.ends_with("_boots");
            std::string tag;
            for (const auto& [material, tools, armors] : std::initializer_list<std::tuple<std::string_view, std::string_view, std::string_view>>{
                     {"wooden_", "wooden_tool_materials", ""}, {"stone_", "stone_tool_materials", ""},
                     {"iron_", "iron_tool_materials", "repairs_iron_armor"}, {"golden_", "gold_tool_materials", "repairs_gold_armor"},
                     {"diamond_", "diamond_tool_materials", "repairs_diamond_armor"},
                     {"netherite_", "netherite_tool_materials", "repairs_netherite_armor"}, {"leather_", "", "repairs_leather_armor"},
                     {"chainmail_", "", "repairs_chain_armor"}})
                if (n.starts_with(material)) tag = std::string(tool ? tools : armor ? armors : "");
            if (n == "turtle_helmet") tag = "repairs_turtle_helmet";
            if (n == "wolf_armor") tag = "repairs_wolf_armor";
            if (n == "shield") tag = "wooden_tool_materials";
            std::vector<std::uint16_t> materials;
            if (!tag.empty()) {
                if (const auto* members = packs->tag("item", "minecraft:" + tag))
                    for (const auto& member : *members)
                        if (const auto id = gameplay::items::by_name(member)) materials.push_back(*id);
            } else if (n == "elytra") {
                materials.push_back(gameplay::items::phantom_membrane);
            } else if (n == "mace") {
                materials.push_back(gameplay::items::breeze_rod);
            }
            std::ranges::sort(materials);
            if (!materials.empty()) game->repair_materials.emplace(item, std::move(materials));
        }
        if (const auto* codec = protocol::ComponentCodec::for_protocol(bundle.schema.component_schema)) {
            game->repair_cost_of = [codec](const contracts::ComponentPatch& patch) { return codec->repair_cost_of(patch); };
            game->int_component = [codec](const contracts::ComponentPatch& patch, std::string_view component) { return codec->varint_of(patch, component); };
            game->with_int_component = [codec, ctx = component_context(bundle, out.registries)](
                                           const contracts::ComponentPatch& base, std::string_view component,
                                           std::optional<std::int32_t> value) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_varint(base, component, value, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->custom_name_of = [codec](const contracts::ComponentPatch& patch) { return codec->custom_name_of(patch); };
            game->banner_layers = [codec](const contracts::ComponentPatch& patch) { return codec->banner_layers(patch); };
            game->dyed_color_of = [codec](const contracts::ComponentPatch& patch) { return codec->dyed_color_of(patch); };
            game->with_dyed_color = [codec, ctx = component_context(bundle, out.registries)](
                                        const contracts::ComponentPatch& base, std::int32_t rgb) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_dyed_color(base, rgb, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->with_component_from = [codec](const contracts::ComponentPatch& base, const contracts::ComponentPatch& from,
                                                std::string_view name) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_component_from(base, from, name);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->firework_star = [codec, ctx = component_context(bundle, out.registries)](
                                      std::int32_t shape, std::span<const std::int32_t> colors, bool trail,
                                      bool twinkle) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->firework_star(shape, colors, trail, twinkle, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->with_fade_colors = [codec, ctx = component_context(bundle, out.registries)](
                                         const contracts::ComponentPatch& base, std::span<const std::int32_t> fades) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_fade_colors(base, fades, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->firework_rocket = [codec, ctx = component_context(bundle, out.registries)](
                                        std::int32_t flight, std::span<const contracts::ComponentPatch> stars) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->firework_rocket(flight, stars, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->with_book_pages = [codec, ctx = component_context(bundle, out.registries)](
                                        const contracts::ComponentPatch& base, std::span<const std::string> pages) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_book_pages(base, pages, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->book_generation = [codec](const contracts::ComponentPatch& patch) { return codec->book_generation(patch); };
            game->with_book_generation = [codec, ctx = component_context(bundle, out.registries)](
                                             const contracts::ComponentPatch& base, std::int32_t generation) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_book_generation(base, generation, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->signed_book = [codec, ctx = component_context(bundle, out.registries)](
                                    const contracts::ComponentPatch& base, std::span<const std::string> pages, std::string_view title,
                                    std::string_view author) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->signed_book(base, pages, title, author, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
            game->with_custom_name = [codec, ctx = component_context(bundle, out.registries)](
                                         const contracts::ComponentPatch& base, const std::optional<std::string>& name) -> std::optional<contracts::ComponentPatch> {
                auto patch = codec->with_custom_name(base, name, ctx);
                return patch ? std::optional(std::move(*patch)) : std::nullopt;
            };
        }
        // Names as a player sees them, in English: what an anvil compares a
        // typed name with. A custom name's text; else a potion's name (its
        // potion without long_ or strong_); else the item's, from the jar's
        // language file (or its id in title case without one).
        {
            const auto& language = packs->language();
            std::unordered_map<std::uint16_t, std::string> names;
            for (std::uint16_t item = 1; const auto* info = gameplay::items::info(item); ++item) {
                const auto id = info->name.substr(10);
                std::string name;
                for (const auto* prefix : {"item.minecraft.", "block.minecraft."})
                    if (const auto it = language.find(prefix + std::string(id)); name.empty() && it != language.end()) name = it->second;
                if (name.empty()) {
                    bool start = true;
                    for (const char ch : id) {
                        name += ch == '_' ? ' ' : start ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : ch;
                        start = ch == '_';
                    }
                }
                names.emplace(item, std::move(name));
            }
            std::unordered_map<std::string, std::string> potion_names;
            for (const auto& [key, text] : language)
                if (key.find(".effect.") != std::string::npos && key.starts_with("item.minecraft.")) potion_names.emplace(key, text);
            game->display_name = [codec = protocol::ComponentCodec::for_protocol(bundle.schema.component_schema), names = std::move(names),
                                  potion_names = std::move(potion_names)](const contracts::ItemStack& stack) -> std::string {
                if (codec)
                    if (auto custom = codec->custom_name_of(stack.components)) return std::move(*custom);
                const auto item = gameplay::items::info(stack.item);
                if (codec && item
                    && (stack.item == gameplay::items::potion || stack.item == gameplay::items::splash_potion
                        || stack.item == gameplay::items::lingering_potion || stack.item == gameplay::items::tipped_arrow)) {
                    const auto id = codec->potion_of(stack.components);
                    const auto all = gameplay::potions::names();
                    std::string potion = id && *id < all.size() ? std::string(all[*id].substr(10)) : "empty";
                    for (const std::string_view prefix : {"long_", "strong_"})
                        if (potion.starts_with(prefix)) potion.erase(0, prefix.size());
                    const auto key = std::format("item.minecraft.{}.effect.{}", item->name.substr(10), potion);
                    if (const auto it = potion_names.find(key); it != potion_names.end()) return it->second;
                }
                const auto it = names.find(stack.item);
                return it == names.end() ? std::string{} : it->second;
            };
        }
        if (const auto* curses = packs->tag("enchantment", "minecraft:curse")) {
            const auto ctx = component_context(bundle, out.registries);
            for (const auto& name : *curses)
                if (const auto id = ctx.id_of("enchantment", name)) game->curses.push_back(*id);
            std::ranges::sort(game->curses);
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
