// SPDX-License-Identifier: AGPL-3.0-only
#include "loot_compiler.hpp"
#include <circuit/auth.hpp>
#include <circuit/auth/json.hpp>
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/items.hpp>
#include <circuit/protocol/content.hpp>
#include <charconv>
#include <cmath>
#include <format>

namespace circuit::server {
namespace {
using auth::Json;
namespace loot = gameplay::loot;

// A table outside the deterministic subset; the reason names the part.
struct Refused {
    std::string reason;
};

std::string with_namespace(std::string_view id) {
    return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}

class Compiler {
public:
    Compiler(const data::DataPacks& packs, const protocol::ContentTables& tables, const std::vector<RegistryEntries>& registries,
             std::string_view release)
        : packs_(packs), tables_(tables), registries_(registries), release_(release) {}

    // The table of `block` ("minecraft:stone").
    loot::Table table(const Json& j, std::string block) {
        block_ = std::move(block);
        loot::Table t;
        expect_type(j, "minecraft:block");
        for (const auto& p : array(j, "pools")) t.pools.push_back(pool(p));
        t.functions = functions(j);
        // Rolls draw from the table's random sequence (random_sequence.hpp).
        if (const auto* sequence = j.string("random_sequence")) {
            t.sequence = with_namespace(*sequence);
            const auto digest = auth::md5(t.sequence);
            for (int half = 0; half < 2; ++half)
                for (int i = 0; i < 8; ++i) t.sequence_hash[half] = t.sequence_hash[half] << 8 | digest[half * 8 + i];
        }
        return t;
    }

private:
    [[noreturn]] static void refuse(std::string reason) { throw Refused{std::move(reason)}; }

    static void expect_type(const Json& j, std::string_view type) {
        if (const auto* t = j.string("type"); t && *t != type) refuse(std::format("table type {}", *t));
    }
    static const std::vector<Json>& array(const Json& j, std::string_view key) {
        static const std::vector<Json> none;
        const auto* a = j.find(key);
        if (!a) return none;
        if (a->kind() != Json::Kind::array) refuse(std::format("{} is not a list", key));
        return a->items();
    }
    // A number provider that must be constant.
    static double constant(const Json& j, std::string_view what) {
        if (j.kind() == Json::Kind::number) return j.as_number();
        if (j.kind() == Json::Kind::object) {
            const auto* type = j.string("type");
            const auto* value = j.find("value");
            if ((!type || *type == "minecraft:constant") && value && value->kind() == Json::Kind::number) return value->as_number();
            refuse(std::format("random {}", what));
        }
        refuse(std::format("invalid {}", what));
    }

    // A count: constant, uniform between constant bounds, or binomial with
    // constant n and p.
    static loot::Number number(const Json& j, std::string_view what) {
        loot::Number n;
        if (j.kind() == Json::Kind::object) {
            const auto* type = j.string("type");
            if (type && *type == "minecraft:uniform") {
                const auto* min = j.find("min");
                const auto* max = j.find("max");
                if (!min || !max) refuse(std::format("uniform {} without bounds", what));
                n.kind = loot::Number::Kind::uniform;
                n.min = constant(*min, what);
                n.max = constant(*max, what);
                return n;
            }
            if (type && *type == "minecraft:binomial") {
                const auto* trials = j.find("n");
                const auto* p = j.find("p");
                if (!trials || !p) refuse(std::format("binomial {} without n and p", what));
                n.kind = loot::Number::Kind::binomial;
                n.n = std::lround(constant(*trials, what));
                n.p = constant(*p, what);
                return n;
            }
        }
        n.value = constant(j, what);
        return n;
    }

    // A bound of a state property range: an integer, as a number or a string.
    static long bound(const Json& j) {
        if (j.kind() == Json::Kind::number) return static_cast<long>(j.as_number());
        long v{};
        const auto& text = j.kind() == Json::Kind::string ? j.as_string() : std::string{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), v);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size()) refuse("block state property range");
        return v;
    }

    loot::Pool pool(const Json& j) {
        const auto* rolls = j.find("rolls");
        if (!rolls) refuse("pool without rolls");
        if (const auto* bonus = j.find("bonus_rolls"); bonus && constant(*bonus, "bonus rolls") != 0.0) refuse("bonus rolls");
        loot::Pool p;
        p.rolls = std::lround(constant(*rolls, "rolls"));
        for (const auto& e : array(j, "entries")) p.entries.push_back(entry(e));
        p.conditions = conditions(j);
        p.functions = functions(j);
        return p;
    }

    loot::Entry entry(const Json& j) {
        const auto* type = j.string("type");
        if (!type) refuse("entry without a type");
        loot::Entry e;
        e.conditions = conditions(j);
        e.functions = functions(j);
        if (*type == "minecraft:item") {
            e.kind = loot::Entry::Kind::item;
            const auto* name = j.string("name");
            if (!name) refuse("item entry without a name");
            e.item = item(*name);
        } else if (*type == "minecraft:empty") {
            e.kind = loot::Entry::Kind::empty;
        } else if (*type == "minecraft:alternatives" || *type == "minecraft:group" || *type == "minecraft:sequence") {
            e.kind = *type == "minecraft:alternatives" ? loot::Entry::Kind::alternatives
                   : *type == "minecraft:group"        ? loot::Entry::Kind::group
                                                       : loot::Entry::Kind::sequence;
            for (const auto& c : array(j, "children")) e.children.push_back(entry(c));
        } else {
            refuse(std::format("entry {}", *type));
        }
        return e;
    }

    std::vector<loot::Condition> conditions(const Json& j) {
        std::vector<loot::Condition> out;
        for (const auto& c : array(j, "conditions")) out.push_back(condition(c));
        return out;
    }

    loot::Condition condition(const Json& j) {
        const auto* kind = j.string("condition");
        if (!kind) refuse("condition without a kind");
        loot::Condition c;
        if (*kind == "minecraft:survives_explosion") {
            c.kind = loot::Condition::Kind::survives_explosion;
        } else if (*kind == "minecraft:block_state_property") {
            c.kind = loot::Condition::Kind::block_state;
            if (const auto* block = j.string("block"); block && with_namespace(*block) != block_) c.other_block = true;
            if (const auto* props = j.find("properties"))
                for (const auto& [name, value] : props->members()) {
                    loot::Condition::Property p{name, {}, {}, {}};
                    if (value.kind() == Json::Kind::string) p.exact = value.as_string();
                    else if (value.kind() == Json::Kind::object) {
                        if (const auto* min = value.find("min")) p.min = bound(*min);
                        if (const auto* max = value.find("max")) p.max = bound(*max);
                    } else refuse("block state property value");
                    c.properties.push_back(std::move(p));
                }
        } else if (*kind == "minecraft:match_tool") {
            c.kind = loot::Condition::Kind::tool;
            const auto* predicate = j.find("predicate");
            if (predicate) tool(*predicate, c);
        } else if (*kind == "minecraft:any_of" || *kind == "minecraft:all_of") {
            c.kind = *kind == "minecraft:any_of" ? loot::Condition::Kind::any_of : loot::Condition::Kind::all_of;
            for (const auto& t : array(j, "terms")) c.children.push_back(condition(t));
        } else if (*kind == "minecraft:random_chance") {
            c.kind = loot::Condition::Kind::random_chance;
            const auto* chance = j.find("chance");
            if (!chance) refuse("random_chance without a chance");
            c.chance = constant(*chance, "chance");
        } else if (*kind == "minecraft:location_check") {
            // The block at an offset (a tall plant's other half): its id and
            // exact state properties.
            c.kind = loot::Condition::Kind::location;
            const auto offset = [&](std::string_view key) {
                const auto* v = j.find(key);
                return v ? static_cast<std::int32_t>(std::lround(v->as_number())) : 0;
            };
            c.dx = offset("offsetX");
            c.dy = offset("offsetY");
            c.dz = offset("offsetZ");
            const auto* predicate = j.find("predicate");
            const auto* block = predicate ? predicate->find("block") : nullptr;
            if (!predicate || !block || predicate->members().size() != 1) refuse("location_check other than a block");
            for (const auto& [key, value] : block->members()) {
                if (key == "blocks" && value.kind() == Json::Kind::string && !value.as_string().starts_with('#')) {
                    const auto b = gameplay::registry::find_block(release_, with_namespace(value.as_string()));
                    if (!b) refuse("location_check of an unknown block");
                    c.block = *b;
                } else if (key == "state") {
                    for (const auto& [name, v] : value.members()) {
                        if (v.kind() != Json::Kind::string) refuse("location_check state range");
                        c.properties.push_back({name, v.as_string(), {}, {}});
                    }
                } else {
                    refuse(std::format("location_check block {}", key));
                }
            }
        } else if (*kind == "minecraft:inverted") {
            c.kind = loot::Condition::Kind::inverted;
            const auto* term = j.find("term");
            if (!term) refuse("inverted without a term");
            c.children.push_back(condition(*term));
        } else {
            refuse(std::format("condition {}", *kind));
        }
        return c;
    }

    // An item predicate: items (an id, a list or a tag) and enchantments.
    void tool(const Json& p, loot::Condition& c) {
        for (const auto& [key, value] : p.members()) {
            if (key == "items") {
                std::vector<std::string> names;
                if (value.kind() == Json::Kind::string) {
                    const auto& v = value.as_string();
                    if (v.starts_with('#')) {
                        const auto* tag = packs_.tag("item", v.substr(1));
                        if (!tag) refuse("unknown item tag");
                        names = *tag;
                    } else {
                        names.push_back(with_namespace(v));
                    }
                } else if (value.kind() == Json::Kind::array) {
                    for (const auto& v : value.items()) names.push_back(with_namespace(v.as_string()));
                } else {
                    refuse("item predicate items");
                }
                for (const auto& n : names) c.items.push_back(item(n));
            } else if (key == "predicates") {
                for (const auto& [component, list] : value.members()) {
                    if (component != "minecraft:enchantments" || list.kind() != Json::Kind::array) refuse(std::format("item predicate {}", component));
                    for (const auto& e : list.items()) {
                        // One enchantment by id; tags and lists are not supported yet.
                        const auto* name = e.string("enchantments");
                        if (!name || name->starts_with('#')) refuse("enchantment predicate without one enchantment");
                        loot::Condition::Enchantment want;
                        want.id = enchantment(*name);
                        if (const auto* levels = e.find("levels")) {
                            if (levels->kind() == Json::Kind::number) want.min = want.max = static_cast<long>(levels->as_number());
                            else {
                                if (const auto* min = levels->find("min")) want.min = static_cast<long>(min->as_number());
                                if (const auto* max = levels->find("max")) want.max = static_cast<long>(max->as_number());
                            }
                        }
                        c.enchantments.push_back(want);
                    }
                }
            } else {
                refuse(std::format("item predicate {}", key));
            }
        }
    }

    // An item of the version's registry.
    std::uint16_t item(std::string_view name) const {
        const auto full = with_namespace(name);
        const auto id = tables_.item_id(full) ? gameplay::items::by_name(full) : std::nullopt;
        if (!id) refuse("unknown item");
        return *id;
    }

    // The enchantment's id in the synchronized registry; without the registry,
    // an id no tool has.
    std::uint32_t enchantment(std::string_view name) const {
        if (const auto id = registry_index(registries_, "minecraft:enchantment", with_namespace(name))) return static_cast<std::uint32_t>(*id);
        return UINT32_MAX;
    }

    std::vector<loot::Function> functions(const Json& j) {
        std::vector<loot::Function> out;
        for (const auto& f : array(j, "functions")) {
            const auto* kind = f.string("function");
            if (!kind) refuse("function without a kind");
            loot::Function fn;
            fn.conditions = conditions(f);
            if (*kind == "minecraft:set_count") {
                fn.kind = loot::Function::Kind::set_count;
                const auto* count = f.find("count");
                if (!count) refuse("set_count without a count");
                fn.count = number(*count, "count");
                if (const auto* add = f.find("add")) fn.add = add->as_bool();
            } else if (*kind == "minecraft:limit_count") {
                fn.kind = loot::Function::Kind::limit_count;
                const auto* limit = f.find("limit");
                if (!limit) refuse("limit_count without a limit");
                if (limit->kind() == Json::Kind::number) fn.min = fn.max = static_cast<long>(limit->as_number());
                else {
                    if (const auto* min = limit->find("min")) fn.min = static_cast<long>(std::lround(constant(*min, "limit")));
                    if (const auto* max = limit->find("max")) fn.max = static_cast<long>(std::lround(constant(*max, "limit")));
                }
            } else if (*kind == "minecraft:explosion_decay") {
                fn.kind = loot::Function::Kind::explosion_decay;
            } else if (*kind == "minecraft:apply_bonus") {
                const auto* formula = f.string("formula");
                const auto* name = f.string("enchantment");
                if (!formula || !name) refuse("apply_bonus without a formula");
                const auto* parameters = f.find("parameters");
                const auto parameter = [&](std::string_view key) {
                    const auto* v = parameters ? parameters->find(key) : nullptr;
                    if (!v || v->kind() != Json::Kind::number) refuse(std::format("apply_bonus without {}", key));
                    return v->as_number();
                };
                if (*formula == "minecraft:ore_drops") {
                    fn.kind = loot::Function::Kind::ore_bonus;
                } else if (*formula == "minecraft:uniform_bonus_count") {
                    fn.kind = loot::Function::Kind::uniform_bonus;
                    fn.multiplier = std::lround(parameter("bonusMultiplier"));
                } else if (*formula == "minecraft:binomial_with_bonus_count") {
                    fn.kind = loot::Function::Kind::binomial_bonus;
                    fn.extra = std::lround(parameter("extra"));
                    fn.probability = parameter("probability");
                } else {
                    refuse(std::format("apply_bonus {}", *formula));
                }
                fn.enchantment = enchantment(*name);
            } else {
                refuse(std::format("function {}", *kind));
            }
            out.push_back(std::move(fn));
        }
        return out;
    }

    const data::DataPacks& packs_;
    const protocol::ContentTables& tables_;
    const std::vector<RegistryEntries>& registries_;
    std::string_view release_;
    std::string block_;
};
} // namespace

LootCompilation compile_block_loot(const data::DataPacks& packs, const contracts::VersionBundle& bundle,
                                   const std::vector<RegistryEntries>& registries) {
    namespace registry = gameplay::registry;
    LootCompilation out;
    const auto* tables = packs.resources("loot_table");
    const auto* content = protocol::ContentTables::for_protocol(bundle.protocol);
    if (!content) return out;
    const auto release = bundle.schema.content_release;
    Compiler compiler(packs, *content, registries, release);
    for (std::uint32_t b = 0; b < registry::block_count(); ++b) {
        // Blocks with rules of their own drop as their rules say, but wheat,
        // which drops by its table.
        if (gameplay::modeled_block(b) && registry::game_name(b) != "wheat") continue;
        const auto name = std::string(registry::game_name(b));
        if (registry::find_block(release, "minecraft:" + name) != b) continue;
        if (!tables) break;
        auto it = tables->find("minecraft:blocks/" + name);
        // A wall torch, sign, banner, head or coral fan drops as the standing one does.
        if (const auto wall = name.find("wall_"); it == tables->end() && wall != std::string::npos)
            it = tables->find("minecraft:blocks/" + name.substr(0, wall) + name.substr(wall + 5));
        if (it == tables->end()) continue; // no table: no drops
        ++out.blocks;
        const auto json = Json::parse(it->second);
        if (!json) {
            ++out.refused["invalid JSON"];
            continue;
        }
        try {
            out.tables.emplace(b, compiler.table(*json, "minecraft:" + name));
        } catch (const Refused& r) {
            ++out.refused[r.reason];
        }
    }
    return out;
}

} // namespace circuit::server
