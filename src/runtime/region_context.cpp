// SPDX-License-Identifier: AGPL-3.0-only
#include "region_context.hpp"
#include <circuit/gameplay/rules.hpp>
#include <stdexcept>

namespace circuit::runtime::detail {
using contracts::block_of;
using contracts::chunk_of;

RegionContext::RegionContext(const contracts::VersionBundle& bundle, WorldStore& world, const RegionMap& regions,
                             RegionId region, Phase phase, bool record_trace, bool unchecked, const gameplay::GameData* data)
    : RuleContext(bundle, data), world_(world), regions_(regions), region_(region), phase_(phase),
      record_trace_(record_trace), unchecked_(unchecked) {}

void RegionContext::begin_root(std::uint64_t root, ChunkPos home) {
    root_ = root;
    seq_ = 0;
    creations_ = 0;
    home_ = home;
    updater().reset();
}

void RegionContext::rollback() {
    for (auto& [c, snapshot] : snapshots) *world_.loaded.at(c) = std::move(snapshot);
    snapshots.clear();
    trace.clear();
    entity_moves.clear();
    interactions.clear();
    fifo.clear();
    fifo_pending.clear();
    children.clear();
    for (auto& entries : ledger) entries.clear();
}

Provisional RegionContext::stage(Counter counter, std::uint32_t seq, EntityId entity) {
    auto& entries = ledger[static_cast<std::size_t>(counter)];
    entries.push_back({root_, seq, entity});
    return {true, static_cast<std::uint32_t>(entries.size() - 1)};
}

ChunkData* RegionContext::loaded_in_world(BlockPos pos) const {
    if (!gameplay::in_world(bundle(), pos)) return nullptr;
    return world_.find_loaded(chunk_of(pos));
}

ChunkData& RegionContext::owned(ChunkPos c, ChunkData& data) {
    if (!unchecked_ && region_ != all_regions && regions_.owner.at(c) != region_) throw ForeignAccess{c};
    if (c != home_ && phase_ != Phase::commands) interactions.insert(std::minmax(home_, c));
    return data;
}

ChunkData& RegionContext::writable(ChunkPos c, ChunkData& data) {
    owned(c, data);
    if (!snapshots.contains(c)) snapshots.emplace(c, data);
    return data;
}

BlockState RegionContext::block(BlockPos pos) {
    auto* data = loaded_in_world(pos);
    if (!data) return BlockState::unloaded_sentinel();
    const auto& chunk = owned(chunk_of(pos), *data);
    const auto& section = chunk.sections[section_of(pos.y)];
    return section ? section->blocks[contracts::section_local_index(pos)] : gameplay::blocks::air();
}

void RegionContext::write_block(BlockPos pos, BlockState state) {
    auto* data = loaded_in_world(pos);
    if (!data) throw std::logic_error("runtime: write to an unloaded position");
    auto& chunk = writable(chunk_of(pos), *data);
    auto& section = chunk.sections[section_of(pos.y)];
    if (!section) {
        if (state == gameplay::blocks::air()) return;
        section = std::make_shared<Section>();
    } else if (section.use_count() > 1) {
        section = std::make_shared<Section>(*section); // Copy on write: the snapshot keeps the old section.
    }
    auto& cell = section->blocks[contracts::section_local_index(pos)];
    section->random_ticking = static_cast<std::uint16_t>(section->random_ticking - (gameplay::reacts_to_random_ticks(cell.type()) ? 1 : 0) +
                                                         (gameplay::reacts_to_random_ticks(state.type()) ? 1 : 0));
    cell = state;
}

std::optional<contracts::Generation> RegionContext::chunk_generation(ChunkPos chunk) {
    // Loading state and generations are fixed while a phase runs: no ownership needed.
    if (const auto* data = world_.find_loaded(chunk)) return data->generation;
    return std::nullopt;
}

bool RegionContext::has_tick(TickList list, BlockPos pos, BlockType type) {
    auto* data = loaded_in_world(pos);
    if (!data) return false;
    if (owned(chunk_of(pos), *data).ticks[static_cast<int>(list)].contains({pos, type})) return true;
    const bool draining = (list == TickList::block && phase_ == Phase::block_ticks)
        || (list == TickList::fluid && phase_ == Phase::fluid_ticks);
    return draining && pending_ticks.contains({pos, type});
}

void RegionContext::add_tick(TickList list, BlockPos pos, BlockType type, Tick due, std::int8_t priority, std::uint32_t seq) {
    auto* data = loaded_in_world(pos);
    if (!data) throw std::logic_error("runtime: tick scheduled at an unloaded position");
    auto& chunk = writable(chunk_of(pos), *data);
    chunk.ticks[static_cast<int>(list)][{pos, type}] = TickEntry{due, priority, 0, stage(Counter::suborder, seq)};
}

bool RegionContext::has_block_event(const BlockEvent& event) {
    auto* data = loaded_in_world(event.pos);
    if (!data) return false;
    const auto& chunk = owned(chunk_of(event.pos), *data);
    if (fifo_pending.contains(event)) return true;
    for (const auto& e : chunk.events)
        if (e.event == event) return true;
    return false;
}

void RegionContext::push_local(const BlockEvent& event) {
    fifo.push_back({event, next_local++});
    fifo_pending.insert(event);
    children.emplace_back();
}

void RegionContext::add_block_event(const BlockEvent& event, std::uint32_t seq) {
    auto* data = loaded_in_world(event.pos);
    if (!data) throw std::logic_error("runtime: block event at an unloaded position");
    const auto c = chunk_of(event.pos);
    if (phase_ == Phase::block_events && data->level == ChunkLevel::ticking) {
        owned(c, *data);
        // Runs later in this phase; its global position is derived at commit.
        children.at(root_).push_back(next_local);
        push_local(event);
        return;
    }
    writable(c, *data).events.push_back({event, 0, stage(Counter::event_seq, seq)});
}

const BlockEntity* RegionContext::peek_block_entity(BlockPos pos) {
    auto* data = loaded_in_world(pos);
    if (!data) return nullptr;
    const auto& map = owned(chunk_of(pos), *data).block_entities;
    const auto it = map.find(pos);
    return it == map.end() ? nullptr : &it->second;
}

BlockEntity* RegionContext::block_entity(BlockPos pos) {
    auto* data = loaded_in_world(pos);
    if (!data) return nullptr;
    auto& map = owned(chunk_of(pos), *data).block_entities;
    if (!map.contains(pos)) return nullptr;
    return &writable(chunk_of(pos), *data).block_entities.at(pos);
}

void RegionContext::create_block_entity(BlockPos pos, BlockType kind) {
    auto* data = loaded_in_world(pos);
    if (!data) throw std::logic_error("runtime: block entity at an unloaded position");
    auto& chunk = writable(chunk_of(pos), *data);
    BlockEntity be{kind, 0, std::vector<contracts::ItemStack>(gameplay::container_size(kind)), 0};
    if (region_ == all_regions) {
        be.order = world_.next_block_entity_order++; // Serial command phase.
    } else {
        // Creation order is level-wide: staged and assigned at commit in serial order.
        chunk.new_block_entities[pos] = stage(Counter::block_entity_order, creations_++);
    }
    chunk.block_entities[pos] = std::move(be);
}

void RegionContext::remove_block_entity(BlockPos pos) {
    if (auto* data = loaded_in_world(pos)) {
        auto& chunk = writable(chunk_of(pos), *data);
        chunk.block_entities.erase(pos);
        chunk.new_block_entities.erase(pos); // Its creation stays in the ledger.
    }
}

std::vector<EntityId> RegionContext::entities_in_cell(BlockPos cell) {
    std::vector<EntityId> out;
    const auto c = chunk_of(cell);
    auto* data = world_.find_loaded(c);
    if (!data) return out;
    for (const auto& [id, e] : owned(c, *data).entities)
        if (block_of(e.position) == cell) out.push_back(id);
    return out;
}

std::optional<ChunkPos> RegionContext::locate(EntityId id) const {
    if (const auto it = entity_moves.find(id); it != entity_moves.end()) return it->second;
    if (const auto it = world_.entity_index.find(id); it != world_.entity_index.end()) return it->second;
    return std::nullopt;
}

const Entity* RegionContext::peek_entity(EntityId id) {
    const auto c = locate(id);
    if (!c) return nullptr;
    auto* data = world_.find_loaded(*c);
    if (!data) return nullptr;
    const auto& map = owned(*c, *data).entities;
    const auto it = map.find(id);
    return it == map.end() ? nullptr : &it->second;
}

Entity* RegionContext::entity(EntityId id) {
    const auto c = locate(id);
    if (!c) return nullptr;
    auto* data = world_.find_loaded(*c);
    if (!data) return nullptr;
    if (!owned(*c, *data).entities.contains(id)) return nullptr;
    return &writable(*c, *data).entities.at(id);
}

EntityId RegionContext::spawn_entity(const Entity& prototype) {
    const auto c = chunk_of(block_of(prototype.position));
    auto* data = world_.find_loaded(c);
    if (!data) throw std::logic_error("runtime: entity spawned in an unloaded chunk");
    auto& chunk = writable(c, *data);
    EntityId id;
    if (region_ == all_regions) {
        if (world_.next_entity_id == UINT64_MAX) throw std::overflow_error("runtime: entity ids exhausted");
        id = EntityId{world_.next_entity_id++}; // Serial command phase.
    } else {
        // Entity ids are level-wide: the real id is assigned at commit in serial order.
        if (provisional_entities_ == (1U << 24) - 1) throw std::overflow_error("runtime: provisional entity ids exhausted");
        id = EntityId{provisional_entity_bit | (region_ << 24) | provisional_entities_++};
        chunk.new_entities[id] = stage(Counter::entity_id, creations_++, id);
    }
    auto e = prototype;
    e.id = id;
    chunk.entities[id] = e;
    entity_moves[id] = c;
    return id;
}

void RegionContext::relocate_entity(EntityId id, contracts::FixedVec position) {
    const auto from = locate(id);
    const auto to = chunk_of(block_of(position));
    auto* source = from ? world_.find_loaded(*from) : nullptr;
    auto* target = world_.find_loaded(to);
    if (!source || !target) throw std::logic_error("runtime: invalid entity relocation");
    if (*from == to) {
        writable(to, *target).entities.at(id).position = position;
        return;
    }
    auto& src = writable(*from, *source);
    auto& dst = writable(to, *target); // May abort before anything moved.
    auto node = src.entities.extract(id);
    node.mapped().position = position;
    dst.entities.insert(std::move(node));
    if (auto staged = src.new_entities.extract(id)) dst.new_entities.insert(std::move(staged));
    entity_moves[id] = to;
}

void RegionContext::remove_entity(EntityId id) {
    const auto c = locate(id);
    if (!c) return;
    if (auto* data = world_.find_loaded(*c)) {
        auto& chunk = writable(*c, *data);
        chunk.entities.erase(id);
        chunk.new_entities.erase(id); // Its creation stays in the ledger.
    }
    entity_moves[id] = std::nullopt;
}

// Player inventories are level-wide state; only the serial command phase
// touches them.
const gameplay::PlayerInventory* RegionContext::peek_player(contracts::PlayerKey player) {
    if (phase_ != Phase::commands) throw std::logic_error("runtime: player inventories outside the command phase");
    const auto it = world_.players.find(player);
    return it == world_.players.end() ? nullptr : &it->second;
}

gameplay::PlayerInventory& RegionContext::player(contracts::PlayerKey player) {
    if (phase_ != Phase::commands) throw std::logic_error("runtime: player inventories outside the command phase");
    auto [it, fresh] = world_.players.try_emplace(player);
    if (fresh) it->second.player = player;
    return it->second;
}

std::uint32_t RegionContext::record(TraceKind kind, BlockPos pos, std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    const auto seq = seq_++;
    if (record_trace_) trace.push_back({{world_.time, phase_, root_, seq}, kind, pos, a, b, c});
    return seq;
}

} // namespace circuit::runtime::detail
