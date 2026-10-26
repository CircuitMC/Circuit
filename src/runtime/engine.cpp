// SPDX-License-Identifier: AGPL-3.0-only
#include "region_context.hpp"
#include <circuit/gameplay/light.hpp>
#include <circuit/gameplay/rng.hpp>
#include <circuit/gameplay/rules.hpp>
#include <circuit/gameplay/sky.hpp>
#include <circuit/runtime/engine.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

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
    void light_phase(DimensionId level);
    void command_phase(const std::vector<Command>& commands);
    void chunk_phase(DimensionId level);
    void apply_levels(RegionContext& ctx, DimensionId level);
    void tick_phase(DimensionId level, TickList list);
    void random_phase(DimensionId level);
    void block_event_phase(DimensionId level);
    void entity_phase(DimensionId level);
    void block_entity_phase(DimensionId level);
    // Whether the level ticks entities and block entities this tick (idle rule).
    bool awake(DimensionId level);
    void run_phase(Phase phase, DimensionId level, std::vector<RootItem> roots);
    void commit(const std::vector<RegionContext*>& contexts);
    void maintain_layout();
    void split_pass();
    RegionId region_for_new_chunk(ChunkKey c);
    WorldDump dump() const;
    void restore(const WorldDump& state);

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
    std::map<std::pair<ChunkKey, ChunkKey>, Tick> edges; // Last tick two columns interacted.
    gameplay::Rng layout_rng;
    gameplay::Rng weather_rng{0}; // this tick's weather stream
    // Light of each level, brought up to date when read: the loaded columns
    // as they were at the start of the tick, the sections the light was last
    // brought up to date with (copy-on-write: a written section is a new
    // one), and the tick it was.
    using Sections = std::map<ChunkKey, std::vector<std::shared_ptr<Section>>>;
    Sections start;
    std::array<std::optional<gameplay::LightField>, contracts::dimension_count> light;
    Sections lit;
    std::array<Tick, contracts::dimension_count> lit_time{};
    std::mutex light_mutex; // light brought up to date for rules of parallel phases
    void update_light(DimensionId level);
};

namespace {
// The blocks a level's light is computed from: its columns as they were at
// the start of the tick, with the states some cells had before changes the
// light has not followed yet.
class LightBlocks final : public gameplay::LightWorld {
public:
    LightBlocks(const Engine::Impl::Sections& start, DimensionId level, std::int32_t min_y) : start_(start), level_(level), min_y_(min_y) {}
    struct PosHash {
        std::size_t operator()(BlockPos p) const noexcept {
            return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) * 0x9E3779B97F4A7C15ull
                                              ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) << 20 ^ static_cast<std::uint32_t>(p.y));
        }
    };
    std::unordered_map<BlockPos, gameplay::BlockState, PosHash> before;
    gameplay::BlockState block(BlockPos pos) const override {
        if (!before.empty())
            if (const auto it = before.find(pos); it != before.end()) return it->second;
        const auto* sections = column(contracts::chunk_of(pos));
        if (!sections) return gameplay::BlockState::unloaded_sentinel();
        const auto s = static_cast<std::size_t>((pos.y - min_y_) >> 4);
        if (s >= sections->size() || !(*sections)[s]) return gameplay::blocks::air();
        return (*sections)[s]->blocks[contracts::section_local_index(pos)];
    }
    bool loaded(ChunkPos chunk) const override { return column(chunk) != nullptr; }
    bool empty(ChunkPos chunk, std::int32_t section) const override {
        const auto* sections = column(chunk);
        return sections && static_cast<std::size_t>(section) < sections->size() && !(*sections)[static_cast<std::size_t>(section)];
    }

private:
    const std::vector<std::shared_ptr<Section>>* column(ChunkPos chunk) const {
        if (cached_ && chunk == cached_chunk_) return cached_;
        const auto it = start_.find({level_, chunk});
        if (it == start_.end()) return nullptr;
        cached_chunk_ = chunk;
        return cached_ = &it->second;
    }
    const Engine::Impl::Sections& start_;
    DimensionId level_;
    std::int32_t min_y_;
    mutable const std::vector<std::shared_ptr<Section>>* cached_{};
    mutable ChunkPos cached_chunk_;
};
} // namespace

namespace {
class RegionTask final : public Task {
public:
    enum class Outcome { running, done, conflict, injected, failed };

    RegionTask(Engine::Impl& engine, RegionId region, Phase phase, DimensionId level, std::vector<RootItem> roots)
        : region(region), phase(phase), roots(std::move(roots)), engine_(engine) {
        ctx = std::make_unique<RegionContext>(engine.bundle, engine.world, engine.regions, region, phase, level,
                                              engine.options.record_trace, engine.options.unchecked_ownership_for_testing,
                                              engine.options.data.get());
        if (phase == Phase::block_events)
            for (const auto& r : this->roots) ctx->push_local(r.event);
        if (phase == Phase::block_ticks || phase == Phase::fluid_ticks)
            for (const auto& r : this->roots) ctx->pending_ticks.insert({r.tick.pos, r.tick.block});
    }

    bool step() override {
        if (outcome != Outcome::running) return false;
        // Time spent in rule code, when measured: the useful work of the phase.
        struct Timer {
            RegionTask& t;
            std::chrono::steady_clock::time_point start;
            ~Timer() {
                t.nanoseconds += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            }
        };
        std::optional<Timer> timer;
        if (engine_.options.measure_tasks) timer.emplace(Timer{*this, std::chrono::steady_clock::now()});
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
    ChunkKey conflict_chunk{};
    std::exception_ptr error;
    std::uint64_t nanoseconds{}; // in step(), with EngineOptions::measure_tasks

private:
    void execute(const RootItem& r) {
        ctx->begin_root(r.key, r.home);
        if (phase == Phase::block_ticks || phase == Phase::fluid_ticks) ctx->pending_ticks.erase({r.tick.pos, r.tick.block});
        switch (phase) {
        case Phase::block_ticks: gameplay::run_scheduled_tick(*ctx, TickList::block, r.tick); break;
        case Phase::fluid_ticks: gameplay::run_scheduled_tick(*ctx, TickList::fluid, r.tick); break;
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
    // Each level's random sequence: seeded from the world seed, or as the
    // options say; a restored state replaces it. Its pseudo column joins the
    // region map.
    for (const auto level : contracts::level_order) {
        world.counters(level).random = gameplay::initial_level_random(seed, level, options.level_random_seed);
        world.counters(level).random_tick = gameplay::initial_random_tick(seed, level, options.level_random_seed);
        region_for_new_chunk(random_key(level));
    }
    world.sequence_seed = options.sequence_seed.value_or(static_cast<std::int64_t>(seed));
    // Rules of the phases that do not publish the light bring it up to date
    // when they first read it; the result depends only on the blocks at the
    // start of the tick, whichever root asks first.
    world.light_now = [this](DimensionId level) -> const gameplay::LightField* {
        const std::lock_guard lock(light_mutex);
        update_light(level);
        const auto& field = light[static_cast<std::size_t>(level)];
        return field ? &*field : nullptr;
    };
}

void Engine::Impl::advance(const std::vector<Command>& commands) {
    if (failed) throw std::logic_error("engine: a previous tick failed");
    if (world.time == UINT64_MAX) throw std::overflow_error("engine: time exhausted");
    failed = true; // Cleared only when the whole tick completes.
    ++epoch;
    ++world.time;
    weather_rng = gameplay::weather_stream(seed, world.time);
    trace.clear();
    const auto timed = [this](contracts::Phase phase, auto&& run) {
        const auto start = std::chrono::steady_clock::now();
        run();
        stats.phase_nanoseconds[static_cast<std::size_t>(phase)] += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    };
    // Light follows the blocks as they were at the end of the previous tick,
    // as on the official servers: they are kept, and the light catches up
    // with them when read.
    start.clear();
    for (const auto& [c, data] : world.loaded) start.emplace(c, data->sections);
    timed(Phase::commands, [&] { command_phase(commands); });
    // The weather, then the day time, advance before the levels run (both
    // official servers).
    gameplay::advance_weather(world.weather, gameplay::game_rule_value(bundle, world.game_rules, "doWeatherCycle") != 0, weather_rng);
    // The sky darkens by the weather and the day time before it advances.
    world.sky_darken = gameplay::sky_darken(world.weather, world.day_time);
    if (gameplay::game_rule_value(bundle, world.game_rules, "doDaylightCycle") != 0) ++world.day_time;
    // Then each level, in level order, runs its phases: the scheduled ticks
    // of its loaded chunks, its chunk stage, and the rest where chunks are
    // loaded.
    const auto loaded = [&](DimensionId level) {
        bool any = false;
        world.for_each_loaded(level, [&](ChunkPos, const ChunkData&) { any = true; });
        return any;
    };
    for (const auto level : contracts::level_order) {
        if (loaded(level)) {
            timed(Phase::block_ticks, [&] { tick_phase(level, TickList::block); });
            timed(Phase::fluid_ticks, [&] { tick_phase(level, TickList::fluid); });
        }
        timed(Phase::chunks, [&] { chunk_phase(level); });
        if (!loaded(level)) continue;
        const bool level_awake = awake(level);
        timed(Phase::random_ticks, [&] { random_phase(level); });
        timed(Phase::block_events, [&] { block_event_phase(level); });
        // An idle level ticks neither entities nor block entities.
        if (level_awake) timed(Phase::entities, [&] { entity_phase(level); });
        else ++stats.phases;
        if (level_awake) timed(Phase::block_entities, [&] { block_entity_phase(level); });
        else ++stats.phases;
    }
    maintain_layout();
    failed = false;
}

void Engine::Impl::update_light(DimensionId level) {
    auto& when = lit_time[static_cast<std::size_t>(level)];
    if (when == world.time) return;
    const auto clock = std::chrono::steady_clock::now();
    light_phase(level);
    when = world.time;
    stats.light_nanoseconds += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - clock).count());
}

void Engine::Impl::light_phase(DimensionId level) {
    auto& field = light[static_cast<std::size_t>(level)];
    LightBlocks blocks(start, level, bundle.shape(level).min_y);
    const auto columns = [&](auto&& f) {
        for (auto it = start.lower_bound({level, {INT32_MIN, INT32_MIN}}); it != start.end() && it->first.dimension == level; ++it)
            f(it->first.pos, it->second);
    };
    if (!field) {
        bool any = false;
        columns([&](ChunkPos, const auto&) { any = true; });
        if (!any) return;
        field.emplace(bundle.shape(level).min_y, bundle.shape(level).height);
    }
    // Columns that left.
    for (const auto c : field->columns())
        if (!blocks.loaded(c)) {
            field->unload(c, blocks);
            lit.erase({level, c});
        }
    // Cells that changed in columns the light already follows, with their
    // states before, followed one by one.
    const auto min_y = bundle.shape(level).min_y;
    columns([&](ChunkPos c, const std::vector<std::shared_ptr<Section>>& sections) {
        const auto it = lit.find({level, c});
        if (it == lit.end()) return;
        for (std::size_t s = 0; s < sections.size(); ++s) {
            const auto& now = sections[s];
            const auto& then = it->second[s];
            if (now == then) continue;
            for (std::uint32_t i = 0; i < 4096; ++i) {
                const auto a = then ? then->blocks[i] : gameplay::blocks::air();
                const auto b = now ? now->blocks[i] : gameplay::blocks::air();
                if (a == b) continue;
                const BlockPos p{c.x * 16 + static_cast<std::int32_t>(i & 15), min_y + static_cast<std::int32_t>(s) * 16 + static_cast<std::int32_t>(i >> 8),
                                 c.z * 16 + static_cast<std::int32_t>((i >> 4) & 15)};
                blocks.before.emplace(p, a);
            }
        }
    });
    // In position order, so that the work does not depend on hashing.
    std::vector<std::pair<BlockPos, gameplay::BlockState>> changes(blocks.before.begin(), blocks.before.end());
    std::ranges::sort(changes, {}, &std::pair<BlockPos, gameplay::BlockState>::first);
    for (const auto& [p, state] : changes) {
        blocks.before.erase(p);
        field->change(p, state, blocks);
    }
    // Columns that joined.
    columns([&](ChunkPos c, const auto&) {
        if (!field->has(c)) field->load(c, blocks);
    });
    columns([&](ChunkPos c, const std::vector<std::shared_ptr<Section>>& sections) { lit[{level, c}] = sections; });
}

void Engine::Impl::command_phase(const std::vector<Command>& commands) {
    ++stats.phases;
    // Commands that use blocks read the light as of the start of the tick.
    if (!commands.empty())
        for (const auto level : contracts::level_order) {
            update_light(level);
            world.light[static_cast<std::size_t>(level)] = light[static_cast<std::size_t>(level)] ? &*light[static_cast<std::size_t>(level)] : nullptr;
        }
    // Commands run serially on the coordinator with access to every column.
    RegionContext ctx(bundle, world, regions, all_regions, Phase::commands, DimensionId::overworld, options.record_trace, false,
                      options.data.get());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& c = commands[i];
        ctx.use_level(c.dimension);
        ctx.begin_root(i, {});
        contracts::Result<void> ticket_result;
        if (!contracts::valid_dimension(static_cast<std::uint8_t>(c.dimension)))
            ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
        else if (const auto* add = std::get_if<contracts::AddTicket>(&c.body))
            ticket_result = world.tickets.add(*add, c.dimension, world.time, bundle.limits);
        else if (const auto* rem = std::get_if<contracts::RemoveTicket>(&c.body)) ticket_result = world.tickets.remove(rem->ticket);
        else if (const auto* presence = std::get_if<contracts::PlayerPresence>(&c.body)) {
            // A player is in at most one level.
            for (auto& l : world.levels) l.presences.erase(presence->player);
            if (presence->present)
                world.counters(c.dimension).presences[presence->player] = {presence->player, presence->position, presence->spectator};
        } else if (std::holds_alternative<contracts::LevelsStarted>(c.body)) {
            world.started = world.time - 1;
            gameplay::restart_weather(world.weather);
        } else if (const auto* rule = std::get_if<contracts::SetGameRule>(&c.body)) {
            ticket_result = gameplay::check_game_rule(bundle, *rule);
            if (ticket_result) gameplay::set_game_rule(bundle, world.game_rules, *rule);
        } else if (const auto* day = std::get_if<contracts::SetDayTime>(&c.body)) {
            if (day->time < 0) ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
            else world.day_time = day->time;
        } else if (const auto* set = std::get_if<contracts::SetDifficulty>(&c.body)) {
            if (set->difficulty > contracts::max_difficulty) ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
            else world.difficulty = set->difficulty;
        } else if (const auto* weather = std::get_if<contracts::SetWeather>(&c.body)) {
            if (static_cast<std::uint8_t>(weather->kind) >= contracts::weather_kind_count || weather->duration < 0)
                ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
            else gameplay::set_weather(world.weather, *weather, weather_rng);
        } else {
            gameplay::apply_command(ctx, c);
            continue;
        }
        if (ticket_result) ctx.record(TraceKind::command_applied, {}, c.admission, 0, 0);
        else ctx.record(TraceKind::command_rejected, {}, c.admission, static_cast<std::uint64_t>(ticket_result.error()), 0);
    }
    world.light.fill(nullptr);
    commit({&ctx});
}

// The level's chunk stage: tickets whose lifetime is over go, then chunk
// levels change, as in the target versions after the scheduled ticks.
void Engine::Impl::chunk_phase(DimensionId level) {
    ++stats.phases;
    world.tickets.expire(world.time, level);
    RegionContext ctx(bundle, world, regions, all_regions, Phase::chunks, level, options.record_trace, false);
    ctx.begin_root(0, {});
    apply_levels(ctx, level);
    commit({&ctx});
}

RegionId Engine::Impl::region_for_new_chunk(ChunkKey c) {
    if (options.layout == LayoutPolicy::single_region) {
        // One region per level.
        for (auto& [id, members] : regions.members)
            if (!members.empty() && members.begin()->dimension == c.dimension) {
                regions.owner[c] = id;
                members.insert(c);
                return id;
            }
    }
    return regions.add_singleton(c);
}

void Engine::Impl::apply_levels(RegionContext& ctx, DimensionId level) {
    const auto desired = world.tickets.levels(level);
    std::set<ChunkKey> keys;
    world.for_each_loaded(level, [&](ChunkPos c, const ChunkData&) { keys.insert({level, c}); });
    for (const auto& [c, levels] : desired) keys.insert(c);
    for (const auto c : keys) {
        auto* data = world.find_loaded(c);
        const auto current = data ? data->level : ChunkLevel::unloaded;
        const auto it = desired.find(c);
        const auto levels = it == desired.end() ? gameplay::ChunkLevels{} : it->second;
        const auto want = gameplay::status_of(levels.load);
        if (data) data->levels = levels; // the simulation level may change without the status
        if (current == want) continue;
        const auto origin = contracts::chunk_origin(c.pos);
        ctx.use_level(c.dimension);
        if (!data) {
            std::unique_ptr<ChunkData> chunk;
            if (auto saved = world.saved.find(c); saved != world.saved.end()) {
                chunk = std::move(saved->second);
                world.saved.erase(saved);
                const auto next = chunk->generation.next();
                if (!next) throw std::overflow_error("engine: chunk generation exhausted");
                chunk->generation = *next;
                // A reload: its pending ticks wait out their remaining delay.
                for (auto& list : chunk->ticks)
                    for (auto& [key, t] : list) t.due = gameplay::reload_shift(t.due, chunk->saved_at, world.time);
                chunk->saved_at = 0;
                chunk->resumes = world.time + gameplay::reload_resume;
                // Its entities come back in the next tick (gameplay::entity_return).
                for (auto& [id, e] : chunk->entities) {
                    e.returns = world.time + gameplay::entity_return;
                    chunk->returning.insert_or_assign(id, std::move(e));
                }
                chunk->entities.clear();
                for (auto& [id, e] : chunk->returning) e.returns = world.time + gameplay::entity_return;
            } else {
                chunk = std::make_unique<ChunkData>();
                chunk->dimension = c.dimension;
                chunk->pos = c.pos;
                chunk->sections.resize(static_cast<std::size_t>(bundle.shape(c.dimension).sections()));
                if (options.generator) {
                    auto& sections = chunk->sections;
                    options.generator(c, [&sections](std::size_t s) -> std::array<gameplay::BlockState, 4096>& {
                        if (!sections.at(s)) sections[s] = std::make_shared<Section>();
                        return sections[s]->blocks;
                    });
                    for (auto& section : sections)
                        if (section) count_random_ticking(*section);
                }
            }
            chunk->level = want;
            chunk->levels = levels;
            ctx.record(TraceKind::chunk_loaded, origin, chunk->generation.value, static_cast<std::uint64_t>(want), 0);
            world.loaded.emplace(c, std::move(chunk));
            region_for_new_chunk(c);
        } else if (want == ChunkLevel::unloaded) {
            ctx.record(TraceKind::chunk_unloaded, origin, data->generation.value, 0, 0);
            // Its entities leave the level with it (saved, not removed).
            for (const auto& [id, e] : data->entities) {
                ctx.record(TraceKind::entity_removed, contracts::block_of(e.position), id.value,
                           static_cast<std::uint64_t>(gameplay::RemovalReason::unloaded), 0);
                world.entity_index.erase(id);
            }
            data->level = ChunkLevel::unloaded;
            data->levels = {};
            data->saved_at = world.time + 1;
            data->resumes = 0;
            auto node = world.loaded.extract(c);
            world.saved.insert(std::move(node));
            regions.remove(c);
        } else {
            ctx.record(TraceKind::chunk_level_changed, origin, static_cast<std::uint64_t>(current),
                       static_cast<std::uint64_t>(want), 0);
            data->level = want;
        }
    }
    // Entities a reload brought back return now, column by column, in the
    // order of their old ids, under new ids and with their tick counts
    // starting again (gameplay::entity_return).
    for (auto it = world.loaded.lower_bound({level, {INT32_MIN, INT32_MIN}}); it != world.loaded.end() && it->first.dimension == level; ++it) {
        auto& data = *it->second;
        for (auto e = data.returning.begin(); e != data.returning.end();) {
            if (e->second.returns > world.time) {
                ++e;
                continue;
            }
            auto entity = std::move(e->second);
            const auto old = e->first;
            e = data.returning.erase(e);
            entity.id = EntityId{world.next_entity_id++};
            entity.returns = 0;
            entity.tick_count = 0;
            const auto cell = contracts::block_of(entity.position);
            ctx.use_level(level);
            ctx.record(TraceKind::entity_loaded, cell, entity.id.value, old.value, gameplay::entity_payload(entity));
            ctx.record(TraceKind::entity_spawn_position, cell, contracts::double_bits(entity.position.x), contracts::double_bits(entity.position.y),
                       contracts::double_bits(entity.position.z));
            ctx.record(TraceKind::entity_spawn_velocity, cell, contracts::double_bits(entity.velocity.x), contracts::double_bits(entity.velocity.y),
                       contracts::double_bits(entity.velocity.z));
            world.entity_index[entity.id] = it->first;
            data.entities.emplace(entity.id, std::move(entity));
        }
    }
}

void Engine::Impl::tick_phase(DimensionId level, TickList list) {
    const auto l = static_cast<int>(list);
    std::vector<gameplay::ScheduledTick> due;
    world.for_each_loaded(level, [&](ChunkPos, const ChunkData& data) {
        if (!data.block_ticking() || world.time < data.resumes) return;
        for (const auto& [key, e] : data.ticks[l])
            if (e.due <= world.time) due.push_back({key.first, key.second, e.due, e.priority, e.suborder});
    });
    std::ranges::sort(due, gameplay::tick_before);
    if (due.size() > bundle.limits.max_scheduled_ticks_per_list) due.resize(bundle.limits.max_scheduled_ticks_per_list);
    std::vector<RootItem> roots;
    roots.reserve(due.size());
    for (std::size_t i = 0; i < due.size(); ++i) {
        const auto c = contracts::chunk_of(due[i].pos);
        world.loaded.at({level, c})->ticks[l].erase({due[i].pos, due[i].block});
        RootItem r;
        r.key = i;
        r.home = c;
        r.tick = due[i];
        roots.push_back(r);
    }
    run_phase(list == TickList::block ? Phase::block_ticks : Phase::fluid_ticks, level, std::move(roots));
}

// The level's random ticks, as the target versions run them: chunk by chunk
// in their order (gameplay::random_tick_order), each drawing from the level
// random and the level's position generator. Both are level-wide, so the
// phase runs serially on the coordinator with access to every column.
void Engine::Impl::random_phase(DimensionId level) {
    if (gameplay::game_rule_value(bundle, world.game_rules, "randomTickSpeed") <= 0) return;
    const auto sections = bundle.shape(level).sections();
    const auto& presences = world.counters(level).presences;
    std::vector<ChunkPos> ticking;
    world.for_each_loaded(level, [&](ChunkPos c, const ChunkData& data) {
        if (gameplay::random_ticks_run(bundle.rules->random_ticks, data.levels.simulation, c, presences)) ticking.push_back(c);
    });
    if (ticking.empty()) return;
    ++stats.phases;
    // Rules that read light see it as of the start of the tick.
    update_light(level);
    world.light[static_cast<std::size_t>(level)] = light[static_cast<std::size_t>(level)] ? &*light[static_cast<std::size_t>(level)] : nullptr;
    RegionContext ctx(bundle, world, regions, all_regions, Phase::random_ticks, level, options.record_trace, false, options.data.get());
    std::uint64_t index = 0;
    for (const auto c : gameplay::random_tick_order(std::move(ticking))) {
        ctx.begin_root(index++, c);
        gameplay::run_random_ticks(ctx, c, sections);
    }
    commit({&ctx});
    world.light[static_cast<std::size_t>(level)] = nullptr;
}

void Engine::Impl::block_event_phase(DimensionId level) {
    std::vector<RootItem> roots;
    world.for_each_loaded(level, [&](ChunkPos c, ChunkData& data) {
        if (!data.block_ticking()) return;
        for (const auto& e : data.events) {
            RootItem r;
            r.key = e.seq;
            r.home = c;
            r.event = e.event;
            roots.push_back(r);
        }
        data.events.clear(); // Deferred events in non-ticking columns stay where they are.
    });
    std::ranges::sort(roots, {}, &RootItem::key);
    run_phase(Phase::block_events, level, std::move(roots));
}

void Engine::Impl::entity_phase(DimensionId level) {
    std::vector<RootItem> roots;
    world.for_each_loaded(level, [&](ChunkPos c, const ChunkData& data) {
        if (!data.entity_ticking() || world.time < data.resumes) return;
        for (const auto& [id, e] : data.entities) {
            RootItem r;
            r.key = id.value;
            r.home = c;
            r.entity = id;
            roots.push_back(r);
        }
    });
    std::ranges::sort(roots, {}, &RootItem::key);
    run_phase(Phase::entities, level, std::move(roots));
}

void Engine::Impl::block_entity_phase(DimensionId level) {
    std::vector<RootItem> roots;
    bool reads_light = false;
    world.for_each_loaded(level, [&](ChunkPos c, const ChunkData& data) {
        if (!data.block_ticking() || world.time < data.resumes) return;
        for (const auto& [pos, be] : data.block_entities) {
            RootItem r;
            r.key = be.order;
            r.home = c;
            r.pos = pos;
            roots.push_back(r);
            reads_light = reads_light || be.kind == gameplay::BlockType::daylight_detector;
        }
    });
    std::ranges::sort(roots, {}, &RootItem::key);
    // Daylight detectors read the light as of the start of the tick.
    if (reads_light) {
        update_light(level);
        world.light[static_cast<std::size_t>(level)] = light[static_cast<std::size_t>(level)] ? &*light[static_cast<std::size_t>(level)] : nullptr;
    }
    run_phase(Phase::block_entities, level, std::move(roots));
    world.light[static_cast<std::size_t>(level)] = nullptr;
}

bool Engine::Impl::awake(DimensionId level) {
    auto& state = world.counters(level);
    const bool active = !state.presences.empty() || world.tickets.keeps_active(level);
    if (active) state.last_active = world.time;
    return gameplay::level_awake(active, world.time, state.last_active, world.started);
}

void Engine::Impl::run_phase(Phase phase, DimensionId level, std::vector<RootItem> roots) {
    ++stats.phases;
    if (roots.empty()) return;
    std::map<RegionId, std::vector<RootItem>> groups;
    for (auto& r : roots) groups[regions.owner.at({level, r.home})].push_back(std::move(r));
    std::map<RegionId, std::unique_ptr<RegionTask>> staged;
    std::vector<RegionId> to_run;
    for (const auto& [id, list] : groups) to_run.push_back(id);
    std::uint32_t attempt = 0;
    while (!to_run.empty()) {
        std::vector<std::unique_ptr<RegionTask>> tasks;
        std::vector<Task*> pointers;
        for (const auto id : to_run) {
            tasks.push_back(std::make_unique<RegionTask>(*this, id, phase, level, groups.at(id)));
            pointers.push_back(tasks.back().get());
        }
        ++stats.attempts;
        stats.tasks += tasks.size();
        stats.max_tasks_in_phase = std::max<std::uint64_t>(stats.max_tasks_in_phase, tasks.size());
        const auto run_start = std::chrono::steady_clock::now();
        std::size_t root_count = 0;
        for (const auto id : to_run) root_count += groups.at(id).size();
        if (!options.runner && root_count <= options.inline_roots) {
            // Too little work to wake other threads: run the tasks here, in
            // order. Any order is a valid schedule.
            for (auto* t : pointers)
                while (t->step()) {
                }
            ++attempt;
        } else {
            runner->run(pointers, attempt++);
        }
        stats.runner_nanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - run_start).count());
        if (options.measure_tasks) {
            std::uint64_t longest = 0;
            for (const auto& t : tasks) {
                stats.work_nanoseconds += t->nanoseconds;
                longest = std::max(longest, t->nanoseconds);
            }
            stats.critical_nanoseconds += longest;
        }

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
    const auto commit_start = std::chrono::steady_clock::now();
    commit(contexts);
    stats.commit_nanoseconds += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - commit_start).count());
}

void Engine::Impl::commit(const std::vector<RegionContext*>& contexts) {
    // Level-wide counters are assigned in serial order: by root, then by action
    // within the root. Every staged creation consumes a value, including those
    // of objects removed again before commit, as in the serial order.
    // values[counter][context][ledger index] holds the assigned value. Each
    // creation draws from its level's counter, entity ids from the global one.
    std::array<std::vector<std::vector<std::uint64_t>>, counter_kinds> values;
    const auto next = [&](std::size_t k, DimensionId level) -> std::uint64_t& {
        auto& c = world.counters(level);
        switch (static_cast<Counter>(k)) {
        case Counter::suborder: return c.next_suborder;
        case Counter::event_seq: return c.next_event_seq;
        case Counter::block_entity_order: return c.next_block_entity_order;
        case Counter::entity_id: break;
        }
        return world.next_entity_id;
    };
    for (std::size_t k = 0; k < counter_kinds; ++k) {
        struct Ref {
            std::uint64_t root;
            std::uint32_t seq;
            std::size_t context;
            std::size_t index;
            DimensionId level;
        };
        std::vector<Ref> refs;
        values[k].resize(contexts.size());
        for (std::size_t i = 0; i < contexts.size(); ++i) {
            const auto& entries = contexts[i]->ledger[k];
            values[k][i].resize(entries.size());
            for (std::size_t j = 0; j < entries.size(); ++j) refs.push_back({entries[j].root, entries[j].seq, i, j, entries[j].level});
        }
        std::ranges::sort(refs, [](const Ref& a, const Ref& b) { return a.root != b.root ? a.root < b.root : a.seq < b.seq; });
        for (const auto& r : refs) {
            auto& counter = next(k, r.level);
            if (counter == UINT64_MAX) throw std::overflow_error("engine: level-wide counter exhausted");
            values[k][r.context][r.index] = counter++;
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
            // (entity_spawn_position and entity_spawn_velocity carry no id.)
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
            std::vector<ChunkKey> all;
            for (const auto& [c, id] : regions.owner) all.push_back(c);
            std::ranges::sort(all);
            regions.owner.clear();
            regions.members.clear();
            for (const auto c : all) regions.add_singleton(c);
            stats.splits += all.size();
        } else if (choice == 1 && regions.members.size() > 1) {
            // Merge random pairs of each level, including distant ones.
            for (const auto level : contracts::level_order) {
                std::vector<RegionId> ids;
                for (const auto& [id, m] : regions.members)
                    if (!m.empty() && m.begin()->dimension == level) ids.push_back(id);
                for (std::size_t i = 0; i + 1 < ids.size(); i += 2) {
                    const auto j = i + 1 + layout_rng.bounded(static_cast<std::uint32_t>(ids.size() - i - 1));
                    std::swap(ids[i + 1], ids[j]);
                    regions.absorb(ids[i], ids[i + 1]);
                    ++stats.merges;
                }
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
        std::map<ChunkKey, std::vector<ChunkKey>> adjacency;
        for (const auto& [edge, last] : edges) {
            if (!members.contains(edge.first) || !members.contains(edge.second)) continue;
            adjacency[edge.first].push_back(edge.second);
            adjacency[edge.second].push_back(edge.first);
        }
        std::set<ChunkKey> seen;
        std::vector<std::vector<ChunkKey>> components;
        for (const auto start : members) {
            if (seen.contains(start)) continue;
            std::vector<ChunkKey> component;
            std::deque<ChunkKey> queue{start};
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

namespace {
// Fills a dump from the store: every column of every level, loaded or saved.
// `blocks` receives each column's sections (snapshots) or adds its blocks to
// the dump itself (nullptr).
void fill_dump(const WorldStore& world, const contracts::VersionBundle& bundle, WorldDump& d,
               std::map<ChunkKey, std::vector<std::shared_ptr<const ColumnSnapshot::Blocks>>>* sections) {
    d.time = world.time;
    d.next_entity_id = world.next_entity_id;
    d.started = world.started;
    d.game_rules = world.game_rules;
    d.day_time = world.day_time;
    d.difficulty = world.difficulty;
    d.weather = world.weather;
    d.sequence_settings = world.sequence_settings;
    std::map<DimensionId, std::vector<std::pair<std::uint64_t, BlockEvent>>> events;
    const auto add = [&](const ChunkData& data, ChunkLevel level) {
        auto& l = d.level(data.dimension);
        l.chunks.push_back({data.pos, data.generation.value, level, data.levels.load, data.levels.simulation,
                            level == ChunkLevel::unloaded ? data.saved_at : 0, data.resumes > world.time ? data.resumes : 0});
        if (sections) {
            auto& list = (*sections)[{data.dimension, data.pos}];
            list.reserve(data.sections.size());
            for (const auto& section : data.sections)
                list.push_back(section ? std::shared_ptr<const ColumnSnapshot::Blocks>(section, &section->blocks) : nullptr);
        } else {
            const auto min_y = bundle.shape(data.dimension).min_y;
            for (std::size_t s = 0; s < data.sections.size(); ++s) {
                if (!data.sections[s]) continue;
                for (std::uint32_t i = 0; i < 4096; ++i) {
                    const auto state = data.sections[s]->blocks[i];
                    if (state == gameplay::blocks::air()) continue;
                    l.blocks.push_back({BlockPos{data.pos.x * 16 + static_cast<std::int32_t>(i & 15),
                                                 min_y + static_cast<std::int32_t>(s) * 16 + static_cast<std::int32_t>((i >> 8) & 15),
                                                 data.pos.z * 16 + static_cast<std::int32_t>((i >> 4) & 15)},
                                        state.raw()});
                }
            }
        }
        for (const auto& [pos, be] : data.block_entities) l.block_entities.push_back({pos, be});
        for (const auto& [id, e] : data.entities) l.entities.push_back(e);
        for (const auto& [id, e] : data.returning) l.entities.push_back(e);
        for (int t = 0; t < 2; ++t)
            for (const auto& [k, e] : data.ticks[t])
                l.ticks.push_back({static_cast<TickList>(t), {k.first, k.second, e.due, e.priority, e.suborder}});
        for (const auto& e : data.events) events[data.dimension].push_back({e.seq, e.event});
        l.turn_offs.insert(l.turn_offs.end(), data.turn_offs.begin(), data.turn_offs.end());
    };
    for (const auto& [c, data] : world.loaded) add(*data, data->level);
    for (const auto& [c, data] : world.saved) add(*data, ChunkLevel::unloaded);
    for (const auto level : contracts::level_order) {
        const auto& state = world.counters(level);
        if (state.presences.empty() && state.last_active == 0) continue;
        auto& l = d.level(level);
        for (const auto& [key, p] : state.presences) l.presences.push_back(p);
        l.last_active = state.last_active;
    }
    for (auto& l : d.levels) {
        l.next_suborder = world.counters(l.dimension).next_suborder;
        l.next_block_entity_order = world.counters(l.dimension).next_block_entity_order;
        l.random = world.counters(l.dimension).random.state();
        l.random_tick = world.counters(l.dimension).random_tick;
        for (const auto& [id, sequence] : world.counters(l.dimension).sequences) l.sequences.emplace_back(id, sequence);
        auto& list = events[l.dimension];
        std::ranges::sort(list, {}, [](const auto& p) { return p.first; });
        for (const auto& [seq, e] : list) l.block_events.push_back(e);
    }
    d.tickets = world.tickets.list();
    for (const auto& [key, inv] : world.players) d.players.push_back(inv);
    gameplay::canonicalize(d);
}
} // namespace

WorldDump Engine::Impl::dump() const {
    WorldDump d;
    fill_dump(world, bundle, d, nullptr);
    return d;
}

WorldSnapshot Engine::snapshot() const {
    WorldSnapshot snap;
    std::map<ChunkKey, std::vector<std::shared_ptr<const ColumnSnapshot::Blocks>>> sections;
    fill_dump(impl_->world, impl_->bundle, snap.state, &sections);
    snap.sections.reserve(snap.state.levels.size());
    for (const auto& l : snap.state.levels) {
        auto& per_level = snap.sections.emplace_back();
        per_level.reserve(l.chunks.size());
        for (const auto& c : l.chunks) per_level.push_back(std::move(sections.at({l.dimension, c.pos})));
    }
    return snap;
}

Engine::Engine(const contracts::VersionBundle& bundle, std::uint64_t seed, EngineOptions options)
    : impl_(std::make_unique<Impl>(bundle, seed, std::move(options))) {}

Engine::Engine(const contracts::VersionBundle& bundle, std::uint64_t seed, const gameplay::WorldDump& state,
               EngineOptions options)
    : impl_(std::make_unique<Impl>(bundle, seed, std::move(options))) {
    impl_->restore(state);
}

void Engine::Impl::restore(const WorldDump& state) {
    const auto bad = [](std::string_view what) { throw std::invalid_argument(std::format("engine: invalid state: {}", what)); };
    world.time = state.time;
    world.next_entity_id = state.next_entity_id;
    world.started = state.started;
    for (const auto& [name, value] : state.game_rules)
        if (!gameplay::check_game_rule(bundle, {name, value})) bad("game rule");
    world.game_rules = state.game_rules;
    if (state.day_time < 0) bad("day time");
    world.day_time = state.day_time;
    if (state.difficulty > contracts::max_difficulty) bad("difficulty");
    world.difficulty = state.difficulty;
    if (!gameplay::valid_weather(state.weather)) bad("weather");
    world.weather = state.weather;
    world.sequence_settings = state.sequence_settings;
    // As the last tick computed it, from the day time before it advanced.
    world.sky_darken = gameplay::sky_darken(world.weather, world.day_time
                                                               - (gameplay::game_rule_value(bundle, world.game_rules, "doDaylightCycle") != 0 ? 1 : 0));
    // Valid entity stacks; entities of every level are checked against the
    // global id counter.
    std::optional<DimensionId> previous;
    std::set<EntityId> entity_ids; // over all levels, present or returning
    for (const auto& l : state.levels) {
        if (!contracts::valid_dimension(static_cast<std::uint8_t>(l.dimension))) bad("level");
        if (previous && *previous >= l.dimension) bad("levels out of order");
        previous = l.dimension;
        const auto level = l.dimension;
        const auto& shape = bundle.shape(level);
        auto& counters = world.counters(level);
        counters.next_suborder = l.next_suborder;
        counters.next_block_entity_order = l.next_block_entity_order;
        counters.last_active = l.last_active;
        if (l.random > gameplay::LevelRandom::mask) bad("level random");
        counters.random = gameplay::LevelRandom::from_state(l.random);
        counters.random_tick = l.random_tick;
        counters.sequences.clear();
        for (const auto& [id, sequence] : l.sequences)
            if (id.empty() || !counters.sequences.emplace(id, sequence).second) bad("random sequence");
        for (const auto& p : l.presences) {
            const bool elsewhere = std::ranges::any_of(world.levels, [&](const auto& other) { return other.presences.contains(p.player); });
            if (elsewhere || !counters.presences.emplace(p.player, p).second) bad("a player in two levels");
        }
        std::map<ChunkPos, ChunkData*> columns;
        for (const auto& c : l.chunks) {
            auto chunk = std::make_unique<ChunkData>();
            chunk->dimension = level;
            chunk->pos = c.pos;
            chunk->generation = contracts::Generation{c.generation};
            chunk->level = c.level;
            chunk->levels = {c.load, c.simulation};
            chunk->saved_at = c.saved_at;
            chunk->resumes = c.resumes;
            if ((c.level == ChunkLevel::unloaded && c.resumes != 0) || (c.level != ChunkLevel::unloaded && c.saved_at != 0))
                bad("chunk reload ticks");
            if (c.level != gameplay::status_of(c.load)) bad("chunk status and load level disagree");
            chunk->sections.resize(static_cast<std::size_t>(shape.sections()));
            auto* raw = chunk.get();
            if (!columns.emplace(c.pos, raw).second) bad("duplicate column");
            if (c.level == ChunkLevel::unloaded) {
                world.saved.emplace(ChunkKey{level, c.pos}, std::move(chunk));
            } else {
                world.loaded.emplace(ChunkKey{level, c.pos}, std::move(chunk));
                region_for_new_chunk({level, c.pos});
            }
        }
        const auto column = [&](BlockPos p) -> ChunkData& {
            if (!shape.contains_y(p.y) || !contracts::within_border(p)) bad("position outside the world");
            const auto it = columns.find(contracts::chunk_of(p));
            if (it == columns.end()) bad("position in an unknown column");
            return *it->second;
        };
        for (const auto& [pos, raw] : l.blocks) {
            const auto s = gameplay::BlockState::decode(raw);
            if (!s) bad("block state");
            if (*s == gameplay::blocks::air()) continue;
            auto& chunk = column(pos);
            auto& section = chunk.sections[static_cast<std::size_t>((pos.y - shape.min_y) >> 4)];
            if (!section) section = std::make_shared<Section>();
            section->blocks[contracts::section_local_index(pos)] = *s;
            if (gameplay::ticks_section_randomly(*s)) ++section->random_ticking;
        }
        for (const auto& [pos, be] : l.block_entities) {
            auto& chunk = column(pos);
            if (!chunk.block_entities.emplace(pos, be).second) bad("duplicate block entity");
        }
        // Entities may be above or below the world height (falling out of it),
        // within the bounds the rules can reach; everything else is checked at
        // full width so that ticking a restored state cannot overflow.
        // Below the world: falling blocks and primed TNT fall on until their
        // time is up (at most 32,767 ticks, under 2 blocks a tick once their
        // speed of at most 64 has worn off), other entities are gone a tick
        // after passing 64 below it.
        const double lowest = static_cast<double>(shape.min_y) - 131072.0;
        const double highest = static_cast<double>(shape.min_y) + shape.height + 4096.0;
        for (const auto& e : l.entities) {
            if (!contracts::within_border(e.position) || e.id.value == 0 || e.id.value >= world.next_entity_id) bad("entity");
            if (static_cast<std::uint8_t>(e.kind) == 0 || static_cast<std::uint8_t>(e.kind) >= gameplay::entity_kind_count) bad("entity kind");
            if (e.position.y < lowest || e.position.y >= highest) bad("entity height");
            if (!gameplay::valid_velocity(e.velocity)) bad("entity velocity");
            if (e.piston_tick > state.time || !contracts::finite(e.piston_push) || std::abs(e.piston_push.x) > 0.51 || std::abs(e.piston_push.y) > 0.51
                || std::abs(e.piston_push.z) > 0.51)
                bad("entity piston push");
            // A fall within the heights an entity can reach.
            if (!std::isfinite(e.fall_distance) || e.fall_distance < 0.0 || e.fall_distance > highest - lowest) bad("entity fall distance");
            // Each kind's own state, valid; the state of other kinds at its defaults.
            auto rest = e;
            switch (e.kind) {
            case gameplay::EntityKind::item:
                if (!gameplay::items::valid(e.stack.item) || e.stack.count < 1 || e.stack.count > gameplay::items::max_stack(e.stack)
                    || e.stack.damage > gameplay::items::max_damage(e.stack) || e.pickup_delay > gameplay::item_pickup_never)
                    bad("entity stack");
                if (!gameplay::valid_item_age(e.age)) bad("entity age");
                if (e.health < 1 || e.health > 32767) bad("item health"); // a short, as saved
                if (e.fire < 0 || e.fire > 32767) bad("item fire");
                rest.stack = {};
                rest.fire = 0;
                rest.age = 0;
                rest.pickup_delay = 0;
                rest.health = gameplay::item_health;
                break;
            case gameplay::EntityKind::falling_block:
                if (!gameplay::BlockState::decode(e.carried.raw()) || e.carried.is_air() || e.carried.is_unloaded()) bad("falling block state");
                rest.carried = {};
                rest.fall_time = 0;
                break;
            case gameplay::EntityKind::primed_tnt:
                if (e.fuse < -32768 || e.fuse > 32767) bad("primed TNT fuse"); // a short, as saved
                rest.fuse = 0;
                break;
            case gameplay::EntityKind::arrow:
                if (e.life < 0 || e.life > 32767 || e.fire < -32768 || e.fire > 32767) bad("arrow state");
                rest.in_ground = false;
                rest.life = 0;
                rest.fire = 0;
                break;
            case gameplay::EntityKind::boat:
                if (e.variant >= gameplay::boat_types.size() || e.status > 5 || !std::isfinite(e.last_dy) || std::abs(e.last_dy) > 64.0)
                    bad("boat state");
                rest.variant = 0;
                rest.status = 0;
                rest.last_dy = 0.0;
                break;
            case gameplay::EntityKind::minecart:
                if (!std::isfinite(e.yaw) || std::abs(e.yaw) >= 360.0f) bad("minecart yaw");
                rest.in_water = false;
                rest.yaw = 0.0f;
                rest.flipped = false;
                break;
            default: break;
            }
            if (!rest.stack.empty() || rest.age != 0 || rest.pickup_delay != 0 || rest.health != gameplay::item_health
                || rest.carried != gameplay::BlockState{} || rest.fall_time != 0 || rest.fuse != 0 || rest.in_ground || rest.life != 0
                || rest.fire != 0 || rest.variant != 0 || rest.status != 0 || rest.last_dy != 0.0 || rest.in_water || rest.yaw != 0.0f
                || rest.flipped)
                bad("entity state of another kind");
            const auto it = columns.find(contracts::chunk_of(contracts::block_of(e.position)));
            if (it == columns.end()) bad("entity in an unknown column");
            auto& chunk = *it->second;
            if (!entity_ids.insert(e.id).second) bad("duplicate entity id");
            if (e.returns != 0) {
                // Saved with its column and not back yet.
                if (e.returns <= state.time) bad("entity return");
                chunk.returning.emplace(e.id, e);
                continue;
            }
            chunk.entities.emplace(e.id, e);
            world.entity_index[e.id] = {level, chunk.pos};
        }
        for (const auto& t : l.ticks) {
            auto& chunk = column(t.tick.pos);
            const TickEntry entry{t.tick.due, t.tick.priority, t.tick.suborder, {}};
            if (!chunk.ticks[static_cast<int>(t.list)].emplace(TickKey{t.tick.pos, t.tick.block}, entry).second)
                bad("duplicate scheduled tick");
        }
        // Block events keep their queue order; their internal sequence restarts.
        for (const auto& e : l.block_events) column(e.pos).events.push_back({e, counters.next_event_seq++, {}});
        for (const auto& t : l.turn_offs) {
            if (t.second > state.time) bad("torch turn-off");
            column(t.first).turn_offs.push_back(t);
        }
    }
    for (const auto& t : state.tickets)
        if (!contracts::valid_dimension(static_cast<std::uint8_t>(t.dimension)) || !world.tickets.restore(t, bundle.limits))
            bad("ticket");
    const auto valid_stack = [](const contracts::ItemStack& s) {
        if (s.item == 0 && s.count == 0 && s.damage == 0 && s.components.empty()) return true;
        return gameplay::items::valid(s.item) && s.count >= 1 && s.count <= gameplay::items::max_stack(s)
            && s.damage <= gameplay::items::max_damage(s);
    };
    for (const auto& p : state.players) {
        if (!valid_stack(p.cursor) || !std::ranges::all_of(p.slots, valid_stack) || !std::ranges::all_of(p.table, valid_stack)
            || !std::ranges::all_of(p.cutter, valid_stack) || p.cutter_recipe < -1 || p.drag_stage > 1 || p.drag_type > 2
            || p.drag_slots.size() > 256 || std::ranges::any_of(p.drag_slots, [](std::int16_t s) { return s < 0 || s >= 256; }))
            bad("player inventory");
        if (!world.players.emplace(p.player, p).second) bad("duplicate player");
    }
}
Engine::~Engine() = default;
void Engine::advance(const std::vector<Command>& commands) { impl_->advance(commands); }
contracts::Tick Engine::time() const noexcept { return impl_->world.time; }
std::int64_t Engine::day_time() const noexcept { return impl_->world.day_time; }
std::uint8_t Engine::difficulty() const noexcept { return impl_->world.difficulty; }
const gameplay::Weather& Engine::weather() const noexcept { return impl_->world.weather; }
const std::vector<TraceRecord>& Engine::trace() const noexcept { return impl_->trace; }
WorldDump Engine::dump() const { return impl_->dump(); }
const EngineStats& Engine::stats() const noexcept { return impl_->stats; }
std::size_t Engine::region_count() const noexcept { return impl_->regions.members.size(); }
std::size_t Engine::loaded_chunks() const noexcept { return impl_->world.loaded.size(); }
WorldView Engine::view() const noexcept { return WorldView(this, impl_->epoch, impl_->world.time); }

std::optional<ColumnSnapshot> Engine::column(ChunkKey key) const {
    const auto* data = impl_->world.find_loaded(key);
    if (!data) return std::nullopt;
    ColumnSnapshot s{key.dimension, key.pos, data->generation, data->level, {}, {}, {}, data->levels};
    s.sections.reserve(data->sections.size());
    for (const auto& section : data->sections)
        // Aliasing pointer: shares ownership of the section, so the engine copies
        // it before its next write instead of changing the snapshot.
        s.sections.push_back(section ? std::shared_ptr<const ColumnSnapshot::Blocks>(section, &section->blocks) : nullptr);
    for (const auto& [p, be] : data->block_entities) s.block_entities.emplace_back(p, be);
    for (const auto& [id, e] : data->entities) s.entities.push_back(e);
    return s;
}

std::pair<std::uint8_t, std::uint8_t> Engine::light(DimensionId level, BlockPos pos) const {
    impl_->update_light(level);
    const auto& field = impl_->light[static_cast<std::size_t>(level)];
    if (!field) return {0, 0};
    return {field->get(gameplay::LightKind::block, pos), field->get(gameplay::LightKind::sky, pos)};
}

std::optional<std::vector<Engine::SectionLight>> Engine::column_light(ChunkKey key) const {
    impl_->update_light(key.dimension);
    const auto& field = impl_->light[static_cast<std::size_t>(key.dimension)];
    if (!field || !field->has(key.pos)) return std::nullopt;
    std::vector<SectionLight> out(static_cast<std::size_t>(impl_->bundle.shape(key.dimension).height / 16));
    for (std::size_t s = 0; s < out.size(); ++s) {
        field->section(gameplay::LightKind::block, key.pos, static_cast<std::int32_t>(s), out[s].block);
        field->section(gameplay::LightKind::sky, key.pos, static_cast<std::int32_t>(s), out[s].sky);
    }
    return out;
}

std::vector<std::pair<ChunkPos, std::int32_t>> Engine::light_changes(DimensionId level) {
    impl_->update_light(level);
    auto& field = impl_->light[static_cast<std::size_t>(level)];
    if (!field) return {};
    return field->take_changed();
}

std::vector<std::pair<ChunkKey, gameplay::ChunkLevel>> Engine::loaded_columns() const {
    std::vector<std::pair<ChunkKey, gameplay::ChunkLevel>> out;
    for (const auto& [c, data] : impl_->world.loaded) out.emplace_back(c, data->level);
    return out;
}

contracts::Result<gameplay::BlockState> WorldView::block(DimensionId level, BlockPos pos) const {
    const auto& impl = *engine_->impl_;
    if (impl.epoch != epoch_) return std::unexpected(contracts::ErrorCode::stale_view);
    const auto& shape = impl.bundle.shape(level);
    if (!gameplay::in_world(shape, pos)) return std::unexpected(contracts::ErrorCode::out_of_bounds);
    const auto* data = impl.world.find_loaded({level, contracts::chunk_of(pos)});
    if (!data) return std::unexpected(contracts::ErrorCode::unloaded_chunk);
    const auto& section = data->sections[static_cast<std::size_t>((pos.y - shape.min_y) >> 4)];
    return section ? section->blocks[contracts::section_local_index(pos)] : gameplay::blocks::air();
}

contracts::Result<std::optional<gameplay::BlockEntity>> WorldView::block_entity(DimensionId level, BlockPos pos) const {
    const auto& impl = *engine_->impl_;
    if (impl.epoch != epoch_) return std::unexpected(contracts::ErrorCode::stale_view);
    const auto* data = impl.world.find_loaded({level, contracts::chunk_of(pos)});
    if (!data) return std::unexpected(contracts::ErrorCode::unloaded_chunk);
    const auto it = data->block_entities.find(pos);
    if (it == data->block_entities.end()) return std::optional<gameplay::BlockEntity>{};
    return std::optional<gameplay::BlockEntity>{it->second};
}

contracts::Result<std::optional<gameplay::PlayerInventory>> WorldView::player(contracts::PlayerKey key) const {
    const auto& impl = *engine_->impl_;
    if (impl.epoch != epoch_) return std::unexpected(contracts::ErrorCode::stale_view);
    const auto it = impl.world.players.find(key);
    if (it == impl.world.players.end()) return std::optional<gameplay::PlayerInventory>{};
    return std::optional<gameplay::PlayerInventory>{it->second};
}

contracts::Result<std::optional<gameplay::Entity>> WorldView::entity(DimensionId level, EntityId id) const {
    const auto& impl = *engine_->impl_;
    if (impl.epoch != epoch_) return std::unexpected(contracts::ErrorCode::stale_view);
    const auto it = impl.world.entity_index.find(id);
    if (it == impl.world.entity_index.end() || it->second.dimension != level) return std::optional<gameplay::Entity>{};
    const auto* data = impl.world.find_loaded(it->second);
    if (!data) return std::unexpected(contracts::ErrorCode::unloaded_chunk);
    const auto e = data->entities.find(id);
    if (e == data->entities.end()) return std::optional<gameplay::Entity>{};
    return std::optional<gameplay::Entity>{e->second};
}

} // namespace circuit::runtime
