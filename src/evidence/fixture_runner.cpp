// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/evidence/content.hpp>
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/runtime/engine.hpp>
#include <algorithm>
#include <format>
#include <map>

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
              return o;
          }()) {}
    void advance(const std::vector<Command>& commands) override { engine_.advance(commands); }
    const std::vector<contracts::TraceRecord>& trace() const override { return engine_.trace(); }
    BlockState block(BlockPos pos) const override {
        const auto s = engine_.view().block(pos);
        return s ? *s : BlockState::unloaded_sentinel();
    }
    std::optional<contracts::ItemStack> slot(BlockPos pos, std::uint8_t slot) const override {
        const auto be = engine_.view().block_entity(pos);
        if (!be || !*be || slot >= (*be)->slots.size()) return std::nullopt;
        return (*be)->slots[slot];
    }

private:
    runtime::Engine engine_;
};

struct MappedChange {
    BlockPos pos;
    BlockState before, after;
    std::size_t line{};
    bool operator==(const MappedChange& o) const { return pos == o.pos && before == o.before && after == o.after; }
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

FixtureReport report(Outcome outcome, std::string detail) { return {outcome, std::move(detail), 0, 0}; }

std::expected<Prepared, FixtureReport> prepare(const Fixture& f, bool require_observations) {
    Prepared p;
    p.mapping = ContentMapping::for_game_version(f.game_version);
    if (!p.mapping) return std::unexpected(report(Outcome::unsupported, std::format("no content mapping for {}", f.game_version)));
    const contracts::VersionBundle* found = nullptr;
    for (const auto& b : contracts::BundleRegistry::candidates())
        if (b.game_version == f.game_version && b.rules) found = &b;
    if (!found) return std::unexpected(report(Outcome::unsupported, std::format("no executable bundle for {}", f.game_version)));
    p.bundle = *found;
    p.bundle.limits.random_tick_speed = f.random_tick_speed;
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
    p.inputs[1].push_back({admission++, contracts::AddTicket{1, center, static_cast<std::uint8_t>(radius)}});
    const auto fail_line = [](std::size_t line, const std::string& message) {
        return std::unexpected(report(Outcome::unsupported, std::format("line {}: {}", line, message)));
    };
    for (const auto& a : f.actions) {
        contracts::CommandBody body;
        switch (a.kind) {
        case Action::Kind::set: {
            const auto s = p.mapping->block(a.state);
            if (!s) return fail_line(a.line, s.error());
            body = contracts::SetBlock{a.pos, s->raw(), std::nullopt};
            break;
        }
        case Action::Kind::use: body = contracts::UseLever{a.pos}; break;
        case Action::Kind::insert: {
            const auto item = p.mapping->item(a.item);
            if (!item) return fail_line(a.line, item.error());
            body = contracts::InsertItem{a.pos, a.slot, {*item, a.count}};
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
        if (*before != *after) p.changes[c.tick].push_back({c.pos, *before, *after, c.line});
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

std::vector<MappedChange> area_changes(const Fixture& f, const std::vector<contracts::TraceRecord>& trace) {
    std::vector<MappedChange> out;
    for (const auto& r : trace)
        if (r.kind == TraceKind::block_changed && f.contains(r.pos))
            out.push_back({r.pos, BlockState::decode(static_cast<std::uint32_t>(r.a)).value(),
                           BlockState::decode(static_cast<std::uint32_t>(r.b)).value(), 0});
    return out;
}

std::string change_text(const ContentMapping& m, const MappedChange& c) {
    return std::format("({},{},{}) {} -> {}", c.pos.x, c.pos.y, c.pos.z, m.notation(c.before), m.notation(c.after));
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

FixtureReport evaluate_fixture(const Fixture& f, const ExecutorFactory& factory) {
    auto prepared = prepare(f, true);
    if (!prepared) return prepared.error();
    const auto& p = *prepared;
    auto exec = factory(p.bundle, 0);
    std::size_t changes = 0, probes = 0;
    const auto observe = [&](Tick t) -> std::optional<FixtureReport> {
        if (f.changes != ChangeMode::none && t >= 1) {
            auto actual = area_changes(f, exec->trace());
            const auto it = p.changes.find(t);
            auto expected = it == p.changes.end() ? std::vector<MappedChange>{} : it->second;
            if (f.changes == ChangeMode::unordered) {
                const auto key = [](const MappedChange& c) { return std::tuple(c.pos, c.before.raw(), c.after.raw()); };
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
        return std::nullopt;
    };
    if (auto r = execute(f, p, *exec, observe)) return *r;
    return {Outcome::passed, std::format("{} changes and {} probes matched", changes, probes), changes, probes};
}

std::expected<Fixture, std::string> record_fixture(const Fixture& skeleton, const ExecutorFactory& factory) {
    auto prepared = prepare(skeleton, false);
    if (!prepared) return std::unexpected(prepared.error().detail);
    const auto& p = *prepared;
    auto out = skeleton;
    out.source = contracts::EvidenceSource::circuit_reference;
    out.harness = std::string(p.bundle.rules->id);
    out.recorded_changes.clear();
    auto exec = factory(p.bundle, 0);
    std::string error;
    const auto observe = [&](Tick t) -> std::optional<FixtureReport> {
        if (out.changes != ChangeMode::none && t >= 1)
            for (const auto& c : area_changes(out, exec->trace()))
                out.recorded_changes.push_back({t, c.pos, p.mapping->notation(c.before), p.mapping->notation(c.after), 0});
        for (auto& probe : out.block_probes)
            if (probe.tick == t) probe.state.value = p.mapping->notation(exec->block(probe.pos));
        for (auto& probe : out.slot_probes) {
            if (probe.tick != t) continue;
            const auto s = exec->slot(probe.pos, probe.slot);
            if (!s) return report(Outcome::invalid, std::format("line {}: no container slot to record", probe.line));
            probe.item.value = s->empty() ? std::string("empty") : p.mapping->item_name(s->item);
            probe.count = s->empty() ? 0 : s->count;
        }
        return std::nullopt;
    };
    if (auto r = execute(skeleton, p, *exec, observe)) return std::unexpected(r->detail);
    return out;
}

} // namespace circuit::evidence
