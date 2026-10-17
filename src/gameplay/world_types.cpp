// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/world_types.hpp>
#include <circuit/contracts/trace.hpp>
#include <algorithm>
#include <bit>
#include <format>

namespace circuit::gameplay {
using contracts::ErrorCode;

namespace {
// Every chunk a ticket loads must stay within the border, checked at full width.
contracts::Result<void> check_reach(ChunkPos center, std::int64_t reach) {
    const std::int64_t limit = contracts::world_border / 16;
    const std::int64_t x = center.x, z = center.z;
    if (x - reach < -limit || x + reach >= limit || z - reach < -limit || z + reach >= limit)
        return std::unexpected(ErrorCode::out_of_bounds);
    return {};
}
} // namespace

contracts::Result<void> TicketSet::add(const contracts::AddTicket& command, contracts::DimensionId dimension, Tick now,
                                       const contracts::RuleLimits& limits) {
    if (tickets_.contains(command.ticket)) return std::unexpected(ErrorCode::duplicate_ticket);
    if (static_cast<std::uint8_t>(command.type) >= contracts::ticket_type_count || command.level > 44
        || (command.lifetime != 0 && now > UINT64_MAX - command.lifetime))
        return std::unexpected(ErrorCode::invalid_argument);
    Ticket t{command.ticket, command.center, command.radius, command.type, command.level,
             command.lifetime == 0 ? Tick{0} : now + command.lifetime, dimension};
    if (auto ok = restore(t, limits); !ok) return ok;
    return {};
}

contracts::Result<void> TicketSet::restore(const Ticket& t, const contracts::RuleLimits& limits) {
    if (tickets_.contains(t.id)) return std::unexpected(ErrorCode::duplicate_ticket);
    if (static_cast<std::uint8_t>(t.type) >= contracts::ticket_type_count || t.level > 44)
        return std::unexpected(ErrorCode::invalid_argument);
    if (t.radius > limits.max_ticket_radius || t.reach() > limits.max_load_radius) return std::unexpected(ErrorCode::out_of_bounds);
    if (auto ok = check_reach(t.center, t.reach()); !ok) return ok;
    tickets_.emplace(t.id, t);
    return {};
}

contracts::Result<void> TicketSet::remove(std::uint32_t id) {
    if (tickets_.erase(id) == 0) return std::unexpected(ErrorCode::unknown_ticket);
    return {};
}

contracts::Result<void> check_game_rule(const contracts::VersionBundle& bundle, const contracts::SetGameRule& rule) {
    const auto* known = bundle.schema.game_rule(rule.name);
    if (!known || known->experimental) return std::unexpected(ErrorCode::invalid_argument);
    if (known->type == contracts::GameRuleType::boolean && rule.value != 0 && rule.value != 1)
        return std::unexpected(ErrorCode::invalid_argument);
    const auto range = game_rule_range(rule.name);
    if (rule.value < range.min || rule.value > range.max) return std::unexpected(ErrorCode::out_of_bounds);
    return {};
}

GameRuleRange game_rule_range(std::string_view name) noexcept {
    if (name == "spawnChunkRadius") return {0, 32};
    return {};
}

void set_game_rule(const contracts::VersionBundle& bundle, std::map<std::string, std::int32_t>& set, const contracts::SetGameRule& rule) {
    const auto* known = bundle.schema.game_rule(rule.name);
    if (known && known->default_value == rule.value) set.erase(rule.name);
    else set[rule.name] = rule.value;
}

std::int32_t game_rule_value(const contracts::VersionBundle& bundle, const std::map<std::string, std::int32_t>& set,
                             std::string_view name) {
    if (const auto it = set.find(std::string(name)); it != set.end()) return it->second;
    const auto* known = bundle.schema.game_rule(name);
    return known ? known->default_value : 0;
}

std::vector<std::uint32_t> TicketSet::expire(Tick now, contracts::DimensionId dimension) {
    std::vector<std::uint32_t> out;
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        if (it->second.dimension == dimension && it->second.expires != 0 && it->second.expires <= now) {
            out.push_back(it->first);
            it = tickets_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

std::map<contracts::ChunkKey, ChunkLevels> TicketSet::levels(std::optional<contracts::DimensionId> only) const {
    std::map<contracts::ChunkKey, ChunkLevels> out;
    for (const auto& [id, t] : tickets_) {
        if (only && t.dimension != *only) continue;
        const std::int32_t reach = t.reach();
        for (std::int32_t dx = -reach; dx <= reach; ++dx)
            for (std::int32_t dz = -reach; dz <= reach; ++dz) {
                const ChunkPos c{t.center.x + dx, t.center.z + dz};
                const auto beyond = std::max(0, contracts::chebyshev_distance(c, t.center) - std::int32_t{t.radius});
                const auto level = static_cast<std::uint8_t>(std::min<std::int32_t>(t.level + beyond, contracts::unloaded_level));
                auto& slot = out[{t.dimension, c}];
                if (t.loads()) slot.load = std::min(slot.load, level);
                if (t.simulates()) slot.simulation = std::min(slot.simulation, level);
            }
    }
    // Simulation applies to the chunks that are loaded, and runs only what
    // their load level allows as well.
    std::erase_if(out, [](const auto& e) { return e.second.load >= contracts::unloaded_level; });
    for (auto& [key, l] : out) l.simulation = std::max(l.simulation, l.load);
    return out;
}

bool TicketSet::keeps_active(contracts::DimensionId dimension) const {
    return std::ranges::any_of(tickets_, [&](const auto& e) {
        return e.second.dimension == dimension && e.second.type == contracts::TicketType::forced;
    });
}

std::vector<Ticket> TicketSet::list() const {
    std::vector<Ticket> out;
    for (const auto& [id, t] : tickets_) out.push_back(t);
    return out;
}

const LevelDump& WorldDump::level(contracts::DimensionId dimension) const {
    static const auto empty = [](contracts::DimensionId d) {
        LevelDump l;
        l.dimension = d;
        return l;
    };
    static const LevelDump empty_overworld = empty(contracts::DimensionId::overworld),
                           empty_nether = empty(contracts::DimensionId::the_nether), empty_end = empty(contracts::DimensionId::the_end);
    for (const auto& l : levels)
        if (l.dimension == dimension) return l;
    return dimension == contracts::DimensionId::the_nether ? empty_nether
         : dimension == contracts::DimensionId::the_end    ? empty_end
                                                           : empty_overworld;
}

LevelDump& WorldDump::level(contracts::DimensionId dimension) {
    const auto it = std::ranges::lower_bound(levels, dimension, {}, &LevelDump::dimension);
    if (it != levels.end() && it->dimension == dimension) return *it;
    LevelDump fresh;
    fresh.dimension = dimension;
    return *levels.insert(it, std::move(fresh));
}

void canonicalize(WorldDump& d) {
    std::erase_if(d.levels, [](const LevelDump& l) { return l.chunks.empty() && l.presences.empty() && l.last_active == 0; });
    std::ranges::sort(d.levels, {}, &LevelDump::dimension);
    for (auto& l : d.levels) {
        std::ranges::sort(l.chunks, {}, &LevelDump::Chunk::pos);
        std::ranges::sort(l.blocks, {}, [](const auto& b) { return b.first; });
        std::ranges::sort(l.block_entities, {}, [](const auto& b) { return b.first; });
        std::ranges::sort(l.entities, {}, &Entity::id);
        std::ranges::sort(l.ticks, [](const LevelDump::PendingTick& a, const LevelDump::PendingTick& b) {
            if (a.list != b.list) return a.list < b.list;
            return tick_before(a.tick, b.tick);
        });
        std::ranges::sort(l.presences, {}, &Presence::player);
    }
    std::ranges::sort(d.tickets, {}, &Ticket::id);
    std::erase_if(d.players, [](const PlayerInventory& p) { return p.empty(); });
    std::ranges::sort(d.players, {}, &PlayerInventory::player);
}

std::string_view entity_kind_name(EntityKind kind) noexcept {
    switch (kind) {
    case EntityKind::item: return "minecraft:item";
    case EntityKind::falling_block: return "minecraft:falling_block";
    case EntityKind::primed_tnt: return "minecraft:tnt";
    case EntityKind::arrow: return "minecraft:arrow";
    case EntityKind::snowball: return "minecraft:snowball";
    }
    return "?";
}

namespace {
std::string pos_text(BlockPos p) { return std::format("({},{},{})", p.x, p.y, p.z); }
std::string chunk_text(ChunkPos c) { return std::format("[{},{}]", c.x, c.z); }
std::string text(const LevelDump::Chunk& c) {
    return std::format("chunk {} gen={} status={} load={} simulation={} saved_at={} resumes={}", chunk_text(c.pos), c.generation,
                       static_cast<int>(c.level), c.load, c.simulation, c.saved_at, c.resumes);
}
std::string text(const std::pair<BlockPos, std::uint32_t>& b) { return std::format("block {} state={:#x}", pos_text(b.first), b.second); }
std::string text(const std::pair<BlockPos, BlockEntity>& b) {
    std::string slots;
    for (const auto& s : b.second.slots) slots += std::format("{}x{} ", s.item, s.count);
    return std::format("block_entity {} kind={} order={} cooldown={} signal={} moving={:#x} progress={} motion={} slots=[{}]",
                       pos_text(b.first), static_cast<int>(b.second.kind), b.second.order, b.second.cooldown,
                       b.second.signal, b.second.moving, b.second.progress, b.second.motion, slots);
}
std::string text(const Entity& e) {
    return std::format("entity {} {} pos=({},{},{}) vel=({},{},{}) ticks={} stack={}x{}/{} age={} last_tick={} ground={} pickup_delay={} "
                       "returns={} carried={:#x} fall_time={} fuse={} health={} in_ground={} life={} fire={}",
                       e.id.value, entity_kind_name(e.kind), e.position.x, e.position.y, e.position.z, e.velocity.x, e.velocity.y,
                       e.velocity.z, e.tick_count, e.stack.item, e.stack.count, e.stack.damage, e.age, e.last_tick, e.on_ground,
                       e.pickup_delay, e.returns, e.carried.raw(), e.fall_time, e.fuse, e.health, e.in_ground, e.life, e.fire);
}
std::string text(const LevelDump::PendingTick& t) {
    return std::format("tick list={} {} block={} due={} priority={} suborder={}", static_cast<int>(t.list),
                       pos_text(t.tick.pos), static_cast<int>(t.tick.block), t.tick.due, t.tick.priority, t.tick.suborder);
}
std::string text(const BlockEvent& e) {
    return std::format("block_event {} block={} event={} param={}", pos_text(e.pos), static_cast<int>(e.block), e.event, e.param);
}
std::string text(const Ticket& t) {
    return std::format("ticket {} {} type={} center={} radius={} level={} expires={}", t.id, contracts::dimension_name(t.dimension),
                       static_cast<int>(t.type), chunk_text(t.center), t.radius, t.level, t.expires);
}
std::string text(const Presence& p) {
    return std::format("presence {:016x}{:016x} at ({},{},{}) spectator={}", p.player.hi, p.player.lo, p.position.x, p.position.y,
                       p.position.z, p.spectator);
}
std::string text(const PlayerInventory& p) {
    std::string slots;
    for (std::size_t i = 0; i < p.slots.size(); ++i)
        if (!p.slots[i].empty()) slots += std::format("{}:{}x{}/{} ", i, p.slots[i].item, p.slots[i].count, p.slots[i].damage);
    return std::format("player {:016x}{:016x} cursor={}x{} slots=[{}]", p.player.hi, p.player.lo, p.cursor.item, p.cursor.count, slots);
}

template <class T>
std::string compare(std::string_view name, const std::vector<T>& a, const std::vector<T>& b) {
    const auto n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (!(a[i] == b[i])) return std::format("{}[{}]: {} != {}", name, i, text(a[i]), text(b[i]));
    if (a.size() != b.size())
        return std::format("{}: size {} != {}; next: {}", name, a.size(), b.size(),
                           a.size() > n ? text(a[n]) : text(b[n]));
    return {};
}
} // namespace

std::string first_difference(const WorldDump& a, const WorldDump& b) {
    if (a.time != b.time) return std::format("time {} != {}", a.time, b.time);
    if (a.next_entity_id != b.next_entity_id) return std::format("next_entity_id {} != {}", a.next_entity_id, b.next_entity_id);
    if (a.started != b.started) return std::format("started {} != {}", a.started, b.started);
    if (a.game_rules != b.game_rules) {
        for (const auto& [name, value] : a.game_rules)
            if (!b.game_rules.contains(name) || b.game_rules.at(name) != value)
                return std::format("game rule {}: {} != {}", name, value, b.game_rules.contains(name) ? std::to_string(b.game_rules.at(name)) : "unset");
        return "game rules: set only in the second";
    }
    if (a.day_time != b.day_time) return std::format("day time {} != {}", a.day_time, b.day_time);
    if (a.weather != b.weather) {
        const auto text = [](const Weather& w) {
            return std::format("clear {} rain {}{} thunder {}{} levels {} {}", w.clear_time, w.raining ? "on " : "off ", w.rain_time,
                               w.thundering ? "on " : "off ", w.thunder_time, w.rain_level, w.thunder_level);
        };
        return std::format("weather {} != {}", text(a.weather), text(b.weather));
    }
    if (a.levels.size() != b.levels.size()) return std::format("levels: {} != {}", a.levels.size(), b.levels.size());
    for (std::size_t i = 0; i < a.levels.size(); ++i) {
        const auto& la = a.levels[i];
        const auto& lb = b.levels[i];
        const auto name = contracts::dimension_name(la.dimension);
        if (la.dimension != lb.dimension)
            return std::format("level {}: {} != {}", i, name, contracts::dimension_name(lb.dimension));
        if (la.next_suborder != lb.next_suborder)
            return std::format("{} next_suborder {} != {}", name, la.next_suborder, lb.next_suborder);
        if (la.next_block_entity_order != lb.next_block_entity_order)
            return std::format("{} next_block_entity_order {} != {}", name, la.next_block_entity_order, lb.next_block_entity_order);
        if (la.last_active != lb.last_active) return std::format("{} last_active {} != {}", name, la.last_active, lb.last_active);
        if (la.random != lb.random) return std::format("{} level random {:#x} != {:#x}", name, la.random, lb.random);
        // The overworld's differences keep their short form.
        const auto prefix = la.dimension == contracts::DimensionId::overworld ? std::string{} : std::string(name) + " ";
        for (auto s : {compare("chunks", la.chunks, lb.chunks), compare("blocks", la.blocks, lb.blocks),
                       compare("block_entities", la.block_entities, lb.block_entities),
                       compare("entities", la.entities, lb.entities), compare("ticks", la.ticks, lb.ticks),
                       compare("block_events", la.block_events, lb.block_events), compare("presences", la.presences, lb.presences)})
            if (!s.empty()) return prefix + s;
    }
    for (auto s : {compare("tickets", a.tickets, b.tickets), compare("players", a.players, b.players)})
        if (!s.empty()) return s;
    return {};
}

namespace {
class Hasher {
public:
    void add(std::uint64_t v) {
        h_ ^= v + 0x9E3779B97F4A7C15ULL + (h_ << 6) + (h_ >> 2);
        h_ = (h_ ^ (h_ >> 30)) * 0xBF58476D1CE4E5B9ULL;
        h_ = (h_ ^ (h_ >> 27)) * 0x94D049BB133111EBULL;
        h_ ^= h_ >> 31;
    }
    void add(BlockPos p) { add(static_cast<std::uint32_t>(p.x)); add(static_cast<std::uint32_t>(p.y)); add(static_cast<std::uint32_t>(p.z)); }
    void add(Vec3 v) { add(std::bit_cast<std::uint64_t>(v.x)); add(std::bit_cast<std::uint64_t>(v.y)); add(std::bit_cast<std::uint64_t>(v.z)); }
    [[nodiscard]] std::uint64_t value() const { return h_; }

private:
    std::uint64_t h_{0x243F6A8885A308D3ULL};
};
} // namespace

std::uint64_t digest(const WorldDump& d) {
    Hasher h;
    h.add(d.time);
    h.add(d.next_entity_id);
    h.add(d.started);
    h.add(d.game_rules.size());
    for (const auto& [name, value] : d.game_rules) {
        for (const char ch : name) h.add(static_cast<std::uint64_t>(static_cast<unsigned char>(ch)));
        h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
    }
    h.add(static_cast<std::uint64_t>(d.day_time));
    const auto& w = d.weather;
    h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(w.clear_time)));
    h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(w.rain_time)));
    h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(w.thunder_time)));
    h.add(static_cast<std::uint64_t>(w.raining) | static_cast<std::uint64_t>(w.thundering) << 1);
    h.add(std::bit_cast<std::uint32_t>(w.rain_level));
    h.add(std::bit_cast<std::uint32_t>(w.thunder_level));
    h.add(d.levels.size());
    for (const auto& l : d.levels) {
        h.add(static_cast<std::uint64_t>(l.dimension));
        h.add(l.next_suborder);
        h.add(l.next_block_entity_order);
        h.add(l.last_active);
        h.add(l.random);
        for (const auto& c : l.chunks) {
            h.add(contracts::pack_chunk(c.pos)); h.add(c.generation); h.add(static_cast<std::uint64_t>(c.level)); h.add(c.load); h.add(c.simulation); h.add(c.saved_at); h.add(c.resumes);
        }
        h.add(l.presences.size());
        for (const auto& p : l.presences) { h.add(p.player.hi); h.add(p.player.lo); h.add(p.position); h.add(p.spectator); }
        h.add(l.blocks.size());
        for (const auto& [p, s] : l.blocks) { h.add(p); h.add(s); }
        h.add(l.block_entities.size());
        for (const auto& [p, be] : l.block_entities) {
            h.add(p); h.add(static_cast<std::uint64_t>(be.kind)); h.add(be.order); h.add(be.cooldown); h.add(be.signal);
            h.add(be.moving); h.add(be.progress); h.add(be.motion); h.add(be.ticked);
            h.add(be.components.hash()); h.add(be.saved.hash());
            for (const auto& s : be.slots) { h.add(contracts::pack_stack(s)); h.add(s.components.hash()); }
        }
        h.add(l.entities.size());
        for (const auto& e : l.entities) {
            h.add(e.id.value); h.add(static_cast<std::uint64_t>(e.kind)); h.add(e.position); h.add(e.velocity); h.add(contracts::pack_stack(e.stack));
            h.add(e.stack.components.hash());
            h.add(e.tick_count); h.add(static_cast<std::uint32_t>(e.age)); h.add(e.last_tick); h.add(e.on_ground); h.add(e.pickup_delay);
            h.add(e.returns); h.add(e.carried.raw()); h.add(e.fall_time); h.add(static_cast<std::uint32_t>(e.fuse)); h.add(static_cast<std::uint32_t>(e.health));
            h.add(e.in_ground); h.add(static_cast<std::uint32_t>(e.life)); h.add(static_cast<std::uint32_t>(e.fire));
        }
        h.add(l.ticks.size());
        for (const auto& t : l.ticks) {
            h.add(static_cast<std::uint64_t>(t.list)); h.add(t.tick.pos); h.add(static_cast<std::uint64_t>(t.tick.block));
            h.add(t.tick.due); h.add(static_cast<std::uint64_t>(t.tick.priority + 128)); h.add(t.tick.suborder);
        }
        h.add(l.block_events.size());
        for (const auto& e : l.block_events) { h.add(e.pos); h.add(static_cast<std::uint64_t>(e.block)); h.add(e.event); h.add(e.param); }
    }
    for (const auto& t : d.tickets) {
        h.add(t.id); h.add(static_cast<std::uint64_t>(t.dimension)); h.add(contracts::pack_chunk(t.center)); h.add(t.radius);
        h.add(static_cast<std::uint64_t>(t.type)); h.add(t.level); h.add(t.expires);
    }
    h.add(d.players.size());
    for (const auto& p : d.players) {
        h.add(p.player.hi); h.add(p.player.lo); h.add(contracts::pack_stack(p.cursor)); h.add(p.cursor.components.hash());
        for (const auto& s : p.slots) { h.add(contracts::pack_stack(s)); h.add(s.components.hash()); }
        h.add(p.drag_stage); h.add(p.drag_type); h.add(p.drag_slots.size());
        for (const auto slot : p.drag_slots) h.add(static_cast<std::uint16_t>(slot));
    }
    return h.value();
}

} // namespace circuit::gameplay
