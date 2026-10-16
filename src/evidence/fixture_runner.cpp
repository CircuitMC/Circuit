// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/evidence/content.hpp>
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/runtime/engine.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <variant>

namespace circuit::evidence {
using contracts::Command;
using contracts::TraceKind;
using gameplay::BlockState;
using Outcome = FixtureReport::Outcome;

namespace {
class EngineExecutor final : public FixtureExecutor {
public:
    EngineExecutor(const contracts::VersionBundle& bundle, std::uint64_t seed, unsigned workers)
        : engine_(bundle, seed, [workers] {
              runtime::EngineOptions o;
              o.workers = workers;
              o.level_random_seed = 0; // as the reference harness seeds the overworld's
              return o;
          }()) {}
    void advance(const std::vector<Command>& commands) override { engine_.advance(commands); }
    const std::vector<contracts::TraceRecord>& trace() const override { return engine_.trace(); }
    // Fixtures run in the overworld (the runner refuses other dimensions).
    BlockState block(BlockPos pos) const override {
        const auto s = engine_.view().block(contracts::DimensionId::overworld, pos);
        return s ? *s : BlockState::unloaded_sentinel();
    }
    std::optional<contracts::ItemStack> slot(BlockPos pos, std::uint8_t slot) const override {
        const auto be = engine_.view().block_entity(contracts::DimensionId::overworld, pos);
        if (!be || !*be || slot >= (*be)->slots.size()) return std::nullopt;
        return (*be)->slots[slot];
    }
    std::vector<gameplay::Entity> entities() const override { return engine_.dump().overworld().entities; }
    std::optional<std::pair<std::uint8_t, std::uint8_t>> light(BlockPos pos) const override {
        return engine_.light(contracts::DimensionId::overworld, pos);
    }

private:
    runtime::Engine engine_;
};

struct MappedChange {
    BlockPos pos;
    BlockState before, after;
    std::string stage; // v2
    std::size_t line{};
    std::size_t seq{}; // v2: position among the tick's events
    bool operator==(const MappedChange& o) const {
        return pos == o.pos && before == o.before && after == o.after && stage == o.stage;
    }
};

// A fixture translated into commands and reference-registry values.
struct Prepared {
    contracts::VersionBundle bundle;
    const ContentMapping* mapping{};
    std::map<Tick, std::vector<Command>> inputs;            // by Circuit tick
    std::map<std::uint64_t, std::size_t> action_lines;      // admission -> fixture line
    std::map<Tick, std::vector<MappedChange>> changes;      // by fixture tick; no-ops after mapping dropped
    std::vector<std::pair<const BlockProbe*, BlockState>> blocks;
    std::vector<std::pair<const SlotProbe*, contracts::ItemStack>> slots;
};

FixtureReport report(Outcome outcome, std::string detail) { return {outcome, std::move(detail), 0, 0, 0}; }

Vec3d to_vec(contracts::Vec3 v) { return {v.x, v.y, v.z}; }

// The commands of a `forceload add|remove <x> <z> [<x2> <z2>]` or
// `forceload remove all` action: one forced ticket (level 31, radius 0) per
// chunk of the block coordinates' rectangle, as the target versions'
// command marks chunks. Chunks already in the wanted state are left alone.
std::expected<std::vector<contracts::CommandBody>, std::string> forceload(const std::string& command,
                                                                        std::map<contracts::ChunkPos, std::uint32_t>& forced,
                                                                        std::uint32_t& next_ticket) {
    std::vector<std::string> w;
    for (std::size_t i = 0; i <= command.size();) {
        const auto end = std::min(command.find(' ', i), command.size());
        w.push_back(command.substr(i, end - i));
        i = end + 1;
    }
    const auto unsupported = std::format("command `{}`: only forceload add and remove and gamerule randomTickSpeed are modeled", command);
    if (w.size() == 3 && w[0] == "gamerule" && w[1] == "randomTickSpeed") {
        std::int32_t v{};
        const auto [end, ec] = std::from_chars(w[2].data(), w[2].data() + w[2].size(), v);
        if (ec != std::errc{} || end != w[2].data() + w[2].size()) return std::unexpected(unsupported);
        return std::vector<contracts::CommandBody>{contracts::SetGameRule{w[1], v}};
    }
    if (w.size() < 2 || w[0] != "forceload" || (w[1] != "add" && w[1] != "remove")) return std::unexpected(unsupported);
    std::vector<contracts::CommandBody> out;
    if (w[1] == "remove" && w.size() == 3 && w[2] == "all") {
        for (const auto& [chunk, id] : forced) out.push_back(contracts::RemoveTicket{id});
        forced.clear();
        return out;
    }
    if (w.size() != 4 && w.size() != 6) return std::unexpected(unsupported);
    std::int32_t v[4]{};
    for (std::size_t i = 2; i < w.size(); ++i) {
        const auto& s = w[i];
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v[i - 2]);
        if (ec != std::errc{} || end != s.data() + s.size()) return std::unexpected(std::format("command `{}`: invalid coordinate", command));
    }
    if (w.size() == 4) v[2] = v[0], v[3] = v[1];
    const auto a = contracts::chunk_of({v[0], 0, v[1]}), b = contracts::chunk_of({v[2], 0, v[3]});
    const std::int64_t count = (std::int64_t{std::max(a.x, b.x)} - std::min(a.x, b.x) + 1) * (std::int64_t{std::max(a.z, b.z)} - std::min(a.z, b.z) + 1);
    if (count > 256) return std::unexpected(std::format("command `{}`: more than 256 chunks", command));
    for (auto x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        for (auto z = std::min(a.z, b.z); z <= std::max(a.z, b.z); ++z) {
            const contracts::ChunkPos chunk{x, z};
            if (w[1] == "add" && !forced.contains(chunk)) {
                forced[chunk] = next_ticket;
                out.push_back(contracts::AddTicket{next_ticket++, chunk, 0, contracts::TicketType::forced, 31});
            } else if (w[1] == "remove" && forced.contains(chunk)) {
                out.push_back(contracts::RemoveTicket{forced.at(chunk)});
                forced.erase(chunk);
            }
        }
    return out;
}

std::expected<Prepared, FixtureReport> prepare(const Fixture& f, bool require_observations) {
    Prepared p;
    const contracts::VersionBundle* found = nullptr;
    for (const auto& b : contracts::BundleRegistry::candidates())
        if (b.game_version == f.game_version && b.rules) found = &b;
    if (!found) return std::unexpected(report(Outcome::unsupported, std::format("no executable bundle for {}", f.game_version)));
    p.mapping = ContentMapping::for_game_version(found->schema.content_release);
    if (!p.mapping) return std::unexpected(report(Outcome::unsupported, std::format("no content mapping for {}", f.game_version)));
    // A game rule the version lacks makes the scenario invalid there; a rule
    // at its default changes nothing; any other value is not modeled.
    for (const auto& [name, value] : f.gamerules) {
        const auto* rule = found->schema.game_rule(name);
        if (!rule || rule->experimental)
            return std::unexpected(report(Outcome::invalid, std::format("game rule {} does not exist in {}", name, f.game_version)));
        const auto expected = rule->type == contracts::GameRuleType::boolean ? std::string(rule->default_value ? "true" : "false")
                                                                             : std::to_string(rule->default_value);
        // randomTickSpeed is world state the rules read; other rules are
        // modeled only at their defaults.
        if (name == "randomTickSpeed") continue;
        if (value != expected)
            return std::unexpected(report(Outcome::unsupported, std::format("game rule {}: the rules model only its default, {}", name, expected)));
    }
    if (f.dimension != "minecraft:overworld")
        return std::unexpected(report(Outcome::unsupported, std::format("dimension {}: the rules model the overworld only", f.dimension)));
    p.bundle = *found;
    if (f.neighbor_update_limit != 0) p.bundle.limits.max_chained_neighbor_updates = f.neighbor_update_limit;
    for (const auto& m : f.mechanisms) {
        const auto& table = p.bundle.rules->mechanisms;
        if (std::ranges::none_of(table, [&](const auto& e) { return e.id == m; }))
            return std::unexpected(report(Outcome::invalid, std::format("unknown mechanism `{}`", m)));
    }
    if (!p.bundle.world.contains_y(f.area_min.y) || !p.bundle.world.contains_y(f.area_max.y))
        return std::unexpected(report(Outcome::unsupported, "area outside the world height"));
    const auto lo = contracts::chunk_of(f.area_min), hi = contracts::chunk_of(f.area_max);
    const contracts::ChunkPos center{lo.x + (hi.x - lo.x) / 2, lo.z + (hi.z - lo.z) / 2};
    const auto radius = std::max({center.x - lo.x, hi.x - center.x, center.z - lo.z, hi.z - center.z}) + 1;
    if (radius > p.bundle.limits.max_ticket_radius) return std::unexpected(report(Outcome::unsupported, "area too large to load"));

    std::uint64_t admission = 1;
    if (f.load_area) p.inputs[1].push_back({admission++, contracts::AddTicket{1, center, static_cast<std::uint8_t>(radius)}});
    // Random ticks are off unless the fixture sets their speed.
    p.inputs[1].push_back({admission++, contracts::SetGameRule{"randomTickSpeed", static_cast<std::int32_t>(f.random_tick_speed)}});
    for (const auto& [name, value] : f.gamerules)
        if (name == "randomTickSpeed") {
            std::int32_t v{};
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
            if (ec != std::errc{} || end != value.data() + value.size())
                return std::unexpected(report(Outcome::invalid, "game rule randomTickSpeed needs an integer"));
            p.inputs[1].push_back({admission++, contracts::SetGameRule{name, v}});
        }
    const auto fail_line = [](std::size_t line, const std::string& message) {
        return std::unexpected(report(Outcome::unsupported, std::format("line {}: {}", line, message)));
    };
    // Force-loaded chunks of `forceload` commands, by chunk: their ticket ids.
    std::map<contracts::ChunkPos, std::uint32_t> forced;
    std::uint32_t next_ticket = 2;
    for (const auto& a : f.actions) {
        if (a.kind == Action::Kind::command) {
            auto bodies = forceload(a.command, forced, next_ticket);
            if (!bodies) return fail_line(a.line, bodies.error());
            for (auto& body : *bodies) {
                p.action_lines[admission] = a.line;
                p.inputs[a.tick + 2].push_back({admission++, std::move(body)});
            }
            continue;
        }
        contracts::CommandBody body;
        switch (a.kind) {
        case Action::Kind::command: break;
        case Action::Kind::set: {
            const auto s = p.mapping->block(a.state);
            if (!s) return fail_line(a.line, s.error());
            body = contracts::SetBlock{a.pos, s->raw(), std::nullopt};
            break;
        }
        case Action::Kind::use: body = contracts::UseBlock{a.pos}; break;
        case Action::Kind::insert: {
            const auto item = p.mapping->item(a.item);
            if (!item) return fail_line(a.line, item.error());
            body = contracts::InsertItem{a.pos, a.slot, {*item, a.count}};
            break;
        }
        case Action::Kind::spawn_item: {
            const auto item = p.mapping->item(a.item);
            if (!item) return fail_line(a.line, item.error());
            const contracts::Vec3 position{a.position[0], a.position[1], a.position[2]};
            const contracts::Vec3 velocity{a.velocity[0], a.velocity[1], a.velocity[2]};
            body = contracts::SpawnItem{position, velocity, {*item, a.count}};
            break;
        }
        }
        p.action_lines[admission] = a.line;
        p.inputs[a.tick + 2].push_back({admission++, std::move(body)});
    }
    if (!require_observations) return p;
    if (f.is_skeleton()) return std::unexpected(report(Outcome::invalid, "skeleton: record observations before evaluating"));
    for (const auto& c : f.recorded_changes) {
        const auto before = p.mapping->block(c.before);
        if (!before) return fail_line(c.line, before.error());
        const auto after = p.mapping->block(c.after);
        if (!after) return fail_line(c.line, after.error());
        // Changes of properties the reference rules do not model are not observable here.
        if (*before != *after) p.changes[c.tick].push_back({c.pos, *before, *after, c.stage, c.line, c.seq});
    }
    for (const auto& probe : f.block_probes) {
        const auto s = p.mapping->block(*probe.state.value);
        if (!s) return fail_line(probe.line, s.error());
        p.blocks.push_back({&probe, *s});
    }
    for (const auto& probe : f.slot_probes) {
        contracts::ItemStack stack{};
        if (*probe.item.value != "empty") {
            const auto item = p.mapping->item(*probe.item.value);
            if (!item) return fail_line(probe.line, item.error());
            stack = {*item, probe.count};
        }
        p.slots.push_back({&probe, stack});
    }
    return p;
}

// Executes the prepared inputs and calls `observe(fixture_tick)` after every
// tick from the setup tick on. Rejected actions make the fixture unsupported.
std::optional<FixtureReport> execute(const Fixture& f, const Prepared& p, FixtureExecutor& exec,
                                     const std::function<std::optional<FixtureReport>(Tick)>& observe) {
    static const std::vector<Command> none;
    for (Tick c = 1; c <= f.ticks + 2; ++c) {
        const auto it = p.inputs.find(c);
        exec.advance(it == p.inputs.end() ? none : it->second);
        for (const auto& r : exec.trace()) {
            if (r.kind != TraceKind::command_rejected) continue;
            const auto line = p.action_lines.contains(r.a) ? p.action_lines.at(r.a) : 0;
            return report(Outcome::unsupported,
                          std::format("line {}: action rejected by the reference rules ({})", line,
                                      contracts::error_name(static_cast<contracts::ErrorCode>(r.b))));
        }
        if (c >= 2)
            if (auto r = observe(c - 2)) return r;
    }
    return std::nullopt;
}

std::string stage_of(const contracts::TraceRecord& r) { return std::string(contracts::phase_name(r.key.phase)); }

// Raw draws from the level random in one tick's trace, per stage.
std::map<std::string, std::uint64_t> level_draws(const std::vector<contracts::TraceRecord>& trace) {
    std::map<std::string, std::uint64_t> out;
    for (const auto& r : trace)
        if (r.kind == TraceKind::level_random_draw && r.a != 0) out[stage_of(r)] += r.a;
    return out;
}

std::vector<MappedChange> area_changes(const Fixture& f, const std::vector<contracts::TraceRecord>& trace) {
    std::vector<MappedChange> out;
    for (const auto& r : trace)
        if (r.kind == TraceKind::block_changed && f.contains(r.pos))
            out.push_back({r.pos, BlockState::decode(static_cast<std::uint32_t>(r.a)).value(),
                           BlockState::decode(static_cast<std::uint32_t>(r.b)).value(), f.format >= 2 ? stage_of(r) : "", 0, 0});
    return out;
}

std::string change_text(const ContentMapping& m, const MappedChange& c) {
    return std::format("{}({},{},{}) {} -> {}", c.stage.empty() ? "" : c.stage + " ", c.pos.x, c.pos.y, c.pos.z, m.notation(c.before),
                       m.notation(c.after));
}

// ---- circuit-trace 2 observations of Circuit ---------------------------------

// The target version's removal reasons for Circuit's.
std::string removal_reason(std::uint64_t reason) {
    switch (static_cast<gameplay::RemovalReason>(reason)) {
    case gameplay::RemovalReason::despawned:
    case gameplay::RemovalReason::fell_out_of_world:
    case gameplay::RemovalReason::picked_up: return "discarded";
    case gameplay::RemovalReason::command: return "killed";
    }
    return "discarded";
}

std::string chunk_status(std::uint64_t level) {
    switch (static_cast<gameplay::ChunkLevel>(level)) {
    case gameplay::ChunkLevel::entity_ticking: return "entity_ticking";
    case gameplay::ChunkLevel::block_ticking: return "block_ticking";
    case gameplay::ChunkLevel::border: return "border";
    case gameplay::ChunkLevel::unloaded: return "unloaded";
    }
    return "unloaded";
}

// Entities spawned inside the area are observed for their lifetime under
// handles e1, e2, ... in spawn order.
class EntityTracker {
public:
    explicit EntityTracker(const ContentMapping& mapping) : mapping_(mapping) {}

    // Appends the tick's events, in trace order and with increasing seq: area
    // changes (when `with_changes`), then entity and chunk events.
    void collect(const Fixture& f, Tick tick, const std::vector<contracts::TraceRecord>& trace, bool with_changes,
                 std::vector<MappedChange>& changes, std::vector<EntityEvent>& entities, std::vector<ChunkEvent>& chunks,
                 std::size_t& seq) {
        for (const auto& r : trace) {
            switch (r.kind) {
            case TraceKind::block_changed:
                if (with_changes && tick >= 1 && f.contains(r.pos))
                    changes.push_back({r.pos, BlockState::decode(static_cast<std::uint32_t>(r.a)).value(),
                                       BlockState::decode(static_cast<std::uint32_t>(r.b)).value(), stage_of(r), 0, seq++});
                break;
            case TraceKind::entity_spawned:
                spawned_ = r.a;
                if (f.observe_entities && f.contains(r.pos)) {
                    handles_[r.a] = std::format("e{}", ++count_);
                    stacks_[r.a] = static_cast<std::uint64_t>(r.b);
                    kinds_[r.a] = static_cast<gameplay::EntityKind>(r.c);
                }
                break;
            case TraceKind::entity_spawn_position:
                spawn_position_ = {contracts::bits_double(r.a), contracts::bits_double(r.b), contracts::bits_double(r.c)};
                break;
            case TraceKind::entity_spawn_velocity:
                // The spawn records follow their entity_spawned record.
                if (const auto it = handles_.find(spawned_); f.observe_entities && it != handles_.end() && !added_.contains(spawned_)) {
                    added_.insert(spawned_);
                    const contracts::Vec3 velocity{contracts::bits_double(r.a), contracts::bits_double(r.b), contracts::bits_double(r.c)};
                    entities.push_back({tick, stage_of(r), EntityEvent::Kind::added, it->second,
                                        std::string(gameplay::entity_kind_name(kinds_.at(spawned_))), to_vec(spawn_position_),
                                        to_vec(velocity), stack_properties(stacks_.at(spawned_)), "", seq++, 0});
                }
                break;
            case TraceKind::entity_removed:
                if (const auto it = handles_.find(r.a); f.observe_entities && it != handles_.end()) {
                    entities.push_back({tick, stage_of(r), EntityEvent::Kind::removed, it->second, "", {}, {}, {}, removal_reason(r.b), seq++, 0});
                    removed_[r.a] = r.key;
                }
                break;
            case TraceKind::entity_stack_changed:
                if (const auto it = handles_.find(r.a); f.observe_entities && it != handles_.end()) {
                    Properties props;
                    if (((r.b >> 8) & 0xFFFF) != ((r.c >> 8) & 0xFFFF)) props = stack_properties(r.c);
                    else props.emplace_back("count", std::to_string(r.c & 0xFF));
                    entities.push_back({tick, stage_of(r), EntityEvent::Kind::changed, it->second, "", {}, {}, std::move(props), "", seq++, 0});
                }
                break;
            case TraceKind::chunk_loaded: case TraceKind::chunk_level_changed: case TraceKind::chunk_unloaded:
                if (f.observe_chunks) {
                    const auto chunk = contracts::chunk_of(r.pos);
                    if (const auto& w = f.observe_window; w && (chunk.x < (*w)[0] || chunk.z < (*w)[1] || chunk.x > (*w)[2] || chunk.z > (*w)[3]))
                        break;
                    const auto status = r.kind == TraceKind::chunk_unloaded ? "unloaded" : chunk_status(r.b);
                    // The target versions report every status change in their own stage.
                    chunks.push_back({tick, "chunks", chunk.x, chunk.z, status, seq++, 0});
                }
                break;
            default: break;
            }
        }
    }

    // End-of-tick state of the observed live entities, by handle.
    std::vector<EntityProbe> probes(Tick tick, const std::vector<gameplay::Entity>& live) const {
        std::vector<std::pair<std::size_t, EntityProbe>> out;
        for (const auto& e : live) {
            const auto it = handles_.find(e.id.value);
            if (it == handles_.end()) continue;
            Properties props = stack_properties(contracts::pack_stack(e.stack));
            props.emplace_back("age", std::to_string(e.age));
            props.emplace_back("pickup-delay", std::to_string(e.pickup_delay));
            props.emplace_back("on-ground", e.on_ground ? "true" : "false");
            out.push_back({number(it->second), {tick, it->second, to_vec(e.position), to_vec(e.velocity), std::move(props), 0}});
        }
        std::ranges::sort(out, {}, &std::pair<std::size_t, EntityProbe>::first);
        std::vector<EntityProbe> result;
        for (auto& [n, p] : out) result.push_back(std::move(p));
        return result;
    }

    // The observed entities that ticked in the entity phase of Circuit tick
    // `circuit_tick`, in Circuit's order (ascending id), including those
    // removed during their own tick.
    std::optional<EntityOrder> order(Tick tick, Tick circuit_tick, const std::vector<gameplay::Entity>& live) const {
        std::vector<std::uint64_t> ids;
        for (const auto& e : live)
            if (handles_.contains(e.id.value) && e.last_tick == circuit_tick) ids.push_back(e.id.value);
        // Removed in or after the entity phase: it ticked first.
        for (const auto& [id, key] : removed_)
            if (key.tick == circuit_tick && key.phase >= contracts::Phase::entities) ids.push_back(id);
        if (ids.empty()) return std::nullopt;
        std::ranges::sort(ids);
        EntityOrder o{tick, "entities", {}, 0};
        for (const auto id : ids) o.handles.push_back(handles_.at(id));
        return o;
    }

private:
    Properties stack_properties(std::uint64_t packed) const {
        const auto item = static_cast<std::uint16_t>((packed >> 8) & 0xFFFF);
        Properties props{{"item", mapping_.item_name(item)}, {"count", std::to_string(packed & 0xFF)}};
        if (const auto damage = (packed >> 24) & 0xFFFF; damage != 0) props.emplace_back("damage", std::to_string(damage));
        return props;
    }
    static std::size_t number(const std::string& handle) { return std::stoul(handle.substr(1)); }

    const ContentMapping& mapping_;
    std::map<std::uint64_t, std::string> handles_;
    std::map<std::uint64_t, std::uint64_t> stacks_;
    std::map<std::uint64_t, gameplay::EntityKind> kinds_;
    std::uint64_t spawned_{};         // the entity of the last entity_spawned record
    contracts::Vec3 spawn_position_{}; // from the entity_spawn_position record after it
    std::set<std::uint64_t> added_;
    std::map<std::uint64_t, contracts::OrderKey> removed_;
    std::size_t count_{};
};

bool close(double a, double b, double tolerance) { return tolerance == 0.0 ? a == b : std::abs(a - b) <= tolerance; }
bool close(const Vec3d& a, const Vec3d& b, double tolerance) {
    return close(a[0], b[0], tolerance) && close(a[1], b[1], tolerance) && close(a[2], b[2], tolerance);
}
bool same_properties(const Properties& a, const Properties& b) {
    auto x = a, y = b;
    std::ranges::sort(x);
    std::ranges::sort(y);
    return x == y;
}
std::string vec_text(const Vec3d& v) { return std::format("({}, {}, {})", format_double(v[0]), format_double(v[1]), format_double(v[2])); }
std::string properties_text(const Properties& props) {
    std::string out;
    for (const auto& [k, v] : props) out += std::format(" {}={}", k, v);
    return out;
}
std::string event_text(const EntityEvent& e) {
    switch (e.kind) {
    case EntityEvent::Kind::added:
        return std::format("{} {} added {} at {} moving {}{}", e.stage, e.handle, e.type, vec_text(e.position), vec_text(e.velocity),
                           properties_text(e.properties));
    case EntityEvent::Kind::removed: return std::format("{} {} removed ({})", e.stage, e.handle, e.reason);
    case EntityEvent::Kind::changed: return std::format("{} {} changed{}", e.stage, e.handle, properties_text(e.properties));
    }
    return "";
}
bool same_event(const EntityEvent& a, const EntityEvent& b, double tolerance) {
    if (a.kind != b.kind || a.stage != b.stage || a.handle != b.handle) return false;
    switch (a.kind) {
    case EntityEvent::Kind::added:
        return a.type == b.type && close(a.position, b.position, tolerance) && close(a.velocity, b.velocity, tolerance)
            && same_properties(a.properties, b.properties);
    case EntityEvent::Kind::removed: return a.reason == b.reason;
    case EntityEvent::Kind::changed: return same_properties(a.properties, b.properties);
    }
    return false;
}

// One event of a tick's ordered v2 stream.
using StreamEvent = std::variant<MappedChange, EntityEvent, ChunkEvent>;
std::string stream_text(const ContentMapping& m, const StreamEvent& e) {
    if (const auto* c = std::get_if<MappedChange>(&e)) return "change " + change_text(m, *c);
    if (const auto* x = std::get_if<EntityEvent>(&e)) return "entity " + event_text(*x);
    const auto& k = std::get<ChunkEvent>(e);
    return std::format("chunk {} ({}, {}) {}", k.stage, k.x, k.z, k.status);
}
bool same_stream(const StreamEvent& a, const StreamEvent& b, double tolerance) {
    if (a.index() != b.index()) return false;
    if (const auto* c = std::get_if<MappedChange>(&a)) return *c == std::get<MappedChange>(b);
    if (const auto* x = std::get_if<EntityEvent>(&a)) return same_event(*x, std::get<EntityEvent>(b), tolerance);
    const auto& k = std::get<ChunkEvent>(a);
    const auto& l = std::get<ChunkEvent>(b);
    return k.stage == l.stage && k.x == l.x && k.z == l.z && k.status == l.status;
}
std::size_t stream_line(const StreamEvent& e) {
    return std::visit([](const auto& v) { return v.line; }, e);
}
} // namespace

ExecutorFactory engine_executor(unsigned workers) {
    return [workers](const contracts::VersionBundle& bundle, std::uint64_t seed) -> std::unique_ptr<FixtureExecutor> {
        return std::make_unique<EngineExecutor>(bundle, seed, workers);
    };
}

std::string_view outcome_name(Outcome outcome) noexcept {
    switch (outcome) {
    case Outcome::passed: return "PASS";
    case Outcome::failed: return "FAIL";
    case Outcome::unsupported: return "UNSUPPORTED";
    case Outcome::invalid: return "INVALID";
    }
    return "INVALID";
}

std::expected<FixtureInputs, std::string> fixture_inputs(const Fixture& f) {
    auto prepared = prepare(f, false);
    if (!prepared) return std::unexpected(prepared.error().detail);
    return FixtureInputs{prepared->bundle, prepared->inputs};
}

FixtureReport evaluate_fixture(const Fixture& f, const ExecutorFactory& factory) {
    auto prepared = prepare(f, true);
    if (!prepared) return prepared.error();
    const auto& p = *prepared;
    auto exec = factory(p.bundle, 0);
    std::size_t changes = 0, probes = 0, events = 0;
    EntityTracker tracker(*p.mapping);
    const bool v2 = f.format >= 2;
    // In v2 with ordered changes, a tick's changes, entity events and chunk
    // events form one ordered stream; otherwise changes are compared on
    // their own as in v1.
    const bool merged = v2 && f.changes == ChangeMode::ordered;
    using ChunkKey2 = std::pair<std::int32_t, std::int32_t>;
    std::map<ChunkKey2, std::string> expected_chunk_status, observed_chunk_status; // statuses so far
    const auto observe = [&](Tick t) -> std::optional<FixtureReport> {
        std::vector<MappedChange> observed_changes;
        std::vector<EntityEvent> observed_entities;
        std::vector<ChunkEvent> observed_chunks;
        std::size_t seq = 0;
        if (v2) tracker.collect(f, t, exec->trace(), merged, observed_changes, observed_entities, observed_chunks, seq);
        if (f.changes != ChangeMode::none && t >= 1 && !merged) {
            auto actual = area_changes(f, exec->trace());
            const auto it = p.changes.find(t);
            auto expected = it == p.changes.end() ? std::vector<MappedChange>{} : it->second;
            if (f.changes == ChangeMode::unordered) {
                const auto key = [](const MappedChange& c) { return std::tuple(c.pos, c.before.raw(), c.after.raw(), c.stage); };
                std::ranges::sort(actual, {}, key);
                std::ranges::sort(expected, {}, key);
            }
            const auto n = std::min(actual.size(), expected.size());
            for (std::size_t i = 0; i < n; ++i)
                if (!(actual[i] == expected[i]))
                    return report(Outcome::failed, std::format("tick {} change {} (line {}): expected {}, observed {}", t, i,
                                                               expected[i].line, change_text(*p.mapping, expected[i]),
                                                               change_text(*p.mapping, actual[i])));
            if (actual.size() != expected.size())
                return report(Outcome::failed,
                              std::format("tick {}: expected {} changes, observed {}; first unmatched: {}", t, expected.size(),
                                          actual.size(),
                                          change_text(*p.mapping, actual.size() > n ? actual[n] : expected[n])));
            changes += expected.size();
        }
        if (v2) {
            // The tick's ordered stream: expected in file order, observed in trace order.
            std::vector<std::pair<std::size_t, StreamEvent>> expected, observed;
            if (merged)
                if (const auto it = p.changes.find(t); it != p.changes.end())
                    for (const auto& c : it->second) expected.push_back({c.seq, c});
            for (const auto& e : f.entity_events)
                if (e.tick == t) expected.push_back({e.seq, e});
            for (const auto& c : observed_changes) observed.push_back({c.seq, c});
            for (const auto& e : observed_entities) observed.push_back({e.seq, e});
            // Chunk statuses: the target versions report a tick's changes in
            // the order of their chunk maps and may pass a chunk through
            // several statuses within the tick, while nothing runs in
            // between. What is compared is each chunk whose status at the
            // end of the tick differs from its status before it.
            const auto changed = [](std::map<ChunkKey2, std::string>& now, const std::vector<std::pair<ChunkKey2, std::string>>& events) {
                std::map<ChunkKey2, std::string> before, out;
                for (const auto& [chunk, status] : events) {
                    if (!before.contains(chunk)) before[chunk] = now.contains(chunk) ? now[chunk] : "unloaded";
                    now[chunk] = status;
                }
                for (const auto& [chunk, was] : before)
                    if (now[chunk] != was) out[chunk] = now[chunk];
                return out;
            };
            std::vector<std::pair<ChunkKey2, std::string>> expected_events, observed_events;
            std::map<ChunkKey2, std::size_t> lines;
            for (const auto& c : f.chunk_events)
                if (c.tick == t) {
                    expected_events.push_back({{c.x, c.z}, c.status});
                    lines[{c.x, c.z}] = c.line;
                }
            for (const auto& c : observed_chunks) observed_events.push_back({{c.x, c.z}, c.status});
            const auto expected_chunks = changed(expected_chunk_status, expected_events);
            const auto observed_chunk_changes = changed(observed_chunk_status, observed_events);
            for (const auto& [chunk, status] : expected_chunks) {
                const auto it = observed_chunk_changes.find(chunk);
                if (it == observed_chunk_changes.end() || it->second != status)
                    return report(Outcome::failed, std::format("tick {} chunk ({}, {}) (line {}): expected {}, observed {}", t, chunk.first,
                                                               chunk.second, lines[chunk], status,
                                                               it == observed_chunk_changes.end() ? "no change" : it->second));
                ++events;
            }
            for (const auto& [chunk, status] : observed_chunk_changes)
                if (!expected_chunks.contains(chunk))
                    return report(Outcome::failed, std::format("tick {} chunk ({}, {}): expected no change, observed {}", t, chunk.first,
                                                               chunk.second, status));
            std::ranges::stable_sort(expected, {}, &std::pair<std::size_t, StreamEvent>::first);
            std::ranges::stable_sort(observed, {}, &std::pair<std::size_t, StreamEvent>::first);
            const auto n = std::min(expected.size(), observed.size());
            for (std::size_t i = 0; i < n; ++i)
                if (!same_stream(expected[i].second, observed[i].second, f.tolerance))
                    return report(Outcome::failed, std::format("tick {} event {} (line {}): expected {}, observed {}", t, i,
                                                               stream_line(expected[i].second), stream_text(*p.mapping, expected[i].second),
                                                               stream_text(*p.mapping, observed[i].second)));
            if (expected.size() != observed.size())
                return report(Outcome::failed, std::format("tick {}: expected {} events, observed {}; first unmatched: {}", t,
                                                           expected.size(), observed.size(),
                                                           stream_text(*p.mapping, observed.size() > n ? observed[n].second : expected[n].second)));
            for (const auto& [k, e] : expected) {
                if (std::holds_alternative<MappedChange>(e)) ++changes;
                else ++events;
            }
        }
        for (const auto& probe : f.light_probes) {
            if (probe.tick != t) continue;
            const auto observed = exec->light(probe.pos);
            if (!observed) return report(Outcome::unsupported, "the executor does not compute light");
            if (*observed != *probe.levels)
                return report(Outcome::failed, std::format("tick {} light probe (line {}): expected {} {}, observed {} {}", t, probe.line,
                                                           probe.levels->first, probe.levels->second, observed->first, observed->second));
            ++probes;
        }
        for (const auto& [probe, state] : p.blocks) {
            if (probe->tick != t) continue;
            const auto observed = exec->block(probe->pos);
            if (observed != state)
                return report(Outcome::failed, std::format("tick {} block probe (line {}): expected {}, observed {}", t, probe->line,
                                                           p.mapping->notation(state), p.mapping->notation(observed)));
            ++probes;
        }
        for (const auto& [probe, stack] : p.slots) {
            if (probe->tick != t) continue;
            const auto observed = exec->slot(probe->pos, probe->slot);
            if (!observed || *observed != stack) {
                const auto text = [&](std::optional<contracts::ItemStack> s) -> std::string {
                    if (!s) return "no container slot";
                    return s->empty() ? std::string("empty") : std::format("{} x{}", p.mapping->item_name(s->item), s->count);
                };
                return report(Outcome::failed, std::format("tick {} slot probe (line {}): expected {}, observed {}", t, probe->line,
                                                           text(stack), text(observed)));
            }
            ++probes;
        }
        if (v2 && f.observe_entities) {
            const auto live = exec->entities();
            const auto actual = tracker.probes(t, live);
            std::vector<const EntityProbe*> expected;
            for (const auto& probe : f.entity_probes)
                if (probe.tick == t) expected.push_back(&probe);
            const auto n = std::min(actual.size(), expected.size());
            for (std::size_t i = 0; i < n; ++i) {
                const auto& e = *expected[i];
                const auto& a = actual[i];
                if (e.handle != a.handle || !close(e.position, a.position, f.tolerance) || !close(e.velocity, a.velocity, f.tolerance)
                    || !same_properties(e.properties, a.properties))
                    return report(Outcome::failed, std::format("tick {} entity probe (line {}): expected {} at {} moving {}{}, observed {} at {} moving {}{}",
                                                               t, e.line, e.handle, vec_text(e.position), vec_text(e.velocity),
                                                               properties_text(e.properties), a.handle, vec_text(a.position),
                                                               vec_text(a.velocity), properties_text(a.properties)));
                ++probes;
            }
            if (actual.size() != expected.size())
                return report(Outcome::failed, std::format("tick {}: expected {} live entities, observed {}", t, expected.size(), actual.size()));
            const auto order = tracker.order(t, t + 2, live);
            std::vector<const EntityOrder*> expected_orders;
            for (const auto& o : f.entity_orders)
                if (o.tick == t) expected_orders.push_back(&o);
            const bool observed_order = order.has_value();
            if (expected_orders.size() != (observed_order ? 1u : 0u)
                || (observed_order && (expected_orders[0]->stage != order->stage || expected_orders[0]->handles != order->handles))) {
                std::string want = expected_orders.empty() ? "none" : "";
                for (const auto* o : expected_orders)
                    for (const auto& h : o->handles) want += " " + h;
                std::string got = observed_order ? "" : "none";
                if (observed_order)
                    for (const auto& h : order->handles) got += " " + h;
                return report(Outcome::failed, std::format("tick {} entity order: expected{}, observed{}", t,
                                                           want.empty() ? " none" : want, got.empty() ? " none" : got));
            }
            events += expected_orders.size();
        }
        if (v2 && f.observe_rng) {
            // Draws from the level random, per stage, against the recording.
            auto observed_draws = level_draws(exec->trace());
            for (const auto& r : f.rng) {
                if (r.tick != t) continue;
                const auto it = observed_draws.find(r.stage);
                const std::uint64_t got = it == observed_draws.end() ? 0 : it->second;
                if (r.stream != "level" || got != r.draws)
                    return report(Outcome::failed, std::format("tick {} rng (line {}): expected {} draws from `{}` in {}, observed {}", t, r.line,
                                                               r.draws, r.stream, r.stage, got));
                observed_draws.erase(r.stage);
                ++events;
            }
            if (!observed_draws.empty())
                return report(Outcome::failed, std::format("tick {} rng: expected no draws in {}, observed {}", t, observed_draws.begin()->first,
                                                           observed_draws.begin()->second));
        }
        return std::nullopt;
    };
    if (auto r = execute(f, p, *exec, observe)) return *r;
    return {Outcome::passed, std::format("{} changes, {} probes and {} events matched", changes, probes, events), changes, probes, events};
}

std::expected<Fixture, std::string> record_fixture(const Fixture& skeleton, const ExecutorFactory& factory) {
    auto prepared = prepare(skeleton, false);
    if (!prepared) return std::unexpected(prepared.error().detail);
    const auto& p = *prepared;
    auto out = skeleton;
    out.source = contracts::EvidenceSource::circuit_reference;
    out.harness = std::string(p.bundle.rules->id);
    out.recorded_changes.clear();
    out.entity_events.clear();
    out.entity_probes.clear();
    out.entity_orders.clear();
    out.chunk_events.clear();
    out.rng.clear();
    auto exec = factory(p.bundle, 0);
    EntityTracker tracker(*p.mapping);
    std::size_t seq = 0;
    const auto observe = [&](Tick t) -> std::optional<FixtureReport> {
        if (out.format >= 2) {
            std::vector<MappedChange> changes;
            tracker.collect(out, t, exec->trace(), out.changes != ChangeMode::none, changes, out.entity_events, out.chunk_events, seq);
            for (const auto& c : changes)
                out.recorded_changes.push_back({t, c.stage, c.pos, p.mapping->notation(c.before), p.mapping->notation(c.after), c.seq, 0});
        } else if (out.changes != ChangeMode::none && t >= 1) {
            for (const auto& c : area_changes(out, exec->trace()))
                out.recorded_changes.push_back({t, "", c.pos, p.mapping->notation(c.before), p.mapping->notation(c.after), 0, 0});
        }
        for (auto& probe : out.block_probes)
            if (probe.tick == t) probe.state.value = p.mapping->notation(exec->block(probe.pos));
        for (auto& probe : out.light_probes)
            if (probe.tick == t) {
                probe.levels = exec->light(probe.pos);
                if (!probe.levels) return report(Outcome::unsupported, "the executor does not compute light");
            }
        for (auto& probe : out.slot_probes) {
            if (probe.tick != t) continue;
            const auto s = exec->slot(probe.pos, probe.slot);
            if (!s) return report(Outcome::invalid, std::format("line {}: no container slot to record", probe.line));
            probe.item.value = s->empty() ? std::string("empty") : p.mapping->item_name(s->item);
            probe.count = s->empty() ? 0 : s->count;
        }
        if (out.format >= 2 && out.observe_entities) {
            const auto live = exec->entities();
            for (auto& probe : tracker.probes(t, live)) out.entity_probes.push_back(std::move(probe));
            if (auto order = tracker.order(t, t + 2, live)) out.entity_orders.push_back(std::move(*order));
        }
        if (out.format >= 2 && out.observe_rng)
            for (const auto& [stage, draws] : level_draws(exec->trace())) out.rng.push_back({t, stage, "level", draws, 0});
        return std::nullopt;
    };
    if (auto r = execute(skeleton, p, *exec, observe)) return std::unexpected(r->detail);
    return out;
}

} // namespace circuit::evidence
