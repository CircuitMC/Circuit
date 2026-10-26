// SPDX-License-Identifier: AGPL-3.0-only
#include "region_context.hpp"
#include <circuit/gameplay/rules.hpp>
#include <algorithm>
#include <stdexcept>

namespace circuit::runtime::detail {
using contracts::block_of;
using contracts::chunk_of;

RegionContext::RegionContext(const contracts::VersionBundle& bundle, WorldStore& world, const RegionMap& regions,
                             RegionId region, Phase phase, DimensionId level, bool record_trace, bool unchecked,
                             const gameplay::GameData* data)
    : RuleContext(bundle, data), world_(world), regions_(regions), region_(region), phase_(phase),
      record_trace_(record_trace), unchecked_(unchecked) {
    enter(level);
}

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
    for (std::size_t level = 0; level < random_snapshots_.size(); ++level)
        if (random_snapshots_[level]) world_.levels[level].random = *std::exchange(random_snapshots_[level], std::nullopt);
    for (std::size_t level = 0; level < random_tick_snapshots_.size(); ++level)
        if (random_tick_snapshots_[level]) world_.levels[level].random_tick = *std::exchange(random_tick_snapshots_[level], std::nullopt);
    for (std::size_t level = 0; level < sequence_snapshots_.size(); ++level)
        if (sequence_snapshots_[level]) world_.levels[level].sequences = *std::exchange(sequence_snapshots_[level], std::nullopt);
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
    entries.push_back({root_, seq, entity, dimension()});
    return {true, static_cast<std::uint32_t>(entries.size() - 1)};
}

ChunkData* RegionContext::loaded_in_world(BlockPos pos) const {
    if (!gameplay::in_world(world(), pos)) return nullptr;
    return world_.find_loaded(key(chunk_of(pos)));
}

ChunkData& RegionContext::owned(ChunkPos c, ChunkData& data) {
    if (!unchecked_ && region_ != all_regions && regions_.owner.at(key(c)) != region_) throw ForeignAccess{key(c)};
    if (c != home_ && phase_ != Phase::commands) interactions.insert(std::minmax(key(home_), key(c)));
    return data;
}

ChunkData& RegionContext::writable(ChunkPos c, ChunkData& data) {
    owned(c, data);
    if (!snapshots.contains(key(c))) snapshots.emplace(key(c), data);
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
    section->random_ticking = static_cast<std::uint16_t>(section->random_ticking - (gameplay::ticks_section_randomly(cell) ? 1 : 0) +
                                                         (gameplay::ticks_section_randomly(state) ? 1 : 0));
    cell = state;
}

std::int32_t RegionContext::game_rule(std::string_view name) {
    // Game rules change only in the command phase.
    return gameplay::game_rule_value(bundle(), world_.game_rules, name);
}

const gameplay::LightField& RegionContext::light_field() {
    const auto level = static_cast<std::size_t>(dimension());
    if (const auto* field = world_.light[level]) return *field;
    if (!light_seen_[level] && world_.light_now) light_seen_[level] = world_.light_now(dimension());
    if (!light_seen_[level]) throw std::logic_error("runtime: the level has no light");
    return *light_seen_[level];
}

std::uint8_t RegionContext::light(BlockPos pos) {
    const auto& field = light_field();
    return std::max(field.get(gameplay::LightKind::block, pos), field.get(gameplay::LightKind::sky, pos));
}

std::uint8_t RegionContext::sky_light(BlockPos pos) { return light_field().get(gameplay::LightKind::sky, pos); }
std::uint8_t RegionContext::block_light(BlockPos pos) { return light_field().get(gameplay::LightKind::block, pos); }

bool RegionContext::player_near(ChunkPos chunk) { return gameplay::player_near(world_.counters(dimension()).presences, chunk); }
std::optional<gameplay::Presence> RegionContext::nearest_player(contracts::Vec3 at, double range) {
    return gameplay::nearest_player(world_.counters(dimension()).presences, at, range);
}
std::optional<gameplay::Presence> RegionContext::presence_of(contracts::PlayerKey player) {
    const auto& presences = world_.counters(dimension()).presences;
    const auto it = presences.find(player);
    return it == presences.end() ? std::nullopt : std::optional(it->second);
}
bool RegionContext::final_ids() const noexcept { return region_ == all_regions; }

bool RegionContext::section_ticks_randomly(ChunkPos chunk, std::int32_t section) {
    auto* data = loaded_in_world({chunk.x * 16, world().min_y + section * 16, chunk.z * 16});
    if (!data) return false;
    const auto& sections = owned(chunk, *data).sections;
    const auto& s = sections[static_cast<std::size_t>(section)];
    return s && s->random_ticking > 0;
}

// The day time and weather change only in the command phase and between the
// command phase and the levels' phases.
gameplay::RuleContext::Sky RegionContext::sky() { return {world_.day_time, world_.sky_darken}; }

std::optional<contracts::Generation> RegionContext::chunk_generation(ChunkPos chunk) {
    // Loading state and generations are fixed while a phase runs: no ownership needed.
    if (const auto* data = world_.find_loaded(key(chunk))) return data->generation;
    return std::nullopt;
}

bool RegionContext::has_tick(TickList list, BlockPos pos, BlockType type) {
    auto* data = loaded_in_world(pos);
    if (!data) return false;
    if (owned(chunk_of(pos), *data).ticks[static_cast<int>(list)].contains({pos, type})) return true;
    // A block tick taken out to run this tick still counts for the blocks
    // with rules of their own; a fluid tick, or one of a block without (fire,
    // farmland, leaves and the others), does not, so it can be scheduled
    // again before its taken tick runs (lava_flow, leaves_decay).
    return list == TickList::block && type != BlockType::other && phase_ == Phase::block_ticks && pending_ticks.contains({pos, type});
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

std::size_t RegionContext::torch_turn_offs(BlockPos pos, Tick since, bool add) {
    auto* data = loaded_in_world(pos);
    if (!data) throw std::logic_error("runtime: torch at an unloaded position");
    const auto c = chunk_of(pos);
    auto& column = owned(c, *data).turn_offs;
    // Only a change takes a snapshot of the column.
    if (add || std::ranges::any_of(column, [&](const auto& t) { return t.second < since; }))
        return gameplay::count_turn_offs(writable(c, *data).turn_offs, pos, since, add, now());
    return static_cast<std::size_t>(std::ranges::count(column, pos, &gameplay::TurnOffs::value_type::first));
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
    if (phase_ == Phase::block_events && data->block_ticking()) {
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
        be.order = world_.counters(dimension()).next_block_entity_order++; // Serial command phase.
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
    auto* data = world_.find_loaded(key(c));
    if (!data) return out;
    for (const auto& [id, e] : owned(c, *data).entities)
        if (block_of(e.position) == cell) out.push_back(id);
    return out;
}

std::vector<EntityId> RegionContext::entities_in_cells(BlockPos low, BlockPos high) {
    std::vector<EntityId> out;
    const auto first = chunk_of(low), last = chunk_of(high);
    for (auto cx = first.x; cx <= last.x; ++cx)
        for (auto cz = first.z; cz <= last.z; ++cz) {
            const ChunkPos c{cx, cz};
            auto* data = world_.find_loaded(key(c));
            if (!data) continue;
            for (const auto& [id, e] : owned(c, *data).entities) {
                const auto p = block_of(e.position);
                if (p.x >= low.x && p.x <= high.x && p.y >= low.y && p.y <= high.y && p.z >= low.z && p.z <= high.z) out.push_back(id);
            }
        }
    std::ranges::sort(out);
    return out;
}

std::optional<ChunkPos> RegionContext::locate(EntityId id) const {
    std::optional<ChunkKey> at;
    if (const auto it = entity_moves.find(id); it != entity_moves.end()) at = it->second;
    else if (const auto it = world_.entity_index.find(id); it != world_.entity_index.end()) at = it->second;
    // An entity of another level is not visible from this one.
    if (!at || at->dimension != dimension()) return std::nullopt;
    return at->pos;
}

const Entity* RegionContext::peek_entity(EntityId id) {
    const auto c = locate(id);
    if (!c) return nullptr;
    auto* data = world_.find_loaded(key(*c));
    if (!data) return nullptr;
    const auto& map = owned(*c, *data).entities;
    const auto it = map.find(id);
    return it == map.end() ? nullptr : &it->second;
}

Entity* RegionContext::entity(EntityId id) {
    const auto c = locate(id);
    if (!c) return nullptr;
    auto* data = world_.find_loaded(key(*c));
    if (!data) return nullptr;
    if (!owned(*c, *data).entities.contains(id)) return nullptr;
    return &writable(*c, *data).entities.at(id);
}

EntityId RegionContext::spawn_entity(const Entity& prototype) {
    const auto c = chunk_of(block_of(prototype.position));
    auto* data = world_.find_loaded(key(c));
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
    entity_moves[id] = key(c);
    return id;
}

gameplay::LevelRandom& RegionContext::level_random() {
    // The level's pseudo column: drawing needs to own it.
    const auto k = random_key(dimension());
    if (!unchecked_ && region_ != all_regions && regions_.owner.at(k) != region_) throw ForeignAccess{k};
    if (phase_ != Phase::commands) interactions.insert(std::minmax(key(home_), k));
    auto& random = world_.counters(dimension()).random;
    auto& snapshot = random_snapshots_[static_cast<std::size_t>(dimension())];
    if (!snapshot) snapshot = random;
    return random;
}

std::uint32_t& RegionContext::random_tick_state() {
    // Held with the level random: using it needs the level's pseudo column.
    const auto k = random_key(dimension());
    if (!unchecked_ && region_ != all_regions && regions_.owner.at(k) != region_) throw ForeignAccess{k};
    if (phase_ != Phase::commands) interactions.insert(std::minmax(key(home_), k));
    auto& state = world_.counters(dimension()).random_tick;
    auto& snapshot = random_tick_snapshots_[static_cast<std::size_t>(dimension())];
    if (!snapshot) snapshot = state;
    return state;
}

gameplay::RandomSequence& RegionContext::random_sequence(const std::string& id, const gameplay::SequenceIdHash& hash) {
    // Drawing needs the level's random pseudo column, as the level random does.
    const auto k = random_key(dimension());
    if (!unchecked_ && region_ != all_regions && regions_.owner.at(k) != region_) throw ForeignAccess{k};
    if (phase_ != Phase::commands) interactions.insert(std::minmax(key(home_), k));
    auto& sequences = world_.counters(dimension()).sequences;
    auto& snapshot = sequence_snapshots_[static_cast<std::size_t>(dimension())];
    if (!snapshot) snapshot = sequences;
    auto it = sequences.find(id);
    if (it == sequences.end())
        it = sequences.emplace(id, gameplay::RandomSequence::create(world_.sequence_seed, hash, world_.sequence_settings)).first;
    return it->second;
}

void RegionContext::relocate_entity(EntityId id, contracts::Vec3 position) {
    const auto from = locate(id);
    const auto to = chunk_of(block_of(position));
    auto* source = from ? world_.find_loaded(key(*from)) : nullptr;
    auto* target = world_.find_loaded(key(to));
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
    entity_moves[id] = key(to);
}

void RegionContext::remove_entity(EntityId id) {
    const auto c = locate(id);
    if (!c) return;
    if (auto* data = world_.find_loaded(key(*c))) {
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
    // The command phase precedes every level's phases (OrderKey).
    const auto level = phase_ == Phase::commands ? std::uint8_t{0} : static_cast<std::uint8_t>(dimension());
    if (record_trace_) trace.push_back({{world_.time, phase_, root_, seq, level}, kind, pos, a, b, c, dimension()});
    return seq;
}

} // namespace circuit::runtime::detail
