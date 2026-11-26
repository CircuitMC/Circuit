// SPDX-License-Identifier: AGPL-3.0-only
#include "oracle.hpp"
#include "../support/light_reference.hpp"
#include <circuit/gameplay/rules.hpp>
#include <circuit/gameplay/sky.hpp>
#include <algorithm>
#include <cmath>
#include <format>
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

    // The oracle never points the cursor: every read comes here.
    BlockState load_block(BlockPos pos) override {
        if (!in_world(world(), pos) || lv().level(contracts::chunk_of(pos)) == ChunkLevel::unloaded)
            return BlockState::unloaded_sentinel();
        const auto it = lv().blocks.find(pos);
        return it == lv().blocks.end() ? blocks::air() : it->second;
    }
    void write_block(BlockPos pos, BlockState state) override {
        const auto before = block(pos);
        if (before.is_unloaded()) throw std::logic_error("oracle: write to unloaded position");
        if (!(gameplay::light_info(before) == gameplay::light_info(state))) ++lv().light_version;
        const std::pair key{contracts::chunk_of(pos), (pos.y - world().min_y) >> 4};
        if (gameplay::ticks_section_randomly(before) && --lv().random_ticking[key] == 0) lv().random_ticking.erase(key);
        if (gameplay::ticks_section_randomly(state)) ++lv().random_ticking[key];
        if (state.is_air()) lv().blocks.erase(pos);
        else lv().blocks[pos] = state;
    }
    std::uint8_t light(BlockPos pos) override { return o_.light(dimension(), pos); }
    std::uint8_t sky_light(BlockPos pos) override { return o_.light(dimension(), pos, gameplay::LightKind::sky); }
    std::uint8_t block_light(BlockPos pos) override { return o_.light(dimension(), pos, gameplay::LightKind::block); }
    bool player_near(ChunkPos chunk) override { return gameplay::player_near(lv().presences, chunk); }
    std::optional<gameplay::Presence> nearest_player(contracts::Vec3 at, double range) override {
        return gameplay::nearest_player(lv().presences, at, range);
    }
    std::optional<gameplay::Presence> presence_of(contracts::PlayerKey player) override {
        const auto it = lv().presences.find(player);
        return it == lv().presences.end() ? std::nullopt : std::optional(it->second);
    }
    bool final_ids() const noexcept override { return true; }
    bool section_ticks_randomly(ChunkPos chunk, std::int32_t section) override {
        return lv().random_ticking.contains({chunk, section});
    }
    std::uint8_t difficulty() override { return o_.difficulty_; }
    Sky sky() override { return {o_.day_time_, o_.sky_darken_}; }
    std::int32_t game_rule(std::string_view name) override { return game_rule_value(o_.bundle_, o_.game_rules_, name); }
    std::optional<contracts::Generation> chunk_generation(ChunkPos chunk) override {
        if (lv().level(chunk) == ChunkLevel::unloaded) return std::nullopt;
        return lv().chunks.at(chunk).generation;
    }

    bool has_tick(TickList list, BlockPos pos, BlockType type) override {
        const TickKey key{pos, type};
        return lv().tick_index[static_cast<int>(list)].contains(key) || (list == TickList::block && lv().drained.contains(key));
    }
    void add_tick(TickList list, BlockPos pos, BlockType type, Tick due, std::int8_t priority, std::uint32_t) override {
        if (lv().next_suborder == UINT64_MAX) throw std::overflow_error("oracle: sub-order exhausted");
        const auto l = static_cast<int>(list);
        lv().ticks[l].push_back({pos, type, due, priority, lv().next_suborder++});
        lv().tick_index[l].insert({pos, type});
    }
    bool has_block_event(const BlockEvent& event) override { return lv().pending_events.contains(event); }
    std::size_t torch_turn_offs(BlockPos pos, Tick since, bool add) override {
        return count_turn_offs(lv().turn_offs[contracts::chunk_of(pos)], pos, since, add, now());
    }
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
            if (e.returns == 0 && contracts::block_of(e.position) == cell) out.push_back(id);
        return out;
    }
    std::vector<EntityId> entities_in_cells(BlockPos low, BlockPos high) override {
        std::vector<EntityId> out;
        for (const auto& [id, e] : lv().entities) {
            const auto p = contracts::block_of(e.position);
            if (p.x >= low.x && p.x <= high.x && p.y >= low.y && p.y <= high.y && p.z >= low.z && p.z <= high.z
                && lv().level(contracts::chunk_of(p)) != ChunkLevel::unloaded && e.returns == 0)
                out.push_back(id);
        }
        std::ranges::sort(out);
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
    void relocate_entity(EntityId id, contracts::Vec3 position) override {
        if (lv().level(contracts::chunk_of(contracts::block_of(position))) == ChunkLevel::unloaded)
            throw std::logic_error("oracle: relocation into an unloaded chunk");
        lv().entities.at(id).position = position;
    }
    void remove_entity(EntityId id) override { lv().entities.erase(id); }

    // Serial: every phase may use them (the engine's other phases only in the
    // region holding the level's random, which runs its roots in this order).
    const PlayerInventory* peek_player(contracts::PlayerKey player) override {
        const auto it = o_.players_.find(player);
        return it == o_.players_.end() ? nullptr : &it->second;
    }
    PlayerInventory& player(contracts::PlayerKey player) override {
        auto [it, fresh] = o_.players_.try_emplace(player);
        if (fresh) it->second.player = player;
        return it->second;
    }
    const MapData* peek_map(std::int32_t id) override {
        const auto it = o_.maps_.find(id);
        return it == o_.maps_.end() ? nullptr : &it->second;
    }
    MapData* map(std::int32_t id) override {
        if (phase_ != Phase::commands) throw std::logic_error("oracle: maps outside the command phase");
        const auto it = o_.maps_.find(id);
        return it == o_.maps_.end() ? nullptr : &it->second;
    }
    std::vector<std::int32_t> map_ids() override {
        if (phase_ != Phase::commands) throw std::logic_error("oracle: maps outside the command phase");
        std::vector<std::int32_t> ids;
        for (const auto& [id, m] : o_.maps_) ids.push_back(id);
        return ids;
    }
    std::vector<gameplay::Presence> presences() override {
        std::vector<gameplay::Presence> out;
        for (const auto& [key, p] : lv().presences) out.push_back(p);
        return out;
    }
    std::optional<std::int32_t> column_top(std::int32_t x, std::int32_t z) override {
        const BlockPos base{x, world().min_y, z};
        if (!in_world(world(), base) || lv().level(contracts::chunk_of(base)) == ChunkLevel::unloaded) return std::nullopt;
        std::int32_t top = world().min_y - 1;
        const auto& blocks = lv().blocks;
        for (auto it = blocks.lower_bound({x, INT32_MIN, INT32_MIN}); it != blocks.end() && it->first.x == x; ++it)
            if (it->first.z == z && !it->second.is_air()) top = std::max(top, it->first.y);
        return top;
    }
    std::int32_t add_map(MapData data) override {
        data.id = ++o_.last_map_id_;
        o_.maps_.insert_or_assign(data.id, std::move(data));
        return o_.last_map_id_;
    }
    std::int32_t last_map_id() override {
        return o_.last_map_id_;
    }

    std::uint32_t record(TraceKind kind, BlockPos pos, std::uint64_t a, std::uint64_t b, std::uint64_t c) override {
        const auto seq = seq_++;
        // The command phase precedes every level's phases (OrderKey).
        const auto level = phase_ == Phase::commands ? std::uint8_t{0} : static_cast<std::uint8_t>(dimension());
        o_.trace_.push_back({{o_.time_, phase_, root_, seq, level}, kind, pos, a, b, c, dimension()});
        return seq;
    }

    gameplay::RandomSequence& random_sequence(const std::string& id, const gameplay::SequenceIdHash& hash) override {
        auto it = lv().sequences.find(id);
        if (it == lv().sequences.end())
            it = lv().sequences.emplace(id, gameplay::RandomSequence::create(o_.sequence_seed_, hash, o_.sequence_settings_)).first;
        return it->second;
    }

protected:
    gameplay::LevelRandom& level_random() override { return lv().random; }
    std::uint32_t& random_tick_state() override { return lv().random_tick; }

private:
    Level& lv() { return o_.at(dimension()); }
    BlockEntity* find_block_entity(BlockPos pos) {
        if (block(pos).is_unloaded()) return nullptr;
        const auto it = lv().block_entities.find(pos);
        return it == lv().block_entities.end() ? nullptr : &it->second;
    }
    Entity* find_entity(EntityId id) {
        const auto it = lv().entities.find(id);
        if (it == lv().entities.end() || it->second.returns != 0) return nullptr;
        if (lv().level(contracts::chunk_of(contracts::block_of(it->second.position))) == ChunkLevel::unloaded) return nullptr;
        return &it->second;
    }

    Oracle& o_;
    Phase phase_{};
    std::uint64_t root_{};
    std::uint32_t seq_{};
};

Oracle::Oracle(const contracts::VersionBundle& bundle, std::uint64_t seed, std::shared_ptr<const gameplay::GameData> data,
               std::optional<std::int64_t> level_random_seed, std::optional<std::int64_t> sequence_seed)
    : bundle_(bundle), seed_(seed), sequence_seed_(sequence_seed.value_or(static_cast<std::int64_t>(seed))), data_(std::move(data)),
      ctx_(std::make_unique<Context>(*this)) {
    if (!bundle.rules) throw std::invalid_argument("oracle: bundle has no executable rules");
    for (const auto level : contracts::level_order) {
        at(level).random = gameplay::initial_level_random(seed, level, level_random_seed);
        at(level).random_tick = gameplay::initial_random_tick(seed, level, level_random_seed);
    }
}

Oracle::Oracle(const contracts::VersionBundle& bundle, std::uint64_t seed, const WorldDump& state,
               std::shared_ptr<const gameplay::GameData> data)
    : Oracle(bundle, seed, std::move(data)) {
    restore(state);
}

void Oracle::restore(const WorldDump& state) {
    const auto bad = [](std::string_view what) { throw std::invalid_argument(std::format("oracle: invalid state: {}", what)); };
    time_ = state.time;
    next_entity_id_ = state.next_entity_id;
    started_ = state.started;
    for (const auto& [name, value] : state.game_rules)
        if (!check_game_rule(bundle_, {name, value})) bad("game rule");
    game_rules_ = state.game_rules;
    if (state.day_time < 0) bad("day time");
    day_time_ = state.day_time;
    if (state.difficulty > contracts::max_difficulty) bad("difficulty");
    difficulty_ = state.difficulty;
    if (!valid_weather(state.weather)) bad("weather");
    weather_ = state.weather;
    sequence_settings_ = state.sequence_settings;
    // As the last tick computed it, from the day time before it advanced.
    sky_darken_ = sky_darken(weather_, day_time_ - (game_rule_value(bundle_, game_rules_, "doDaylightCycle") != 0 ? 1 : 0));
    std::optional<DimensionId> previous;
    std::set<EntityId> entity_ids; // over all levels, present or returning
    for (const auto& l : state.levels) {
        if (!contracts::valid_dimension(static_cast<std::uint8_t>(l.dimension))) bad("level");
        if (previous && *previous >= l.dimension) bad("levels out of order");
        previous = l.dimension;
        const auto& shape = bundle_.shape(l.dimension);
        auto& lv = at(l.dimension);
        lv.next_suborder = l.next_suborder;
        lv.next_block_entity_order = l.next_block_entity_order;
        lv.last_active = l.last_active;
        if (l.random > LevelRandom::mask) bad("level random");
        lv.random = LevelRandom::from_state(l.random);
        lv.random_tick = l.random_tick;
        lv.sequences.clear();
        for (const auto& [id, sequence] : l.sequences)
            if (id.empty() || !lv.sequences.emplace(id, sequence).second) bad("random sequence");
        for (const auto& p : l.presences) {
            const bool elsewhere = std::ranges::any_of(levels_, [&](const Level& other) { return other.presences.contains(p.player); });
            if (elsewhere || !lv.presences.emplace(p.player, p).second) bad("a player in two levels");
        }
        for (const auto& c : l.chunks) {
            if ((c.level == ChunkLevel::unloaded && c.resumes != 0) || (c.level != ChunkLevel::unloaded && c.saved_at != 0))
                bad("chunk reload ticks");
            if (c.level != status_of(c.load)) bad("chunk status and load level disagree");
            ChunkInfo info{contracts::Generation{c.generation}, c.level, {c.load, c.simulation}, c.saved_at, c.resumes};
            if (!lv.chunks.emplace(c.pos, info).second) bad("duplicate column");
        }
        const auto column = [&](BlockPos p) {
            if (!shape.contains_y(p.y) || !contracts::within_border(p)) bad("position outside the world");
            const auto chunk = contracts::chunk_of(p);
            if (!lv.chunks.contains(chunk)) bad("position in an unknown column");
            return chunk;
        };
        for (const auto& [pos, raw] : l.blocks) {
            const auto s = BlockState::decode(raw);
            if (!s) bad("block state");
            if (*s == blocks::air()) continue;
            const auto chunk = column(pos);
            lv.blocks[pos] = *s;
            if (ticks_section_randomly(*s)) ++lv.random_ticking[{chunk, (pos.y - shape.min_y) >> 4}];
        }
        for (const auto& [pos, be] : l.block_entities) {
            column(pos);
            if (!lv.block_entities.emplace(pos, be).second) bad("duplicate block entity");
        }
        // Entities may be above or below the world height (falling out of it),
        // within the bounds the rules can reach; everything else is checked at
        // full width so that ticking a restored state cannot overflow. Below the
        // world, falling blocks and primed TNT fall on until their time is up;
        // other entities are gone a tick after passing 64 below it.
        const double lowest = static_cast<double>(shape.min_y) - 131072.0;
        const double highest = static_cast<double>(shape.min_y) + shape.height + 4096.0;
        for (const auto& e : l.entities) {
            if (!contracts::within_border(e.position) || e.id.value == 0 || e.id.value >= next_entity_id_) bad("entity");
            if (static_cast<std::uint8_t>(e.kind) == 0 || static_cast<std::uint8_t>(e.kind) >= entity_kind_count) bad("entity kind");
            if (e.position.y < lowest || e.position.y >= highest) bad("entity height");
            if (!valid_velocity(e.velocity)) bad("entity velocity");
            if (e.piston_tick > state.time || !contracts::finite(e.piston_push) || std::abs(e.piston_push.x) > 0.51
                || std::abs(e.piston_push.y) > 0.51 || std::abs(e.piston_push.z) > 0.51)
                bad("entity piston push");
            if (!std::isfinite(e.fall_distance) || e.fall_distance < 0.0 || e.fall_distance > highest - lowest) bad("entity fall distance");
            // Each kind's own state, valid; the state of other kinds at its defaults.
            auto rest = e;
            switch (e.kind) {
            case EntityKind::item:
                if (!items::valid(e.stack.item) || e.stack.count < 1 || e.stack.count > items::max_stack(e.stack)
                    || e.stack.damage > items::max_damage(e.stack) || e.pickup_delay > item_pickup_never)
                    bad("entity stack");
                if (!valid_item_age(e.age)) bad("entity age");
                if (e.health < 1 || e.health > 32767) bad("item health");
                if (e.fire < 0 || e.fire > 32767) bad("item fire");
                rest.stack = {};
                rest.fire = 0;
                rest.age = 0;
                rest.pickup_delay = 0;
                rest.health = item_health;
                break;
            case EntityKind::falling_block:
                if (!BlockState::decode(e.carried.raw()) || e.carried.is_air() || e.carried.is_unloaded()) bad("falling block state");
                rest.carried = {};
                rest.fall_time = 0;
                break;
            case EntityKind::primed_tnt:
                if (e.fuse < -32768 || e.fuse > 32767) bad("primed TNT fuse");
                rest.fuse = 0;
                break;
            case EntityKind::arrow:
                if (e.life < 0 || e.life > 32767 || e.fire < -32768 || e.fire > 32767) bad("arrow state");
                rest.in_ground = false;
                rest.life = 0;
                rest.fire = 0;
                break;
            case EntityKind::boat:
                if (e.variant >= boat_types.size() || e.status > 5 || !std::isfinite(e.last_dy) || std::abs(e.last_dy) > 64.0)
                    bad("boat state");
                rest.variant = 0;
                rest.status = 0;
                rest.last_dy = 0.0;
                break;
            case EntityKind::experience_orb:
                // 1.21.4 saves an orb's health as 0.
                if (e.experience < -32768 || e.orbs < 1 || e.age < -32768 || e.age > 32767 || e.health < 0 || e.health > 32767)
                    bad("experience orb state");
                rest.experience = 0;
                rest.orbs = 1;
                rest.age = 0;
                rest.health = item_health;
                break;
            case EntityKind::minecart:
                if (!std::isfinite(e.yaw) || std::abs(e.yaw) >= 360.0f) bad("minecart yaw");
                rest.in_water = false;
                rest.yaw = 0.0f;
                rest.flipped = false;
                break;
            default: break;
            }
            if (!rest.stack.empty() || rest.age != 0 || rest.pickup_delay != 0 || rest.health != item_health
                || rest.carried != BlockState{} || rest.fall_time != 0 || rest.fuse != 0 || rest.in_ground || rest.life != 0
                || rest.fire != 0 || rest.variant != 0 || rest.status != 0 || rest.last_dy != 0.0 || rest.in_water || rest.yaw != 0.0f
                || rest.flipped || rest.experience != 0 || rest.orbs != 1)
                bad("entity state of another kind");
            if (!lv.chunks.contains(contracts::chunk_of(contracts::block_of(e.position)))) bad("entity in an unknown column");
            if (!entity_ids.insert(e.id).second) bad("duplicate entity id");
            // Saved with its column and not back yet.
            if (e.returns != 0 && e.returns <= state.time) bad("entity return");
            lv.entities.emplace(e.id, e);
        }
        for (const auto& t : l.ticks) {
            column(t.tick.pos);
            const int list = static_cast<int>(t.list);
            if (!lv.tick_index[list].insert({t.tick.pos, t.tick.block}).second) bad("duplicate scheduled tick");
            lv.ticks[list].push_back(t.tick);
        }
        for (const auto& e : l.block_events) {
            column(e.pos);
            lv.events.push_back(e);
            lv.pending_events.insert(e);
        }
        for (const auto& t : l.turn_offs) {
            if (t.second > state.time) bad("torch turn-off");
            lv.turn_offs[column(t.first)].push_back(t);
        }
        ++lv.light_version;
    }
    for (const auto& t : state.tickets)
        if (!contracts::valid_dimension(static_cast<std::uint8_t>(t.dimension)) || !tickets_.restore(t, bundle_.limits)) bad("ticket");
    const auto valid_stack = [](const contracts::ItemStack& s) {
        if (s.item == 0 && s.count == 0 && s.damage == 0 && s.components.empty()) return true;
        return items::valid(s.item) && s.count >= 1 && s.count <= items::max_stack(s) && s.damage <= items::max_damage(s);
    };
    for (const auto& p : state.players) {
        if (!valid_stack(p.cursor) || !std::ranges::all_of(p.slots, valid_stack) || !std::ranges::all_of(p.table, valid_stack)
            || p.table_recipe < -1 || !std::ranges::all_of(p.cutter, valid_stack) || p.cutter_recipe < -1
            || !std::ranges::all_of(p.smith, valid_stack) || !std::ranges::all_of(p.grind, valid_stack)
            || !std::ranges::all_of(p.loom, valid_stack) || p.loom_pattern < -1 || p.xp_level < 0 || p.xp_total < 0
            || !std::isfinite(p.xp_progress) || p.xp_progress < 0.0f || p.xp_progress > 1.0f || !std::ranges::all_of(p.anvil, valid_stack)
            || p.anvil_name.size() > 4 * contracts::max_item_name || p.anvil_cost < 0 || !std::ranges::all_of(p.enchant, valid_stack)
            || std::ranges::any_of(p.enchant_costs, [](std::int32_t c) { return c < 0; })
            || std::ranges::any_of(p.enchant_clues, [](std::int32_t c) { return c < -1; })
            || std::ranges::any_of(p.enchant_clue_levels, [](std::int32_t c) { return c < -1; }) || p.drag_stage > 1 || p.drag_type > 2
            || p.drag_slots.size() > 256 || std::ranges::any_of(p.drag_slots, [](std::int16_t s) { return s < 0 || s >= 256; }))
            bad("player inventory");
        if (!players_.emplace(p.player, p).second) bad("duplicate player");
    }
    // Maps of ids up to the last given, each once, of a scale up to 4 in a
    // known dimension, with all their pixels.
    if (state.last_map_id < -1) bad("last map id");
    last_map_id_ = state.last_map_id;
    for (const auto& m : state.maps) {
        if (m.id < 0 || m.id > state.last_map_id || m.scale > max_map_scale
            || !contracts::valid_dimension(static_cast<std::uint8_t>(m.dimension)) || m.colors.size() != map_pixels)
            bad("map");
        if (!maps_.emplace(m.id, m).second) bad("duplicate map");
    }
}
// The blocks of a level at the start of a tick, kept while nothing light
// depends on changes, and light computed from scratch around the columns read.
struct Oracle::LightSnapshot {
    std::uint64_t version{};
    std::map<BlockPos, BlockState> blocks;
    std::set<ChunkPos> loaded;
    std::int32_t top{};
    std::map<ChunkPos, test::ReferenceLight> around;
};

Oracle::~Oracle() = default;

std::uint8_t Oracle::light(DimensionId level, BlockPos pos, std::optional<gameplay::LightKind> only) {
    auto& snap = light_[static_cast<std::size_t>(level)];
    const auto chunk = contracts::chunk_of(pos);
    if (!snap || !snap->loaded.contains(chunk)) return 0;
    if (pos.y > snap->top) return only == gameplay::LightKind::block ? 0 : 15;
    auto it = snap->around.find(chunk);
    if (it == snap->around.end()) {
        std::set<ChunkPos> columns;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
                if (snap->loaded.contains({chunk.x + dx, chunk.z + dz})) columns.insert({chunk.x + dx, chunk.z + dz});
        const test::MapLightWorld world(snap->blocks, snap->loaded);
        it = snap->around.try_emplace(chunk, world, columns, bundle_.shape(level).min_y, snap->top).first;
    }
    if (only) return it->second.get(*only, pos);
    return std::max(it->second.get(gameplay::LightKind::block, pos), it->second.get(gameplay::LightKind::sky, pos));
}

ChunkLevel Oracle::Level::level(ChunkPos chunk) const {
    const auto it = chunks.find(chunk);
    return it == chunks.end() ? ChunkLevel::unloaded : it->second.level;
}

std::uint8_t Oracle::Level::simulation(ChunkPos chunk) const {
    const auto it = chunks.find(chunk);
    return it == chunks.end() || it->second.level == ChunkLevel::unloaded ? contracts::unloaded_level : it->second.levels.simulation;
}

bool Oracle::Level::resumed(ChunkPos chunk, Tick now) const {
    const auto it = chunks.find(chunk);
    return it != chunks.end() && now >= it->second.resumes;
}

void Oracle::advance(const std::vector<contracts::Command>& commands) {
    if (failed_) throw std::logic_error("oracle: previous tick failed");
    if (time_ == UINT64_MAX) throw std::overflow_error("oracle: time exhausted");
    failed_ = true; // Cleared only when the whole tick completes.
    ++time_;
    trace_.clear();
    auto weather_rng = weather_stream(seed_, time_);
    // Light during this tick follows the blocks as they are now, at its start.
    for (const auto level : contracts::level_order) {
        auto& lv = at(level);
        auto& snap = light_[static_cast<std::size_t>(level)];
        if (snap && snap->version == lv.light_version) continue;
        if (!snap) snap = std::make_unique<LightSnapshot>();
        snap->version = lv.light_version;
        snap->blocks = lv.blocks;
        snap->loaded.clear();
        for (const auto& [c, info] : lv.chunks)
            if (info.level != ChunkLevel::unloaded) snap->loaded.insert(c);
        std::int32_t highest = bundle_.shape(level).min_y;
        for (const auto& [p, s] : snap->blocks) highest = std::max(highest, p.y);
        snap->top = std::min(bundle_.shape(level).min_y + bundle_.shape(level).height - 1, highest + 16);
        snap->around.clear();
    }

    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& c = commands[i];
        ctx_->use_level(c.dimension);
        ctx_->begin_root(Phase::commands, i);
        contracts::Result<void> ticket_result;
        if (!contracts::valid_dimension(static_cast<std::uint8_t>(c.dimension)))
            ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
        else if (const auto* add = std::get_if<contracts::AddTicket>(&c.body)) ticket_result = tickets_.add(*add, c.dimension, time_, bundle_.limits);
        else if (const auto* rem = std::get_if<contracts::RemoveTicket>(&c.body)) ticket_result = tickets_.remove(rem->ticket);
        else if (const auto* presence = std::get_if<contracts::PlayerPresence>(&c.body); presence && presence->held > 8)
            ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
        else if (const auto* presence = std::get_if<contracts::PlayerPresence>(&c.body)) {
            for (auto& l : levels_) l.presences.erase(presence->player);
            if (presence->present)
                at(c.dimension).presences[presence->player] = {presence->player, presence->position, presence->spectator, presence->held};
        } else if (std::holds_alternative<contracts::LevelsStarted>(c.body)) {
            started_ = time_ - 1;
            restart_weather(weather_);
        } else if (const auto* rule = std::get_if<contracts::SetGameRule>(&c.body)) {
            ticket_result = check_game_rule(bundle_, *rule);
            if (ticket_result) set_game_rule(bundle_, game_rules_, *rule);
        } else if (const auto* day = std::get_if<contracts::SetDayTime>(&c.body)) {
            if (day->time < 0) ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
            else day_time_ = day->time;
        } else if (const auto* set = std::get_if<contracts::SetDifficulty>(&c.body)) {
            if (set->difficulty > contracts::max_difficulty) ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
            else difficulty_ = set->difficulty;
        } else if (const auto* weather = std::get_if<contracts::SetWeather>(&c.body)) {
            if (static_cast<std::uint8_t>(weather->kind) >= contracts::weather_kind_count || weather->duration < 0)
                ticket_result = std::unexpected(contracts::ErrorCode::invalid_argument);
            else set_weather(weather_, *weather, weather_rng);
        } else {
            apply_command(*ctx_, c);
            continue;
        }
        if (ticket_result) ctx_->record(TraceKind::command_applied, {}, c.admission, 0, 0);
        else ctx_->record(TraceKind::command_rejected, {}, c.admission, static_cast<std::uint64_t>(ticket_result.error()), 0);
    }
    advance_weather(weather_, game_rule_value(bundle_, game_rules_, "doWeatherCycle") != 0, weather_rng);
    sky_darken_ = sky_darken(weather_, day_time_);
    if (game_rule_value(bundle_, game_rules_, "doDaylightCycle") != 0) ++day_time_;
    // Each level runs its phases, in level order: the scheduled ticks of its
    // loaded chunks, then its chunk stage (tickets expire and chunk levels
    // change), then the rest where chunks are loaded.
    const auto loaded = [&](DimensionId level) {
        return std::ranges::any_of(at(level).chunks, [](const auto& c) { return c.second.level != ChunkLevel::unloaded; });
    };
    for (const auto level : contracts::level_order) {
        ctx_->use_level(level);
        if (loaded(level)) {
            run_tick_list(level, TickList::block);
            run_tick_list(level, TickList::fluid);
        }
        ctx_->begin_root(Phase::chunks, 0);
        tickets_.expire(time_, level);
        apply_levels(level);
        if (!loaded(level)) continue;
        // Idle: no players and no forced chunks for too long.
        const bool active = !at(level).presences.empty() || tickets_.keeps_active(level);
        if (active) at(level).last_active = time_;
        const bool awake = level_awake(active, time_, at(level).last_active, started_);
        run_random_ticks(level);
        run_block_events(level);
        if (awake) run_entities(level);
        if (awake) run_block_entities(level);
    }
    // The maps players hold explore once every level ran, serially like the
    // commands.
    if (!maps_.empty()) {
        ctx_->begin_root(Phase::commands, 0);
        forget_map_holders(*ctx_);
        for (const auto level : contracts::level_order) {
            if (at(level).presences.empty()) continue;
            ctx_->use_level(level);
            explore_held_maps(*ctx_);
        }
    }
    failed_ = false;
}

void Oracle::apply_levels(DimensionId level) {
    const auto desired = tickets_.levels(level);
    std::set<contracts::ChunkKey> keys;
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
            Tick resumes{};
            if (const auto known = lv.chunks.find(c); known != lv.chunks.end()) {
                const auto next = known->second.generation.next();
                if (!next) throw std::overflow_error("oracle: chunk generation exhausted");
                generation = *next;
                // A reload: its pending ticks wait out their remaining delay.
                for (auto& list : lv.ticks)
                    for (auto& t : list)
                        if (contracts::chunk_of(t.pos) == c) t.due = reload_shift(t.due, known->second.saved_at, time_);
                resumes = time_ + reload_resume;
                // Its entities come back in the next tick.
                for (auto& [id, e] : lv.entities)
                    if (contracts::chunk_of(contracts::block_of(e.position)) == c) e.returns = time_ + gameplay::entity_return;
            }
            lv.chunks[c] = {generation, want, levels, 0, resumes};
            ++lv.light_version;
            ctx_->record(TraceKind::chunk_loaded, origin, generation.value, static_cast<std::uint64_t>(want), 0);
        } else if (want == ChunkLevel::unloaded) {
            ctx_->record(TraceKind::chunk_unloaded, origin, lv.chunks[c].generation.value, 0, 0);
            for (const auto& [id, e] : lv.entities)
                if (e.returns == 0 && contracts::chunk_of(contracts::block_of(e.position)) == c)
                    ctx_->record(TraceKind::entity_removed, contracts::block_of(e.position), id.value,
                                 static_cast<std::uint64_t>(gameplay::RemovalReason::unloaded), 0);
            lv.chunks[c].level = ChunkLevel::unloaded;
            lv.chunks[c].saved_at = time_ + 1;
            lv.chunks[c].resumes = 0;
            ++lv.light_version;
        } else {
            ctx_->record(TraceKind::chunk_level_changed, origin, static_cast<std::uint64_t>(current),
                         static_cast<std::uint64_t>(want), 0);
            lv.chunks[c].level = want;
        }
    }
    // Entities a reload brought back return now, by column and old id, under
    // new ids and with their tick counts starting again.
    auto& lv = at(level);
    std::vector<std::pair<ChunkPos, EntityId>> back;
    for (const auto& [id, e] : lv.entities) {
        const auto chunk = contracts::chunk_of(contracts::block_of(e.position));
        if (e.returns != 0 && e.returns <= time_ && lv.level(chunk) != ChunkLevel::unloaded) back.emplace_back(chunk, id);
    }
    std::ranges::sort(back);
    for (const auto& [chunk, old] : back) {
        auto node = lv.entities.extract(old);
        auto& e = node.mapped();
        e.id = EntityId{next_entity_id_++};
        e.returns = 0;
        e.tick_count = 0;
        const auto cell = contracts::block_of(e.position);
        ctx_->use_level(level);
        ctx_->record(TraceKind::entity_loaded, cell, e.id.value, old.value, gameplay::entity_payload(e));
        ctx_->record(TraceKind::entity_spawn_position, cell, contracts::double_bits(e.position.x), contracts::double_bits(e.position.y),
                     contracts::double_bits(e.position.z));
        ctx_->record(TraceKind::entity_spawn_velocity, cell, contracts::double_bits(e.velocity.x), contracts::double_bits(e.velocity.y),
                     contracts::double_bits(e.velocity.z));
        node.key() = e.id;
        lv.entities.insert(std::move(node));
    }
}

void Oracle::run_tick_list(DimensionId level, TickList list) {
    auto& lv = at(level);
    const auto l = static_cast<int>(list);
    std::vector<ScheduledTick> due;
    std::vector<ScheduledTick> rest;
    for (const auto& t : lv.ticks[l]) {
        if (t.due <= time_ && block_ticking(lv.simulation(contracts::chunk_of(t.pos))) && lv.resumed(contracts::chunk_of(t.pos), time_))
            due.push_back(t);
        else rest.push_back(t);
    }
    std::ranges::sort(due, tick_before);
    if (due.size() > bundle_.limits.max_scheduled_ticks_per_list) {
        rest.insert(rest.end(), due.begin() + bundle_.limits.max_scheduled_ticks_per_list, due.end());
        due.resize(bundle_.limits.max_scheduled_ticks_per_list);
    }
    lv.ticks[l] = std::move(rest);
    const auto phase = list == TickList::block ? Phase::block_ticks : Phase::fluid_ticks;
    // A drained block tick still counts as scheduled until it runs; a drained
    // fluid tick, or one of a block without rules of its own, does not
    // (lava_flow, leaves_decay). A tick scheduled for the same block while
    // the drained one waits is pending on its own and stays so.
    for (const auto& t : due) {
        lv.tick_index[l].erase({t.pos, t.block});
        if (list == TickList::block && t.block != BlockType::other) lv.drained.insert({t.pos, t.block});
    }
    for (std::size_t i = 0; i < due.size(); ++i) {
        if (list == TickList::block) lv.drained.erase({due[i].pos, due[i].block});
        ctx_->begin_root(phase, i);
        run_scheduled_tick(*ctx_, list, due[i]);
    }
}

void Oracle::run_random_ticks(DimensionId level) {
    if (game_rule_value(bundle_, game_rules_, "randomTickSpeed") <= 0) return;
    const auto& shape = bundle_.shape(level);
    auto& lv = at(level);
    std::vector<ChunkPos> ticking;
    for (const auto& [chunk, info] : lv.chunks)
        if (info.level != ChunkLevel::unloaded && random_ticks_run(bundle_.rules->random_ticks, info.levels.simulation, chunk, lv.presences))
            ticking.push_back(chunk);
    std::uint64_t index = 0;
    for (const auto chunk : gameplay::random_tick_order(std::move(ticking))) {
        ctx_->begin_root(Phase::random_ticks, index++);
        gameplay::run_random_ticks(*ctx_, chunk, shape.sections());
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
    for (const auto& [id, e] : lv.entities) {
        const auto chunk = contracts::chunk_of(contracts::block_of(e.position));
        if (e.returns == 0 && entity_ticking(lv.simulation(chunk)) && lv.resumed(chunk, time_)) ids.push_back(id);
    }
    for (const auto id : ids) {
        ctx_->begin_root(Phase::entities, id.value);
        run_entity(*ctx_, id);
    }
}

void Oracle::run_block_entities(DimensionId level) {
    auto& lv = at(level);
    std::vector<std::pair<std::uint64_t, BlockPos>> order;
    for (const auto& [pos, be] : lv.block_entities)
        if (block_ticking(lv.simulation(contracts::chunk_of(pos))) && lv.resumed(contracts::chunk_of(pos), time_))
            order.push_back({be.order, pos});
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
    d.game_rules = game_rules_;
    d.day_time = day_time_;
    d.difficulty = difficulty_;
    d.weather = weather_;
    d.sequence_settings = sequence_settings_;
    for (const auto level : contracts::level_order) {
        const auto& lv = levels_[static_cast<std::size_t>(level)];
        if (lv.chunks.empty() && lv.presences.empty() && lv.last_active == 0) continue;
        auto& l = d.level(level);
        l.next_suborder = lv.next_suborder;
        l.next_block_entity_order = lv.next_block_entity_order;
        l.last_active = lv.last_active;
        l.random = lv.random.state();
        l.random_tick = lv.random_tick;
        for (const auto& [id, sequence] : lv.sequences) l.sequences.emplace_back(id, sequence);
        for (const auto& [key, p] : lv.presences) l.presences.push_back(p);
        for (const auto& [c, info] : lv.chunks)
            l.chunks.push_back({c, info.generation.value, info.level,
                                info.level == ChunkLevel::unloaded ? contracts::unloaded_level : info.levels.load,
                                info.level == ChunkLevel::unloaded ? contracts::unloaded_level : info.levels.simulation,
                                info.level == ChunkLevel::unloaded ? info.saved_at : 0, info.resumes > time_ ? info.resumes : 0});
        for (const auto& [p, s] : lv.blocks) l.blocks.push_back({p, s.raw()});
        for (const auto& [p, be] : lv.block_entities) l.block_entities.push_back({p, be});
        for (const auto& [id, e] : lv.entities) l.entities.push_back(e);
        for (int t = 0; t < 2; ++t)
            for (const auto& tick : lv.ticks[t]) l.ticks.push_back({static_cast<TickList>(t), tick});
        l.block_events.assign(lv.events.begin(), lv.events.end());
        for (const auto& [c, column] : lv.turn_offs) l.turn_offs.insert(l.turn_offs.end(), column.begin(), column.end());
    }
    d.tickets = tickets_.list();
    for (const auto& [key, inv] : players_) d.players.push_back(inv);
    d.last_map_id = last_map_id_;
    for (const auto& [id, map] : maps_) d.maps.push_back(map);
    canonicalize(d);
    return d;
}

} // namespace circuit::oracle
