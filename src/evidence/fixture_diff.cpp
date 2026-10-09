// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/evidence/trace_format.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <tuple>

namespace circuit::evidence {
namespace {
// One observation in a form both recordings can be compared in, with its line.
struct Item {
    std::string text;  // what is compared, apart from doubles
    std::vector<double> numbers;
    std::size_t line{};
};

std::string normalized_state(const std::string& text) {
    auto spec = parse_state(text);
    if (!spec) return text;
    std::ranges::sort(spec->properties);
    std::string out = spec->id;
    if (!spec->properties.empty()) {
        out += '[';
        for (std::size_t i = 0; i < spec->properties.size(); ++i)
            out += std::format("{}{}={}", i ? "," : "", spec->properties[i].first, spec->properties[i].second);
        out += ']';
    }
    return out;
}
std::string sorted_properties(Properties props) {
    std::ranges::sort(props);
    std::string out;
    for (const auto& [k, v] : props) out += std::format(" {}={}", k, v);
    return out;
}
std::vector<double> numbers(const Vec3d& a, const Vec3d& b) { return {a[0], a[1], a[2], b[0], b[1], b[2]}; }
std::string numbers_text(const std::vector<double>& n) {
    std::string out;
    for (const auto v : n) out += " " + format_double(v);
    return out;
}

// A tick's events in the order they happened.
std::vector<Item> events(const Fixture& f, Tick t) {
    std::vector<std::pair<std::size_t, Item>> out;
    for (const auto& c : f.recorded_changes)
        if (c.tick == t)
            out.push_back({f.format >= 2 ? c.seq : c.line, {std::format("change {} ({},{},{}) {} -> {}", c.stage, c.pos.x, c.pos.y, c.pos.z,
                                                                        normalized_state(c.before), normalized_state(c.after)),
                                                            {}, c.line}});
    for (const auto& e : f.entity_events) {
        if (e.tick != t) continue;
        switch (e.kind) {
        case EntityEvent::Kind::added:
            out.push_back({e.seq, {std::format("entity-added {} {} {}{}", e.stage, e.handle, e.type, sorted_properties(e.properties)),
                                   numbers(e.position, e.velocity), e.line}});
            break;
        case EntityEvent::Kind::removed:
            out.push_back({e.seq, {std::format("entity-removed {} {} {}", e.stage, e.handle, e.reason), {}, e.line}});
            break;
        case EntityEvent::Kind::changed:
            out.push_back({e.seq, {std::format("entity-changed {} {}{}", e.stage, e.handle, sorted_properties(e.properties)), {}, e.line}});
            break;
        }
    }
    for (const auto& c : f.chunk_events)
        if (c.tick == t) out.push_back({c.seq, {std::format("chunk {} ({},{}) {}", c.stage, c.x, c.z, c.status), {}, c.line}});
    std::ranges::stable_sort(out, {}, &std::pair<std::size_t, Item>::first);
    std::vector<Item> items;
    for (auto& [k, i] : out) items.push_back(std::move(i));
    return items;
}

// A tick's other observations: draws, entity orders and probes, in a fixed order.
std::vector<Item> observations(const Fixture& f, Tick t) {
    std::vector<Item> out;
    for (const auto& r : f.rng)
        if (r.tick == t) out.push_back({std::format("draws {} {} {}", r.stage, r.stream, r.draws), {}, r.line});
    for (const auto& o : f.entity_orders)
        if (o.tick == t) {
            std::string text = "entity-order " + o.stage;
            for (const auto& h : o.handles) text += " " + h;
            out.push_back({text, {}, o.line});
        }
    std::vector<const BlockProbe*> blocks;
    for (const auto& p : f.block_probes)
        if (p.tick == t) blocks.push_back(&p);
    std::ranges::stable_sort(blocks, {}, [](const BlockProbe* p) { return p->pos; });
    for (const auto* p : blocks)
        out.push_back({std::format("block ({},{},{}) {}", p->pos.x, p->pos.y, p->pos.z, normalized_state(p->state.value.value_or("?"))), {}, p->line});
    std::vector<const SlotProbe*> slots;
    for (const auto& p : f.slot_probes)
        if (p.tick == t) slots.push_back(&p);
    std::ranges::stable_sort(slots, {}, [](const SlotProbe* p) { return std::tie(p->pos, p->slot); });
    for (const auto* p : slots)
        out.push_back({std::format("slot ({},{},{}) {} {} {}", p->pos.x, p->pos.y, p->pos.z, p->slot, p->item.value.value_or("?"), p->count), {}, p->line});
    std::vector<const EntityProbe*> entities;
    for (const auto& p : f.entity_probes)
        if (p.tick == t) entities.push_back(&p);
    std::ranges::stable_sort(entities, {}, [](const EntityProbe* p) { return std::stoul(p->handle.substr(1)); });
    for (const auto* p : entities)
        out.push_back({std::format("entity {}{}", p->handle, sorted_properties(p->properties)), numbers(p->position, p->velocity), p->line});
    return out;
}

bool same(const Item& a, const Item& b, double tolerance) {
    if (a.text != b.text || a.numbers.size() != b.numbers.size()) return false;
    for (std::size_t i = 0; i < a.numbers.size(); ++i)
        if (tolerance == 0.0 ? a.numbers[i] != b.numbers[i] : std::abs(a.numbers[i] - b.numbers[i]) > tolerance) return false;
    return true;
}
std::string item_text(const Item& i) { return i.text + numbers_text(i.numbers); }

std::optional<std::string> compare(Tick t, std::string_view what, const std::vector<Item>& a, const std::vector<Item>& b, double tolerance) {
    const auto n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (!same(a[i], b[i], tolerance))
            return std::format("tick {} {} {} (line {} / line {}): `{}` / `{}`", t, what, i, a[i].line, b[i].line, item_text(a[i]), item_text(b[i]));
    if (a.size() != b.size()) {
        const auto& extra = a.size() > n ? a[n] : b[n];
        return std::format("tick {} {}: {} / {} entries; first unmatched (line {} in the {} file): `{}`", t, what, a.size(), b.size(), extra.line,
                           a.size() > n ? "first" : "second", item_text(extra));
    }
    return std::nullopt;
}
} // namespace

std::optional<std::string> first_difference(const Fixture& a, const Fixture& b, double tolerance) {
    const auto actions = [](const Fixture& f) {
        std::vector<std::tuple<Tick, int, BlockPos, std::string, int, std::string, int, Vec3d, Vec3d>> out;
        for (const auto& x : f.actions)
            out.emplace_back(x.tick, static_cast<int>(x.kind), x.pos, normalized_state(x.state), x.slot, x.item, x.count, x.position, x.velocity);
        return out;
    };
    if (a.area_min != b.area_min || a.area_max != b.area_max || a.ticks != b.ticks) return "different scenarios: area or ticks";
    if (actions(a) != actions(b)) return "different scenarios: actions";
    if (a.changes != b.changes || a.format != b.format || a.observe_entities != b.observe_entities || a.observe_chunks != b.observe_chunks
        || a.observe_rng != b.observe_rng || a.dimension != b.dimension || a.random_tick_speed != b.random_tick_speed
        || a.neighbor_update_limit != b.neighbor_update_limit || a.gamerules != b.gamerules)
        return "different scenarios: observation headers";
    for (Tick t = 0; t <= a.ticks; ++t) {
        if (auto d = compare(t, "event", events(a, t), events(b, t), tolerance)) return d;
        if (auto d = compare(t, "observation", observations(a, t), observations(b, t), tolerance)) return d;
    }
    return std::nullopt;
}

} // namespace circuit::evidence
