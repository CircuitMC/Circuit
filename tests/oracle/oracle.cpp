// SPDX-License-Identifier: AGPL-3.0-only
#include "oracle.hpp"
#include <circuit/gameplay/rules.hpp>
#include <algorithm>
#include <stdexcept>

namespace circuit::oracle {
using namespace circuit::gameplay;
using contracts::BlockPos;
using contracts::ChunkPos;
using contracts::EntityId;
using contracts::Phase;
using contracts::Tick;
using contracts::TickList;
using contracts::TraceKind;

class Oracle::Context final : public RuleContext {
public:
    explicit Context(Oracle& o) : RuleContext(o.bundle_), o_(o) {}

    void begin_root(Phase phase, std::uint64_t root) {
        phase_ = phase;
        root_ = root;
        seq_ = 0;
        updater().reset();
    }

    // Commands are inputs between ticks: they observe the previous tick's time.
    Tick now() const override { return phase_ == Phase::commands ? o_.time_ - 1 : o_.time_; }

    BlockState block(BlockPos pos) override {
        if (!in_world(o_.bundle_, pos) || o_.level(contracts::chunk_of(pos)) == ChunkLevel::unloaded)
            return BlockState::unloaded_sentinel();
        const auto it = o_.blocks_.find(pos);
        return it == o_.blocks_.end() ? blocks::air() : it->second;
    }
    void write_block(BlockPos pos, BlockState state) override {
        if (block(pos).is_unloaded()) throw std::logic_error("oracle: write to unloaded position");
        if (state.is_air()) o_.blocks_.erase(pos);
        else o_.blocks_[pos] = state;
    }
    std::optional<contracts::Generation> chunk_generation(ChunkPos chunk) override {
        if (o_.level(chunk) == ChunkLevel::unloaded) return std::nullopt;
        return o_.chunks_.at(chunk).generation;
    }

    bool has_tick(TickList list, BlockPos pos, BlockType type) override {
        return o_.tick_index_[static_cast<int>(list)].contains({pos, type});
    }
    void add_tick(TickList list, BlockPos pos, BlockType type, Tick due, std::int8_t priority, std::uint32_t) override {
        if (o_.next_suborder_ == UINT64_MAX) throw std::overflow_error("oracle: sub-order exhausted");
        const auto l = static_cast<int>(list);
        o_.ticks_[l].push_back({pos, type, due, priority, o_.next_suborder_++});
        o_.tick_index_[l].insert({pos, type});
    }
    bool has_block_event(const BlockEvent& event) override { return o_.pending_events_.contains(event); }
    void add_block_event(const BlockEvent& event, std::uint32_t) override {
        o_.events_.push_back(event);
        o_.pending_events_.insert(event);
    }

    const BlockEntity* peek_block_entity(BlockPos pos) override { return find_block_entity(pos); }
    BlockEntity* block_entity(BlockPos pos) override { return find_block_entity(pos); }
    void create_block_entity(BlockPos pos, BlockType kind) override {
        o_.block_entities_[pos] = BlockEntity{kind, o_.next_block_entity_order_++,
                                              std::vector<contracts::ItemStack>(container_size(kind)), 0};
    }
    void remove_block_entity(BlockPos pos) override { o_.block_entities_.erase(pos); }

    std::vector<EntityId> entities_in_cell(BlockPos cell) override {
        std::vector<EntityId> out;
        if (o_.level(contracts::chunk_of(cell)) == ChunkLevel::unloaded) return out;
        for (const auto& [id, e] : o_.entities_)
            if (contracts::block_of(e.position) == cell) out.push_back(id);
        return out;
    }
    const Entity* peek_entity(EntityId id) override { return find_entity(id); }
    Entity* entity(EntityId id) override { return find_entity(id); }
    EntityId spawn_entity(const Entity& prototype) override {
        const EntityId id{o_.next_entity_id_++};
        auto e = prototype;
        e.id = id;
        o_.entities_[id] = e;
        return id;
    }
    void relocate_entity(EntityId id, contracts::FixedVec position) override {
        if (o_.level(contracts::chunk_of(contracts::block_of(position))) == ChunkLevel::unloaded)
            throw std::logic_error("oracle: relocation into an unloaded chunk");
        o_.entities_.at(id).position = position;
    }
    void remove_entity(EntityId id) override { o_.entities_.erase(id); }

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
        o_.trace_.push_back({{o_.time_, phase_, root_, seq}, kind, pos, a, b, c});
        return seq;
    }

private:
    BlockEntity* find_block_entity(BlockPos pos) {
        if (block(pos).is_unloaded()) return nullptr;
        const auto it = o_.block_entities_.find(pos);
        return it == o_.block_entities_.end() ? nullptr : &it->second;
    }
    Entity* find_entity(EntityId id) {
        const auto it = o_.entities_.find(id);
        if (it == o_.entities_.end()) return nullptr;
        if (o_.level(contracts::chunk_of(contracts::block_of(it->second.position))) == ChunkLevel::unloaded) return nullptr;
        return &it->second;
    }

    Oracle& o_;
    Phase phase_{};
    std::uint64_t root_{};
    std::uint32_t seq_{};
};

Oracle::Oracle(const contracts::VersionBundle& bundle, std::uint64_t seed)
    : bundle_(bundle), seed_(seed), ctx_(std::make_unique<Context>(*this)) {
    if (!bundle.rules) throw std::invalid_argument("oracle: bundle has no executable rules");
}
Oracle::~Oracle() = default;

ChunkLevel Oracle::level(ChunkPos chunk) const {
    const auto it = chunks_.find(chunk);
    return it == chunks_.end() ? ChunkLevel::unloaded : it->second.level;
}

void Oracle::advance(const std::vector<contracts::Command>& commands) {
    if (failed_) throw std::logic_error("oracle: previous tick failed");
    if (time_ == UINT64_MAX) throw std::overflow_error("oracle: time exhausted");
    failed_ = true; // Cleared only when the whole tick completes.
    ++time_;
    trace_.clear();

    for (std::size_t i = 0; i < commands.size(); ++i) {
        ctx_->begin_root(Phase::commands, i);
        const auto& c = commands[i];
        contracts::Result<void> ticket_result;
        if (const auto* add = std::get_if<contracts::AddTicket>(&c.body)) ticket_result = tickets_.add(*add, bundle_.limits);
        else if (const auto* rem = std::get_if<contracts::RemoveTicket>(&c.body)) ticket_result = tickets_.remove(rem->ticket);
        else {
            apply_command(*ctx_, c);
            continue;
        }
        if (ticket_result) ctx_->record(TraceKind::command_applied, {}, c.admission, 0, 0);
        else ctx_->record(TraceKind::command_rejected, {}, c.admission, static_cast<std::uint64_t>(ticket_result.error()), 0);
    }
    ctx_->begin_root(Phase::commands, commands.size());
    apply_levels();

    run_tick_list(TickList::block);
    run_tick_list(TickList::fluid);
    run_random_ticks();
    run_block_events();
    run_entities();
    run_block_entities();
    failed_ = false;
}

void Oracle::apply_levels() {
    const auto desired = tickets_.levels();
    std::set<ChunkPos> keys;
    for (const auto& [c, info] : chunks_)
        if (info.level != ChunkLevel::unloaded) keys.insert(c);
    for (const auto& [c, l] : desired) keys.insert(c);
    for (const auto c : keys) {
        const auto current = level(c);
        const auto it = desired.find(c);
        const auto want = it == desired.end() ? ChunkLevel::unloaded : it->second;
        if (current == want) continue;
        const auto origin = contracts::chunk_origin(c);
        if (current == ChunkLevel::unloaded) {
            contracts::Generation generation{};
            if (const auto known = chunks_.find(c); known != chunks_.end()) {
                const auto next = known->second.generation.next();
                if (!next) throw std::overflow_error("oracle: chunk generation exhausted");
                generation = *next;
            }
            chunks_[c] = {generation, want};
            ctx_->record(TraceKind::chunk_loaded, origin, generation.value, static_cast<std::uint64_t>(want), 0);
        } else if (want == ChunkLevel::unloaded) {
            ctx_->record(TraceKind::chunk_unloaded, origin, chunks_[c].generation.value, 0, 0);
            chunks_[c].level = ChunkLevel::unloaded;
        } else {
            ctx_->record(TraceKind::chunk_level_changed, origin, static_cast<std::uint64_t>(current),
                         static_cast<std::uint64_t>(want), 0);
            chunks_[c].level = want;
        }
    }
}

void Oracle::run_tick_list(TickList list) {
    const auto l = static_cast<int>(list);
    std::vector<ScheduledTick> due;
    std::vector<ScheduledTick> rest;
    for (const auto& t : ticks_[l]) {
        if (t.due <= time_ && level(contracts::chunk_of(t.pos)) == ChunkLevel::ticking) due.push_back(t);
        else rest.push_back(t);
    }
    std::ranges::sort(due, tick_before);
    if (due.size() > bundle_.limits.max_scheduled_ticks_per_list) {
        rest.insert(rest.end(), due.begin() + bundle_.limits.max_scheduled_ticks_per_list, due.end());
        due.resize(bundle_.limits.max_scheduled_ticks_per_list);
    }
    ticks_[l] = std::move(rest);
    const auto phase = list == TickList::block ? Phase::block_ticks : Phase::fluid_ticks;
    for (std::size_t i = 0; i < due.size(); ++i) {
        // A drained tick still counts as scheduled until it runs.
        tick_index_[l].erase({due[i].pos, due[i].block});
        ctx_->begin_root(phase, i);
        run_scheduled_tick(*ctx_, list, due[i]);
    }
}

void Oracle::run_random_ticks() {
    const auto sections = bundle_.world.sections();
    std::uint64_t index = 0;
    for (const auto& [chunk, info] : chunks_) {
        if (info.level != ChunkLevel::ticking) continue;
        for (std::int32_t s = 0; s < sections; ++s) {
            ctx_->begin_root(Phase::random_ticks, index * static_cast<std::uint64_t>(sections) + static_cast<std::uint64_t>(s));
            auto stream = random_tick_stream(seed_, time_, chunk, s);
            gameplay::run_random_ticks(*ctx_, chunk, s, stream);
        }
        ++index;
    }
}

void Oracle::run_block_events() {
    std::deque<BlockEvent> deferred;
    std::uint64_t processed = 0;
    while (!events_.empty()) {
        const auto e = events_.front();
        events_.pop_front();
        if (level(contracts::chunk_of(e.pos)) != ChunkLevel::ticking) {
            deferred.push_back(e); // Stays pending; its place among deferred events is kept.
            continue;
        }
        pending_events_.erase(e);
        if (processed >= bundle_.limits.max_block_events_per_tick)
            throw std::runtime_error("oracle: block event capacity exceeded");
        ctx_->begin_root(Phase::block_events, processed++);
        run_block_event(*ctx_, e);
    }
    events_ = std::move(deferred);
}

void Oracle::run_entities() {
    std::vector<EntityId> ids;
    for (const auto& [id, e] : entities_)
        if (level(contracts::chunk_of(contracts::block_of(e.position))) == ChunkLevel::ticking) ids.push_back(id);
    for (const auto id : ids) {
        ctx_->begin_root(Phase::entities, id.value);
        run_entity(*ctx_, id);
    }
}

void Oracle::run_block_entities() {
    std::vector<std::pair<std::uint64_t, BlockPos>> order;
    for (const auto& [pos, be] : block_entities_)
        if (level(contracts::chunk_of(pos)) == ChunkLevel::ticking) order.push_back({be.order, pos});
    std::ranges::sort(order);
    for (const auto& [o, pos] : order) {
        ctx_->begin_root(Phase::block_entities, o);
        run_block_entity(*ctx_, pos);
    }
}

WorldDump Oracle::dump() const {
    WorldDump d;
    d.time = time_;
    d.next_suborder = next_suborder_;
    d.next_entity_id = next_entity_id_;
    d.next_block_entity_order = next_block_entity_order_;
    for (const auto& [c, info] : chunks_) d.chunks.push_back({c, info.generation.value, info.level});
    for (const auto& [p, s] : blocks_) d.blocks.push_back({p, s.raw()});
    for (const auto& [p, be] : block_entities_) d.block_entities.push_back({p, be});
    for (const auto& [id, e] : entities_) d.entities.push_back(e);
    for (int l = 0; l < 2; ++l)
        for (const auto& t : ticks_[l]) d.ticks.push_back({static_cast<TickList>(l), t});
    d.block_events.assign(events_.begin(), events_.end());
    d.tickets = tickets_.list();
    for (const auto& [key, inv] : players_) d.players.push_back(inv);
    canonicalize(d);
    return d;
}

} // namespace circuit::oracle
