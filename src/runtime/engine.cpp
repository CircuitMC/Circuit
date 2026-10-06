// SPDX-License-Identifier: AGPL-3.0-only
#include "region_context.hpp"
#include <circuit/gameplay/rng.hpp>
#include <circuit/gameplay/rules.hpp>
#include <circuit/runtime/engine.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <stdexcept>

namespace circuit::runtime {
using namespace detail;
using contracts::Command;
using contracts::TraceRecord;
using gameplay::WorldDump;

namespace {
struct CapacityExceeded : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One root of a phase. `key` is the root's position in the phase's serial
// order; for block events it is the event's queue sequence.
struct RootItem {
    std::uint64_t key{};
    ChunkPos home;
    gameplay::ScheduledTick tick;
    std::int32_t section{};
    EntityId entity;
    BlockPos pos;
    BlockEvent event;
};
} // namespace

struct Engine::Impl {
    Impl(const contracts::VersionBundle& b, std::uint64_t s, EngineOptions o);

    void advance(const std::vector<Command>& commands);
    void command_phase(const std::vector<Command>& commands);
    void apply_levels(RegionContext& ctx);
    void tick_phase(TickList list);
    void random_phase();
    void block_event_phase();
    void entity_phase();
    void block_entity_phase();
    void run_phase(Phase phase, std::vector<RootItem> roots);
    void commit(Phase phase, const std::vector<RegionContext*>& contexts);
    void maintain_layout();
    void split_pass();
    RegionId region_for_new_chunk(ChunkPos c);
    WorldDump dump() const;

    const contracts::VersionBundle& bundle;
    std::uint64_t seed;
    EngineOptions options;
    std::shared_ptr<JobRunner> runner;
    WorldStore world;
    RegionMap regions;
    EngineStats stats;
    std::vector<TraceRecord> trace;
    bool failed{};
    std::uint64_t epoch{};
    std::map<std::pair<ChunkPos, ChunkPos>, Tick> edges; // Last tick two columns interacted.
    gameplay::Rng layout_rng;
};

namespace {
class RegionTask final : public Task {
public:
    enum class Outcome { running, done, conflict, injected, failed };

    RegionTask(Engine::Impl& engine, RegionId region, Phase phase, std::vector<RootItem> roots)
        : region(region), phase(phase), roots(std::move(roots)), engine_(engine) {
        ctx = std::make_unique<RegionContext>(engine.bundle, engine.world, engine.regions, region, phase,
                                              engine.options.record_trace, engine.options.unchecked_ownership_for_testing);
        if (phase == Phase::block_events)
            for (const auto& r : this->roots) ctx->push_local(r.event);
        if (phase == Phase::block_ticks || phase == Phase::fluid_ticks)
            for (const auto& r : this->roots) ctx->pending_ticks.insert({r.tick.pos, r.tick.block});
    }

    bool step() override {
        if (outcome != Outcome::running) return false;
        try {
            if (phase == Phase::block_events) {
                if (!ctx->fifo.empty()) {
                    const auto local = ctx->fifo.front();
                    ctx->fifo.pop_front();
                    ctx->fifo_pending.erase(local.event);
                    if (++processed > engine_.bundle.limits.max_block_events_per_tick)
                        throw CapacityExceeded("block event capacity exceeded");
                    ctx->begin_root(local.id, contracts::chunk_of(local.event.pos));
                    gameplay::run_block_event(*ctx, local.event);
                }
                if (ctx->fifo.empty()) outcome = Outcome::done;
            } else {
                if (cursor < roots.size()) execute(roots[cursor++]);
                if (cursor == roots.size()) outcome = Outcome::done;
            }
        } catch (const ForeignAccess& f) {
            outcome = Outcome::conflict;
            conflict_chunk = f.chunk;
        } catch (...) {
            outcome = Outcome::failed;
            error = std::current_exception();
        }
        return outcome == Outcome::running;
    }

    void inject_abort() override {
        if (outcome == Outcome::running) outcome = Outcome::injected;
    }

    RegionId region;
    Phase phase;
    std::vector<RootItem> roots;
    std::unique_ptr<RegionContext> ctx;
    Outcome outcome{Outcome::running};
    ChunkPos conflict_chunk{};
    std::exception_ptr error;

private:
    void execute(const RootItem& r) {
        ctx->begin_root(r.key, r.home);
        if (phase == Phase::block_ticks || phase == Phase::fluid_ticks) ctx->pending_ticks.erase({r.tick.pos, r.tick.block});
        switch (phase) {
        case Phase::block_ticks: gameplay::run_scheduled_tick(*ctx, TickList::block, r.tick); break;
        case Phase::fluid_ticks: gameplay::run_scheduled_tick(*ctx, TickList::fluid, r.tick); break;
        case Phase::random_ticks: {
            auto stream = gameplay::random_tick_stream(engine_.seed, engine_.world.time, r.home, r.section);
            gameplay::run_random_ticks(*ctx, r.home, r.section, stream);
            break;
        }
        case Phase::entities: gameplay::run_entity(*ctx, r.entity); break;
        case Phase::block_entities: gameplay::run_block_entity(*ctx, r.pos); break;
        default: throw std::logic_error("runtime: phase has no region roots");
        }
    }

    std::size_t cursor{};
    std::uint64_t processed{};
    Engine::Impl& engine_;
};
} // namespace

Engine::Impl::Impl(const contracts::VersionBundle& b, std::uint64_t s, EngineOptions o)
    : bundle(b), seed(s), options(std::move(o)), layout_rng(options.layout_seed) {
    if (!bundle.rules) throw std::invalid_argument("engine: bundle has no executable rules");
    runner = options.runner ? options.runner : std::make_shared<PoolRunner>(options.workers);
    if (options.unchecked_ownership_for_testing && runner->concurrent())
        throw std::invalid_argument("engine: unchecked ownership requires a non-concurrent runner");
    if (options.split_interval == 0) throw std::invalid_argument("engine: split interval must be positive");
}

void Engine::Impl::advance(const std::vector<Command>& commands) {
    if (failed) throw std::logic_error("engine: a previous tick failed");
    if (world.time == UINT64_MAX) throw std::overflow_error("engine: time exhausted");
    failed = true; // Cleared only when the whole tick completes.
    ++epoch;
    ++world.time;
    trace.clear();
    const auto timed = [this](contracts::Phase phase, auto&& run) {
        const auto start = std::chrono::steady_clock::now();
        run();
        stats.phase_nanoseconds[static_cast<std::size_t>(phase)] += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    };
    timed(Phase::commands, [&] { command_phase(commands); });
    timed(Phase::block_ticks, [&] { tick_phase(TickList::block); });
    timed(Phase::fluid_ticks, [&] { tick_phase(TickList::fluid); });
    timed(Phase::random_ticks, [&] { random_phase(); });
    timed(Phase::block_events, [&] { block_event_phase(); });
    timed(Phase::entities, [&] { entity_phase(); });
    timed(Phase::block_entities, [&] { block_entity_phase(); });
    maintain_layout();
    failed = false;
}

void Engine::Impl::command_phase(const std::vector<Command>& commands) {
    ++stats.phases;
    // Commands run serially on the coordinator with access to every column.
    RegionContext ctx(bundle, world, regions, all_regions, Phase::commands, options.record_trace, false);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        ctx.begin_root(i, {});
        const auto& c = commands[i];
        contracts::Result<void> ticket_result;
        if (const auto* add = std::get_if<contracts::AddTicket>(&c.body)) ticket_result = world.tickets.add(*add, bundle.limits);
        else if (const auto* rem = std::get_if<contracts::RemoveTicket>(&c.body)) ticket_result = world.tickets.remove(rem->ticket);
        else {
            gameplay::apply_command(ctx, c);
            continue;
        }
        if (ticket_result) ctx.record(TraceKind::command_applied, {}, c.admission, 0, 0);
        else ctx.record(TraceKind::command_rejected, {}, c.admission, static_cast<std::uint64_t>(ticket_result.error()), 0);
    }
    commit(Phase::commands, {&ctx});
    // Chunk-level changes take effect at the end of the command phase.
    RegionContext levels(bundle, world, regions, all_regions, Phase::commands, options.record_trace, false);
    levels.begin_root(commands.size(), {});
    apply_levels(levels);
    commit(Phase::commands, {&levels});
}

RegionId Engine::Impl::region_for_new_chunk(ChunkPos c) {
    if (options.layout == LayoutPolicy::single_region && !regions.members.empty()) {
        const auto id = regions.members.begin()->first;
        regions.owner[c] = id;
        regions.members[id].insert(c);
        return id;
    }
    return regions.add_singleton(c);
}

void Engine::Impl::apply_levels(RegionContext& ctx) {
    const auto desired = world.tickets.levels();
    std::set<ChunkPos> keys;
    for (const auto& [c, data] : world.loaded) keys.insert(c);
    for (const auto& [c, level] : desired) keys.insert(c);
    for (const auto c : keys) {
        auto* data = world.find_loaded(c);
        const auto current = data ? data->level : ChunkLevel::unloaded;
        const auto it = desired.find(c);
        const auto want = it == desired.end() ? ChunkLevel::unloaded : it->second;
        if (current == want) continue;
        const auto origin = contracts::chunk_origin(c);
        if (!data) {
            std::unique_ptr<ChunkData> chunk;
            if (auto saved = world.saved.find(c); saved != world.saved.end()) {
                chunk = std::move(saved->second);
                world.saved.erase(saved);
                const auto next = chunk->generation.next();
                if (!next) throw std::overflow_error("engine: chunk generation exhausted");
                chunk->generation = *next;
            } else {
                chunk = std::make_unique<ChunkData>();
                chunk->pos = c;
                chunk->sections.resize(static_cast<std::size_t>(bundle.world.sections()));
                if (options.generator) {
                    auto& sections = chunk->sections;
                    options.generator(c, [&sections](std::size_t s) -> std::array<gameplay::BlockState, 4096>& {
                        if (!sections.at(s)) sections[s] = std::make_shared<Section>();
                        return sections[s]->blocks;
                    });
                }
            }
            chunk->level = want;
            ctx.record(TraceKind::chunk_loaded, origin, chunk->generation.value, static_cast<std::uint64_t>(want), 0);
            world.loaded.emplace(c, std::move(chunk));
            region_for_new_chunk(c);
        } else if (want == ChunkLevel::unloaded) {
            ctx.record(TraceKind::chunk_unloaded, origin, data->generation.value, 0, 0);
            data->level = ChunkLevel::unloaded;
            auto node = world.loaded.extract(c);
            world.saved.insert(std::move(node));
            regions.remove(c);
        } else {
            ctx.record(TraceKind::chunk_level_changed, origin, static_cast<std::uint64_t>(current),
                       static_cast<std::uint64_t>(want), 0);
            data->level = want;
        }
    }
}

void Engine::Impl::tick_phase(TickList list) {
    const auto l = static_cast<int>(list);
    std::vector<gameplay::ScheduledTick> due;
    for (const auto& [c, data] : world.loaded) {
        if (data->level != ChunkLevel::ticking) continue;
        for (const auto& [key, e] : data->ticks[l])
            if (e.due <= world.time) due.push_back({key.first, key.second, e.due, e.priority, e.suborder});
    }
    std::ranges::sort(due, gameplay::tick_before);
    if (due.size() > bundle.limits.max_scheduled_ticks_per_list) due.resize(bundle.limits.max_scheduled_ticks_per_list);
    std::vector<RootItem> roots;
    roots.reserve(due.size());
    for (std::size_t i = 0; i < due.size(); ++i) {
        const auto c = contracts::chunk_of(due[i].pos);
        world.loaded.at(c)->ticks[l].erase({due[i].pos, due[i].block});
        RootItem r;
        r.key = i;
        r.home = c;
        r.tick = due[i];
        roots.push_back(r);
    }
    run_phase(list == TickList::block ? Phase::block_ticks : Phase::fluid_ticks, std::move(roots));
}

void Engine::Impl::random_phase() {
    const auto sections = bundle.world.sections();
    std::vector<RootItem> roots;
    std::uint64_t index = 0;
    for (const auto& [c, data] : world.loaded) {
        if (data->level != ChunkLevel::ticking) continue;
        for (std::int32_t s = 0; s < sections; ++s) {
            RootItem r;
            r.key = index * static_cast<std::uint64_t>(sections) + static_cast<std::uint64_t>(s);
            r.home = c;
            r.section = s;
            roots.push_back(r);
        }
        ++index;
    }
    run_phase(Phase::random_ticks, std::move(roots));
}

void Engine::Impl::block_event_phase() {
    std::vector<RootItem> roots;
    for (const auto& [c, data] : world.loaded) {
        if (data->level != ChunkLevel::ticking) continue;
        for (const auto& e : data->events) {
            RootItem r;
            r.key = e.seq;
            r.home = c;
            r.event = e.event;
            roots.push_back(r);
        }
        data->events.clear(); // Deferred events in non-ticking columns stay where they are.
    }
    std::ranges::sort(roots, {}, &RootItem::key);
    run_phase(Phase::block_events, std::move(roots));
}

void Engine::Impl::entity_phase() {
    std::vector<RootItem> roots;
    for (const auto& [c, data] : world.loaded) {
        if (data->level != ChunkLevel::ticking) continue;
        for (const auto& [id, e] : data->entities) {
            RootItem r;
            r.key = id.value;
            r.home = c;
            r.entity = id;
            roots.push_back(r);
        }
    }
    std::ranges::sort(roots, {}, &RootItem::key);
    run_phase(Phase::entities, std::move(roots));
}

void Engine::Impl::block_entity_phase() {
    std::vector<RootItem> roots;
    for (const auto& [c, data] : world.loaded) {
        if (data->level != ChunkLevel::ticking) continue;
        for (const auto& [pos, be] : data->block_entities) {
            RootItem r;
            r.key = be.order;
            r.home = c;
            r.pos = pos;
            roots.push_back(r);
        }
    }
    std::ranges::sort(roots, {}, &RootItem::key);
    run_phase(Phase::block_entities, std::move(roots));
}

void Engine::Impl::run_phase(Phase phase, std::vector<RootItem> roots) {
    ++stats.phases;
    if (roots.empty()) return;
    std::map<RegionId, std::vector<RootItem>> groups;
    for (auto& r : roots) groups[regions.owner.at(r.home)].push_back(std::move(r));
    std::map<RegionId, std::unique_ptr<RegionTask>> staged;
    std::vector<RegionId> to_run;
    for (const auto& [id, list] : groups) to_run.push_back(id);
    std::uint32_t attempt = 0;
    while (!to_run.empty()) {
        std::vector<std::unique_ptr<RegionTask>> tasks;
        std::vector<Task*> pointers;
        for (const auto id : to_run) {
            tasks.push_back(std::make_unique<RegionTask>(*this, id, phase, groups.at(id)));
            pointers.push_back(tasks.back().get());
        }
        ++stats.attempts;
        stats.tasks += tasks.size();
        stats.max_tasks_in_phase = std::max<std::uint64_t>(stats.max_tasks_in_phase, tasks.size());
        runner->run(pointers, attempt++);

        to_run.clear();
        std::vector<std::pair<RegionId, RegionId>> conflicts;
        for (auto& t : tasks) {
            switch (t->outcome) {
            case RegionTask::Outcome::done: staged[t->region] = std::move(t); break;
            case RegionTask::Outcome::conflict:
                ++stats.conflict_aborts;
                t->ctx->rollback();
                conflicts.push_back({t->region, regions.owner.at(t->conflict_chunk)});
                break;
            case RegionTask::Outcome::injected:
                ++stats.injected_aborts;
                t->ctx->rollback();
                to_run.push_back(t->region);
                break;
            case RegionTask::Outcome::failed: std::rethrow_exception(t->error);
            case RegionTask::Outcome::running: throw std::logic_error("runtime: runner returned an unfinished task");
            }
        }
        if (!conflicts.empty()) {
            std::map<RegionId, RegionId> parent;
            const auto find = [&](RegionId x) {
                parent.try_emplace(x, x);
                while (parent[x] != x) x = parent[x] = parent[parent[x]];
                return x;
            };
            for (const auto& [a, b] : conflicts) {
                const auto ra = find(a), rb = find(b);
                if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
            }
            std::map<RegionId, std::vector<RegionId>> sets;
            for (const auto& [x, p] : parent) sets[find(x)].push_back(x);
            for (const auto& [root, members] : sets) {
                std::vector<RootItem> merged;
                for (const auto m : members) {
                    // Staged results of a merged region were computed in isolation
                    // from a column it now shares: discard and recompute together.
                    if (auto s = staged.find(m); s != staged.end()) {
                        s->second->ctx->rollback();
                        staged.erase(s);
                    }
                    if (auto g = groups.find(m); g != groups.end()) {
                        std::ranges::move(g->second, std::back_inserter(merged));
                        groups.erase(g);
                    }
                    std::erase(to_run, m);
                    if (m != root) {
                        regions.absorb(root, m);
                        ++stats.merges;
                    }
                }
                std::ranges::sort(merged, {}, &RootItem::key);
                if (!merged.empty()) {
                    groups[root] = std::move(merged);
                    to_run.push_back(root);
                }
            }
        }
    }

    std::vector<RegionContext*> contexts;
    for (auto& [id, t] : staged) contexts.push_back(t->ctx.get());

    if (phase == Phase::block_events) {
        // Reconstruct the level-wide FIFO order: initial events by sequence,
        // then each event's children appended when it ran.
        struct Ref {
            RegionTask* task;
            std::uint32_t local;
        };
        std::vector<std::pair<std::uint64_t, Ref>> initial;
        std::map<RegionTask*, std::vector<std::uint64_t>> global;
        for (auto& [id, t] : staged) {
            for (std::uint32_t i = 0; i < t->roots.size(); ++i) initial.push_back({t->roots[i].key, {t.get(), i}});
            global[t.get()].assign(t->ctx->children.size(), UINT64_MAX);
        }
        std::ranges::sort(initial, {}, [](const auto& p) { return p.first; });
        std::deque<Ref> queue;
        for (const auto& [seq, ref] : initial) queue.push_back(ref);
        std::uint64_t processed = 0;
        while (!queue.empty()) {
            const auto r = queue.front();
            queue.pop_front();
            global[r.task][r.local] = processed++;
            for (const auto child : r.task->ctx->children[r.local]) queue.push_back({r.task, child});
        }
        if (processed > bundle.limits.max_block_events_per_tick) throw CapacityExceeded("block event capacity exceeded");
        for (auto& [task, map] : global) {
            auto& ctx = *task->ctx;
            for (auto& record : ctx.trace) record.key.root = map.at(record.key.root);
            for (auto& entries : ctx.ledger)
                for (auto& e : entries) e.root = map.at(e.root);
        }
    }
    commit(phase, contexts);
}

void Engine::Impl::commit(Phase, const std::vector<RegionContext*>& contexts) {
    // Level-wide counters are assigned in serial order: by root, then by action
    // within the root. Every staged creation consumes a value, including those
    // of objects removed again before commit, as in the serial order.
    // values[counter][context][ledger index] holds the assigned value.
    std::array<std::vector<std::vector<std::uint64_t>>, counter_kinds> values;
    const std::array<std::uint64_t*, counter_kinds> next{&world.next_suborder, &world.next_event_seq,
                                                         &world.next_block_entity_order, &world.next_entity_id};
    for (std::size_t k = 0; k < counter_kinds; ++k) {
        struct Ref {
            std::uint64_t root;
            std::uint32_t seq;
            std::size_t context;
            std::size_t index;
        };
        std::vector<Ref> refs;
        values[k].resize(contexts.size());
        for (std::size_t i = 0; i < contexts.size(); ++i) {
            const auto& entries = contexts[i]->ledger[k];
            values[k][i].resize(entries.size());
            for (std::size_t j = 0; j < entries.size(); ++j) refs.push_back({entries[j].root, entries[j].seq, i, j});
        }
        std::ranges::sort(refs, [](const Ref& a, const Ref& b) { return a.root != b.root ? a.root < b.root : a.seq < b.seq; });
        for (const auto& r : refs) {
            if (*next[k] == UINT64_MAX) throw std::overflow_error("engine: level-wide counter exhausted");
            values[k][r.context][r.index] = (*next[k])++;
        }
    }
    const auto value = [&](Counter c, std::size_t context, Provisional p) {
        return values[static_cast<std::size_t>(c)][context].at(p.ledger);
    };
    // Entities spawned in parallel roots receive their ids now; provisional ids
    // are rewritten in storage, the index and the trace, also for entities that
    // were removed again.
    std::unordered_map<std::uint64_t, std::uint64_t> real_ids;
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        const auto& spawned = contexts[i]->ledger[static_cast<std::size_t>(Counter::entity_id)];
        for (std::size_t j = 0; j < spawned.size(); ++j)
            real_ids[spawned[j].entity.value] = values[static_cast<std::size_t>(Counter::entity_id)][i][j];
    }
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        for (auto& [c, snapshot] : contexts[i]->snapshots) {
            auto& data = *world.loaded.at(c);
            for (auto& list : data.ticks)
                for (auto& [k, e] : list)
                    if (e.provisional.pending) {
                        e.suborder = value(Counter::suborder, i, e.provisional);
                        e.provisional = {};
                    }
            for (auto& e : data.events)
                if (e.provisional.pending) {
                    e.seq = value(Counter::event_seq, i, e.provisional);
                    e.provisional = {};
                }
            for (const auto& [pos, provisional] : data.new_block_entities)
                data.block_entities.at(pos).order = value(Counter::block_entity_order, i, provisional);
            data.new_block_entities.clear();
            for (const auto& [provisional_id, provisional] : data.new_entities) {
                const EntityId id{value(Counter::entity_id, i, provisional)};
                auto node = data.entities.extract(provisional_id);
                node.key() = id;
                node.mapped().id = id;
                data.entities.insert(std::move(node));
            }
            data.new_entities.clear();
        }
    }
    const auto real = [&](std::uint64_t id) {
        const auto it = real_ids.find(id);
        return it == real_ids.end() ? id : it->second;
    };
    std::vector<TraceRecord> records;
    for (auto* ctx : contexts) {
        for (const auto& [id, c] : ctx->entity_moves) {
            const EntityId key{real(id.value)};
            if (c) world.entity_index[key] = *c;
            else world.entity_index.erase(key);
        }
        for (const auto& edge : ctx->interactions) edges[edge] = world.time;
        for (auto record : ctx->trace) {
            switch (record.kind) {
            case TraceKind::entity_spawned: case TraceKind::entity_removed:
            case TraceKind::entity_changed_chunk: case TraceKind::entity_stack_changed:
                record.a = real(record.a);
                break;
            default: break;
            }
            records.push_back(record);
        }
        ctx->snapshots.clear();
        for (auto& entries : ctx->ledger) entries.clear();
    }
    std::ranges::sort(records, {}, &TraceRecord::key);
    trace.insert(trace.end(), records.begin(), records.end());
}

void Engine::Impl::maintain_layout() {
    std::erase_if(edges, [&](const auto& e) { return world.time - e.second > options.quiet_ticks; });
    switch (options.layout) {
    case LayoutPolicy::single_region: break;
    case LayoutPolicy::adaptive:
        if (world.time % options.split_interval == 0) split_pass();
        break;
    case LayoutPolicy::perturbed: {
        const auto choice = layout_rng.bounded(4);
        if (choice == 0) {
            // Every column becomes its own region.
            std::vector<ChunkPos> all;
            for (const auto& [c, id] : regions.owner) all.push_back(c);
            std::ranges::sort(all);
            regions.owner.clear();
            regions.members.clear();
            for (const auto c : all) regions.add_singleton(c);
            stats.splits += all.size();
        } else if (choice == 1 && regions.members.size() > 1) {
            // Merge random pairs, including distant ones.
            std::vector<RegionId> ids;
            for (const auto& [id, m] : regions.members) ids.push_back(id);
            for (std::size_t i = 0; i + 1 < ids.size(); i += 2) {
                const auto j = i + 1 + layout_rng.bounded(static_cast<std::uint32_t>(ids.size() - i - 1));
                std::swap(ids[i + 1], ids[j]);
                regions.absorb(ids[i], ids[i + 1]);
                ++stats.merges;
            }
        }
        break;
    }
    }
}

void Engine::Impl::split_pass() {
    std::vector<RegionId> ids;
    for (const auto& [id, m] : regions.members)
        if (m.size() > 1) ids.push_back(id);
    for (const auto id : ids) {
        const auto members = regions.members.at(id);
        std::map<ChunkPos, std::vector<ChunkPos>> adjacency;
        for (const auto& [edge, last] : edges) {
            if (!members.contains(edge.first) || !members.contains(edge.second)) continue;
            adjacency[edge.first].push_back(edge.second);
            adjacency[edge.second].push_back(edge.first);
        }
        std::set<ChunkPos> seen;
        std::vector<std::vector<ChunkPos>> components;
        for (const auto start : members) {
            if (seen.contains(start)) continue;
            std::vector<ChunkPos> component;
            std::deque<ChunkPos> queue{start};
            seen.insert(start);
            while (!queue.empty()) {
                const auto c = queue.front();
                queue.pop_front();
                component.push_back(c);
                for (const auto n : adjacency[c])
                    if (seen.insert(n).second) queue.push_back(n);
            }
            components.push_back(std::move(component));
        }
        for (std::size_t i = 1; i < components.size(); ++i) {
            const auto fresh = regions.next_id++;
            for (const auto c : components[i]) {
                regions.members[id].erase(c);
                regions.members[fresh].insert(c);
                regions.owner[c] = fresh;
            }
            ++stats.splits;
        }
    }
}

WorldDump Engine::Impl::dump() const {
    WorldDump d;
    d.time = world.time;
    d.next_suborder = world.next_suborder;
    d.next_entity_id = world.next_entity_id;
    d.next_block_entity_order = world.next_block_entity_order;
    std::vector<std::pair<std::uint64_t, BlockEvent>> events;
    const auto add = [&](const ChunkData& data, ChunkLevel level) {
        d.chunks.push_back({data.pos, data.generation.value, level});
        for (std::size_t s = 0; s < data.sections.size(); ++s) {
            if (!data.sections[s]) continue;
            for (std::uint32_t i = 0; i < 4096; ++i) {
                const auto state = data.sections[s]->blocks[i];
                if (state.is_air()) continue;
                d.blocks.push_back({BlockPos{data.pos.x * 16 + static_cast<std::int32_t>(i & 15),
                                             bundle.world.min_y + static_cast<std::int32_t>(s) * 16 + static_cast<std::int32_t>((i >> 8) & 15),
                                             data.pos.z * 16 + static_cast<std::int32_t>((i >> 4) & 15)},
                                    state.raw()});
            }
        }
        for (const auto& [pos, be] : data.block_entities) d.block_entities.push_back({pos, be});
        for (const auto& [id, e] : data.entities) d.entities.push_back(e);
        for (int l = 0; l < 2; ++l)
            for (const auto& [k, e] : data.ticks[l])
                d.ticks.push_back({static_cast<TickList>(l), {k.first, k.second, e.due, e.priority, e.suborder}});
        for (const auto& e : data.events) events.push_back({e.seq, e.event});
    };
    for (const auto& [c, data] : world.loaded) add(*data, data->level);
    for (const auto& [c, data] : world.saved) add(*data, ChunkLevel::unloaded);
    std::ranges::sort(events, {}, [](const auto& p) { return p.first; });
    for (const auto& [seq, e] : events) d.block_events.push_back(e);
    d.tickets = world.tickets.list();
    gameplay::canonicalize(d);
    return d;
}

Engine::Engine(const contracts::VersionBundle& bundle, std::uint64_t seed, EngineOptions options)
    : impl_(std::make_unique<Impl>(bundle, seed, std::move(options))) {}
Engine::~Engine() = default;
void Engine::advance(const std::vector<Command>& commands) { impl_->advance(commands); }
contracts::Tick Engine::time() const noexcept { return impl_->world.time; }
const std::vector<TraceRecord>& Engine::trace() const noexcept { return impl_->trace; }
WorldDump Engine::dump() const { return impl_->dump(); }
const EngineStats& Engine::stats() const noexcept { return impl_->stats; }
std::size_t Engine::region_count() const noexcept { return impl_->regions.members.size(); }
std::size_t Engine::loaded_chunks() const noexcept { return impl_->world.loaded.size(); }
WorldView Engine::view() const noexcept { return WorldView(this, impl_->epoch, impl_->world.time); }

std::optional<ColumnSnapshot> Engine::column(ChunkPos pos) const {
    const auto* data = impl_->world.find_loaded(pos);
    if (!data) return std::nullopt;
    ColumnSnapshot s{pos, data->generation, data->level, {}, {}, {}};
    s.sections.reserve(data->sections.size());
    for (const auto& section : data->sections)
        // Aliasing pointer: shares ownership of the section, so the engine copies
        // it before its next write instead of changing the snapshot.
        s.sections.push_back(section ? std::shared_ptr<const ColumnSnapshot::Blocks>(section, &section->blocks) : nullptr);
    for (const auto& [p, be] : data->block_entities) s.block_entities.emplace_back(p, be);
    for (const auto& [id, e] : data->entities) s.entities.push_back(e);
    return s;
}

std::vector<std::pair<ChunkPos, gameplay::ChunkLevel>> Engine::loaded_columns() const {
    std::vector<std::pair<ChunkPos, gameplay::ChunkLevel>> out;
    for (const auto& [c, data] : impl_->world.loaded) out.emplace_back(c, data->level);
    return out;
}

contracts::Result<gameplay::BlockState> WorldView::block(BlockPos pos) const {
    const auto& impl = *engine_->impl_;
    if (impl.epoch != epoch_) return std::unexpected(contracts::ErrorCode::stale_view);
    if (!gameplay::in_world(impl.bundle, pos)) return std::unexpected(contracts::ErrorCode::out_of_bounds);
    const auto* data = impl.world.find_loaded(contracts::chunk_of(pos));
    if (!data) return std::unexpected(contracts::ErrorCode::unloaded_chunk);
    const auto& section = data->sections[static_cast<std::size_t>((pos.y - impl.bundle.world.min_y) >> 4)];
    return section ? section->blocks[contracts::section_local_index(pos)] : gameplay::blocks::air();
}

contracts::Result<std::optional<gameplay::BlockEntity>> WorldView::block_entity(BlockPos pos) const {
    const auto& impl = *engine_->impl_;
    if (impl.epoch != epoch_) return std::unexpected(contracts::ErrorCode::stale_view);
    const auto* data = impl.world.find_loaded(contracts::chunk_of(pos));
    if (!data) return std::unexpected(contracts::ErrorCode::unloaded_chunk);
    const auto it = data->block_entities.find(pos);
    if (it == data->block_entities.end()) return std::optional<gameplay::BlockEntity>{};
    return std::optional<gameplay::BlockEntity>{it->second};
}

contracts::Result<std::optional<gameplay::Entity>> WorldView::entity(EntityId id) const {
    const auto& impl = *engine_->impl_;
    if (impl.epoch != epoch_) return std::unexpected(contracts::ErrorCode::stale_view);
    const auto it = impl.world.entity_index.find(id);
    if (it == impl.world.entity_index.end()) return std::optional<gameplay::Entity>{};
    const auto* data = impl.world.find_loaded(it->second);
    if (!data) return std::unexpected(contracts::ErrorCode::unloaded_chunk);
    const auto e = data->entities.find(id);
    if (e == data->entities.end()) return std::optional<gameplay::Entity>{};
    return std::optional<gameplay::Entity>{e->second};
}

} // namespace circuit::runtime
