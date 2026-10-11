// SPDX-License-Identifier: AGPL-3.0-only
#include "oracle.hpp"
#include <circuit/gameplay/rules.hpp>
#include <algorithm>
#include <stdexcept>

namespace circuit::oracle {
using namespace circuit::gameplay;
using contracts::BlockPos;
using contracts::ChunkPos;
using contracts::DimensionId;
using contracts::EntityId;
using contracts::Phase;
using contracts::Tick;
using contracts::TickList;
using contracts::TraceKind;

class Oracle::Context final : public RuleContext {
public:
    explicit Context(Oracle& o) : RuleContext(o.bundle_, o.data_.get()), o_(o) {}

    void begin_root(Phase phase, std::uint64_t root) {
        phase_ = phase;
        root_ = root;
        seq_ = 0;
        updater().reset();
    }
    void use_level(DimensionId level) noexcept { enter(level); }

    // Commands are inputs between ticks: they observe the previous tick's time.
    Tick now() const override { return phase_ == Phase::commands ? o_.time_ - 1 : o_.time_; }

    BlockState block(BlockPos pos) override {
        if (!in_world(world(), pos) || lv().level(contracts::chunk_of(pos)) == ChunkLevel::unloaded)
            return BlockState::unloaded_sentinel();
        const auto it = lv().blocks.find(pos);
        return it == lv().blocks.end() ? blocks::air() : it->second;
    }
    void write_block(BlockPos pos, BlockState state) override {
        if (block(pos).is_unloaded()) throw std::logic_error("oracle: write to unloaded position");
        if (state.is_air()) lv().blocks.erase(pos);
        else lv().blocks[pos] = state;
    }
    std::optional<contracts::Generation> chunk_generation(ChunkPos chunk) override {
        if (lv().level(chunk) == ChunkLevel::unloaded) return std::nullopt;
        return lv().chunks.at(chunk).generation;
    }

    bool has_tick(TickList list, BlockPos pos, BlockType type) override {
        return lv().tick_index[static_cast<int>(list)].contains({pos, type});
    }
    void add_tick(TickList list, BlockPos pos, BlockType type, Tick due, std::int8_t priority, std::uint32_t) override {
        if (lv().next_suborder == UINT64_MAX) throw std::overflow_error("oracle: sub-order exhausted");
        const auto l = static_cast<int>(list);
        lv().ticks[l].push_back({pos, type, due, priority, lv().next_suborder++});
        lv().tick_index[l].insert({pos, type});
    }
    bool has_block_event(const BlockEvent& event) override { return lv().pending_events.contains(event); }
    void add_block_event(const BlockEvent& event, std::uint32_t) override {
        lv().events.push_back(event);
        lv().pending_events.insert(event);
    }

    const BlockEntity* peek_block_entity(BlockPos pos) override { return find_block_entity(pos); }
    BlockEntity* block_entity(BlockPos pos) override { return find_block_entity(pos); }
    void create_block_entity(BlockPos pos, BlockType kind) override {
        lv().block_entities[pos] = BlockEntity{kind, lv().next_block_entity_order++,
                                               std::vector<contracts::ItemStack>(container_size(kind)), 0};
    }
    void remove_block_entity(BlockPos pos) override { lv().block_entities.erase(pos); }

    std::vector<EntityId> entities_in_cell(BlockPos cell) override {
        std::vector<EntityId> out;
        if (lv().level(contracts::chunk_of(cell)) == ChunkLevel::unloaded) return out;
        for (const auto& [id, e] : lv().entities)
            if (contracts::block_of(e.position) == cell) out.push_back(id);
        return out;
    }
    const Entity* peek_entity(EntityId id) override { return find_entity(id); }
    Entity* entity(EntityId id) override { return find_entity(id); }
    EntityId spawn_entity(const Entity& prototype) override {
        // Entity ids are counted over all levels.
        const EntityId id{o_.next_entity_id_++};
        auto e = prototype;
        e.id = id;
        lv().entities[id] = e;
        return id;
    }
    void relocate_entity(EntityId id, contracts::FixedVec position) override {
        if (lv().level(contracts::chunk_of(contracts::block_of(position))) == ChunkLevel::unloaded)
            throw std::logic_error("oracle: relocation into an unloaded chunk");
        lv().entities.at(id).position = position;
    }
    void remove_entity(EntityId id) override { lv().entities.erase(id); }

    const PlayerInventory* peek_player(contracts::PlayerKey player) override {
        if (phase_ != Phase::commands) throw std::logic_error("oracle: player inventories outside the command phase");
        const auto it = o_.players_.find(player);
        return it == o_.players_.end() ? nullptr : &it->second;
    }
    PlayerInventory& player(contracts::PlayerKey player) override {
        if (phase_ != Phase::commands) throw std::logic_error("oracle: player inventories outside the command phase");
        auto [it, fresh] = o_.players_.try_emplace(player);
        if (fresh) it->second.player = player;
        return it->second;
    }

    std::uint32_t record(TraceKind kind, BlockPos pos, std::uint64_t a, std::uint64_t b, std::uint64_t c) override {
        const auto seq = seq_++;
        // The command phase precedes every level's phases (OrderKey).
        const auto level = phase_ == Phase::commands ? std::uint8_t{0} : static_cast<std::uint8_t>(dimension());
        o_.trace_.push_back({{o_.time_, phase_, root_, seq, level}, kind, pos, a, b, c, dimension()});
        return seq;
    }

private:
    Level& lv() { return o_.at(dimension()); }
    BlockEntity* find_block_entity(BlockPos pos) {
        if (block(pos).is_unloaded()) return nullptr;
        const auto it = lv().block_entities.find(pos);
        return it == lv().block_entities.end() ? nullptr : &it->second;
    }
    Entity* find_entity(EntityId id) {
        const auto it = lv().entities.find(id);
        if (it == lv().entities.end()) return nullptr;
        if (lv().level(contracts::chunk_of(contracts::block_of(it->second.position))) == ChunkLevel::unloaded) return nullptr;
        return &it->second;
    }

    Oracle& o_;
    Phase phase_{};
    std::uint64_t root_{};
    std::uint32_t seq_{};
};

Oracle::Oracle(const contracts::VersionBundle& bundle, std::uint64_t seed, std::shared_ptr<const gameplay::GameData> data)
    : bundle_(bundle), seed_(seed), data_(std::move(data)), ctx_(std::make_unique<Context>(*this)) {
    if (!bundle.rules) throw std::invalid_argument("oracle: bundle has no executable rules");
}
Oracle::~Oracle() = default;

ChunkLevel Oracle::Level::level(ChunkPos chunk) const {
    const auto it = chunks.find(chunk);
    return it == chunks.end() ? ChunkLevel::unloaded : it->second.level;
}

std::uint8_t Oracle::Level::simulation(ChunkPos chunk) const {
    const auto it = chunks.find(chunk);
    return it == chunks.end() || it->second.level == ChunkLevel::unloaded ? contracts::unloaded_level : it->second.levels.simulation;
}

void Oracle::advance(const std::vector<contracts::Command>& commands) {
    if (failed_) throw std::logic_error("oracle: previous tick failed");
    if (time_ == UINT64_MAX) throw std::overflow_error("oracle: time exhausted");
    failed_ = true; // Cleared only when the whole tick completes.
    ++time_;
    trace_.clear();

    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& c = commands[i];
        ctx_->use_level(c.dimension);
        ctx_->begin_root(Phase::commands, i);
        contracts::Result<void> ticket_result;
        if (!contracts::valid_dimension(static_cast<std::uint8_t>(c.dimension)))
            ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
        else if (const auto* add = std::get_if<contracts::AddTicket>(&c.body)) ticket_result = tickets_.add(*add, c.dimension, time_, bundle_.limits);
        else if (const auto* rem = std::get_if<contracts::RemoveTicket>(&c.body)) ticket_result = tickets_.remove(rem->ticket);
        else if (const auto* presence = std::get_if<contracts::PlayerPresence>(&c.body)) {
            for (auto& l : levels_) l.presences.erase(presence->player);
            if (presence->present) at(c.dimension).presences[presence->player] = {presence->player, presence->position, presence->spectator};
        } else if (std::holds_alternative<contracts::LevelsStarted>(c.body)) {
            started_ = time_ - 1;
        } else {
            apply_command(*ctx_, c);
            continue;
        }
        if (ticket_result) ctx_->record(TraceKind::command_applied, {}, c.admission, 0, 0);
        else ctx_->record(TraceKind::command_rejected, {}, c.admission, static_cast<std::uint64_t>(ticket_result.error()), 0);
    }
    ctx_->begin_root(Phase::commands, commands.size());
    tickets_.expire(time_);
    apply_levels();

    // Each level with loaded chunks runs all its phases, in level order.
    for (const auto level : contracts::level_order) {
        const auto& lv = at(level);
        if (std::ranges::none_of(lv.chunks, [](const auto& c) { return c.second.level != ChunkLevel::unloaded; })) continue;
        ctx_->use_level(level);
        // Idle: no players and no forced chunks for too long.
        const bool active = !at(level).presences.empty() || tickets_.keeps_active(level);
        if (active) at(level).last_active = time_;
        const bool awake = level_awake(active, time_, at(level).last_active, started_);
        run_tick_list(level, TickList::block);
        run_tick_list(level, TickList::fluid);
        run_random_ticks(level);
        run_block_events(level);
        if (awake) run_entities(level);
        if (awake) run_block_entities(level);
    }
    failed_ = false;
}

void Oracle::apply_levels() {
    const auto desired = tickets_.levels();
    std::set<contracts::ChunkKey> keys;
    for (const auto level : contracts::level_order)
        for (const auto& [c, info] : at(level).chunks)
            if (info.level != ChunkLevel::unloaded) keys.insert({level, c});
    for (const auto& [c, l] : desired) keys.insert(c);
    for (const auto& key : keys) {
        auto& lv = at(key.dimension);
        const auto c = key.pos;
        const auto current = lv.level(c);
        const auto it = desired.find(key);
        const auto levels = it == desired.end() ? ChunkLevels{} : it->second;
        const auto want = status_of(levels.load);
        if (const auto known = lv.chunks.find(c); known != lv.chunks.end()) known->second.levels = levels;
        if (current == want) continue;
        ctx_->use_level(key.dimension);
        const auto origin = contracts::chunk_origin(c);
        if (current == ChunkLevel::unloaded) {
            contracts::Generation generation{};
            if (const auto known = lv.chunks.find(c); known != lv.chunks.end()) {
                const auto next = known->second.generation.next();
                if (!next) throw std::overflow_error("oracle: chunk generation exhausted");
                generation = *next;
            }
            lv.chunks[c] = {generation, want, levels};
            ctx_->record(TraceKind::chunk_loaded, origin, generation.value, static_cast<std::uint64_t>(want), 0);
        } else if (want == ChunkLevel::unloaded) {
            ctx_->record(TraceKind::chunk_unloaded, origin, lv.chunks[c].generation.value, 0, 0);
            lv.chunks[c].level = ChunkLevel::unloaded;
        } else {
            ctx_->record(TraceKind::chunk_level_changed, origin, static_cast<std::uint64_t>(current),
                         static_cast<std::uint64_t>(want), 0);
            lv.chunks[c].level = want;
        }
    }
}

void Oracle::run_tick_list(DimensionId level, TickList list) {
    auto& lv = at(level);
    const auto l = static_cast<int>(list);
    std::vector<ScheduledTick> due;
    std::vector<ScheduledTick> rest;
    for (const auto& t : lv.ticks[l]) {
        if (t.due <= time_ && block_ticking(lv.simulation(contracts::chunk_of(t.pos)))) due.push_back(t);
        else rest.push_back(t);
    }
    std::ranges::sort(due, tick_before);
    if (due.size() > bundle_.limits.max_scheduled_ticks_per_list) {
        rest.insert(rest.end(), due.begin() + bundle_.limits.max_scheduled_ticks_per_list, due.end());
        due.resize(bundle_.limits.max_scheduled_ticks_per_list);
    }
    lv.ticks[l] = std::move(rest);
    const auto phase = list == TickList::block ? Phase::block_ticks : Phase::fluid_ticks;
    for (std::size_t i = 0; i < due.size(); ++i) {
        // A drained tick still counts as scheduled until it runs.
        lv.tick_index[l].erase({due[i].pos, due[i].block});
        ctx_->begin_root(phase, i);
        run_scheduled_tick(*ctx_, list, due[i]);
    }
}

void Oracle::run_random_ticks(DimensionId level) {
    const auto sections = bundle_.shape(level).sections();
    std::uint64_t index = 0;
    std::vector<ChunkPos> ticking;
    for (const auto& [chunk, info] : at(level).chunks)
        if (info.level != ChunkLevel::unloaded
            && random_ticks_run(bundle_.rules->random_ticks, info.levels.simulation, chunk, at(level).presences))
            ticking.push_back(chunk);
    for (const auto chunk : ticking) {
        for (std::int32_t s = 0; s < sections; ++s) {
            ctx_->begin_root(Phase::random_ticks, index * static_cast<std::uint64_t>(sections) + static_cast<std::uint64_t>(s));
            auto stream = random_tick_stream(seed_, time_, level, chunk, s);
            gameplay::run_random_ticks(*ctx_, chunk, s, stream);
        }
        ++index;
    }
}

void Oracle::run_block_events(DimensionId level) {
    auto& lv = at(level);
    std::deque<BlockEvent> deferred;
    std::uint64_t processed = 0;
    while (!lv.events.empty()) {
        const auto e = lv.events.front();
        lv.events.pop_front();
        if (!block_ticking(lv.simulation(contracts::chunk_of(e.pos)))) {
            deferred.push_back(e); // Stays pending; its place among deferred events is kept.
            continue;
        }
        lv.pending_events.erase(e);
        if (processed >= bundle_.limits.max_block_events_per_tick)
            throw std::runtime_error("oracle: block event capacity exceeded");
        ctx_->begin_root(Phase::block_events, processed++);
        run_block_event(*ctx_, e);
    }
    lv.events = std::move(deferred);
}

void Oracle::run_entities(DimensionId level) {
    auto& lv = at(level);
    std::vector<EntityId> ids;
    for (const auto& [id, e] : lv.entities)
        if (entity_ticking(lv.simulation(contracts::chunk_of(contracts::block_of(e.position))))) ids.push_back(id);
    for (const auto id : ids) {
        ctx_->begin_root(Phase::entities, id.value);
        run_entity(*ctx_, id);
    }
}

void Oracle::run_block_entities(DimensionId level) {
    auto& lv = at(level);
    std::vector<std::pair<std::uint64_t, BlockPos>> order;
    for (const auto& [pos, be] : lv.block_entities)
        if (block_ticking(lv.simulation(contracts::chunk_of(pos)))) order.push_back({be.order, pos});
    std::ranges::sort(order);
    for (const auto& [o, pos] : order) {
        ctx_->begin_root(Phase::block_entities, o);
        run_block_entity(*ctx_, pos);
    }
}

WorldDump Oracle::dump() const {
    WorldDump d;
    d.time = time_;
    d.next_entity_id = next_entity_id_;
    d.started = started_;
    for (const auto level : contracts::level_order) {
        const auto& lv = levels_[static_cast<std::size_t>(level)];
        if (lv.chunks.empty() && lv.presences.empty() && lv.last_active == 0) continue;
        auto& l = d.level(level);
        l.next_suborder = lv.next_suborder;
        l.next_block_entity_order = lv.next_block_entity_order;
        l.last_active = lv.last_active;
        for (const auto& [key, p] : lv.presences) l.presences.push_back(p);
        for (const auto& [c, info] : lv.chunks)
            l.chunks.push_back({c, info.generation.value, info.level,
                                info.level == ChunkLevel::unloaded ? contracts::unloaded_level : info.levels.load,
                                info.level == ChunkLevel::unloaded ? contracts::unloaded_level : info.levels.simulation});
        for (const auto& [p, s] : lv.blocks) l.blocks.push_back({p, s.raw()});
        for (const auto& [p, be] : lv.block_entities) l.block_entities.push_back({p, be});
        for (const auto& [id, e] : lv.entities) l.entities.push_back(e);
        for (int t = 0; t < 2; ++t)
            for (const auto& tick : lv.ticks[t]) l.ticks.push_back({static_cast<TickList>(t), tick});
        l.block_events.assign(lv.events.begin(), lv.events.end());
    }
    d.tickets = tickets_.list();
    for (const auto& [key, inv] : players_) d.players.push_back(inv);
    canonicalize(d);
    return d;
}

} // namespace circuit::oracle
