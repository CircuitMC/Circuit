// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/world_types.hpp>
#include <circuit/contracts/trace.hpp>
#include <algorithm>
#include <format>

namespace circuit::gameplay {
using contracts::ErrorCode;

contracts::Result<void> TicketSet::add(const contracts::AddTicket& command, const contracts::RuleLimits& limits) {
    if (tickets_.contains(command.ticket)) return std::unexpected(ErrorCode::duplicate_ticket);
    if (command.radius > limits.max_ticket_radius) return std::unexpected(ErrorCode::out_of_bounds);
    const auto reach = static_cast<std::int32_t>(command.radius) + 1;
    const auto limit = contracts::world_border / 16;
    if (command.center.x - reach < -limit || command.center.x + reach >= limit || command.center.z - reach < -limit
        || command.center.z + reach >= limit)
        return std::unexpected(ErrorCode::out_of_bounds);
    tickets_.emplace(command.ticket, Ticket{command.ticket, command.center, command.radius});
    return {};
}

contracts::Result<void> TicketSet::remove(std::uint32_t id) {
    if (tickets_.erase(id) == 0) return std::unexpected(ErrorCode::unknown_ticket);
    return {};
}

std::map<ChunkPos, ChunkLevel> TicketSet::levels() const {
    std::map<ChunkPos, ChunkLevel> out;
    for (const auto& [id, t] : tickets_) {
        const std::int32_t r = t.radius;
        for (std::int32_t dx = -r - 1; dx <= r + 1; ++dx)
            for (std::int32_t dz = -r - 1; dz <= r + 1; ++dz) {
                const ChunkPos c{t.center.x + dx, t.center.z + dz};
                const auto level = contracts::chebyshev_distance(c, t.center) <= r ? ChunkLevel::ticking : ChunkLevel::border;
                auto& slot = out[c];
                slot = std::max(slot, level);
            }
    }
    return out;
}

std::vector<Ticket> TicketSet::list() const {
    std::vector<Ticket> out;
    for (const auto& [id, t] : tickets_) out.push_back(t);
    return out;
}

void canonicalize(WorldDump& d) {
    std::ranges::sort(d.chunks, {}, &WorldDump::Chunk::pos);
    std::ranges::sort(d.blocks, {}, [](const auto& b) { return b.first; });
    std::ranges::sort(d.block_entities, {}, [](const auto& b) { return b.first; });
    std::ranges::sort(d.entities, {}, &Entity::id);
    std::ranges::sort(d.ticks, [](const WorldDump::PendingTick& a, const WorldDump::PendingTick& b) {
        if (a.list != b.list) return a.list < b.list;
        return tick_before(a.tick, b.tick);
    });
    std::ranges::sort(d.tickets, {}, &Ticket::id);
}

namespace {
std::string pos_text(BlockPos p) { return std::format("({},{},{})", p.x, p.y, p.z); }
std::string chunk_text(ChunkPos c) { return std::format("[{},{}]", c.x, c.z); }
std::string text(const WorldDump::Chunk& c) {
    return std::format("chunk {} gen={} level={}", chunk_text(c.pos), c.generation, static_cast<int>(c.level));
}
std::string text(const std::pair<BlockPos, std::uint32_t>& b) { return std::format("block {} state={:#x}", pos_text(b.first), b.second); }
std::string text(const std::pair<BlockPos, BlockEntity>& b) {
    std::string slots;
    for (const auto& s : b.second.slots) slots += std::format("{}x{} ", s.item, s.count);
    return std::format("block_entity {} kind={} order={} cooldown={} slots=[{}]", pos_text(b.first),
                       static_cast<int>(b.second.kind), b.second.order, b.second.cooldown, slots);
}
std::string text(const Entity& e) {
    return std::format("entity {} pos=({},{},{}) vel=({},{},{}) stack={}x{} age={} last_tick={} ground={}", e.id.value,
                       e.position.x, e.position.y, e.position.z, e.velocity.x, e.velocity.y, e.velocity.z, e.stack.item,
                       e.stack.count, e.age, e.last_tick, e.on_ground);
}
std::string text(const WorldDump::PendingTick& t) {
    return std::format("tick list={} {} block={} due={} priority={} suborder={}", static_cast<int>(t.list),
                       pos_text(t.tick.pos), static_cast<int>(t.tick.block), t.tick.due, t.tick.priority, t.tick.suborder);
}
std::string text(const BlockEvent& e) {
    return std::format("block_event {} block={} event={} param={}", pos_text(e.pos), static_cast<int>(e.block), e.event, e.param);
}
std::string text(const Ticket& t) { return std::format("ticket {} center={} radius={}", t.id, chunk_text(t.center), t.radius); }

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
    if (a.next_suborder != b.next_suborder) return std::format("next_suborder {} != {}", a.next_suborder, b.next_suborder);
    if (a.next_entity_id != b.next_entity_id) return std::format("next_entity_id {} != {}", a.next_entity_id, b.next_entity_id);
    if (a.next_block_entity_order != b.next_block_entity_order)
        return std::format("next_block_entity_order {} != {}", a.next_block_entity_order, b.next_block_entity_order);
    for (auto s : {compare("chunks", a.chunks, b.chunks), compare("blocks", a.blocks, b.blocks),
                   compare("block_entities", a.block_entities, b.block_entities),
                   compare("entities", a.entities, b.entities), compare("ticks", a.ticks, b.ticks),
                   compare("block_events", a.block_events, b.block_events), compare("tickets", a.tickets, b.tickets)})
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
    void add(FixedVec v) { add(static_cast<std::uint64_t>(v.x)); add(static_cast<std::uint64_t>(v.y)); add(static_cast<std::uint64_t>(v.z)); }
    [[nodiscard]] std::uint64_t value() const { return h_; }

private:
    std::uint64_t h_{0x243F6A8885A308D3ULL};
};
} // namespace

std::uint64_t digest(const WorldDump& d) {
    Hasher h;
    h.add(d.time);
    h.add(d.next_suborder);
    h.add(d.next_entity_id);
    h.add(d.next_block_entity_order);
    for (const auto& c : d.chunks) { h.add(contracts::pack_chunk(c.pos)); h.add(c.generation); h.add(static_cast<std::uint64_t>(c.level)); }
    h.add(d.blocks.size());
    for (const auto& [p, s] : d.blocks) { h.add(p); h.add(s); }
    h.add(d.block_entities.size());
    for (const auto& [p, be] : d.block_entities) {
        h.add(p); h.add(static_cast<std::uint64_t>(be.kind)); h.add(be.order); h.add(be.cooldown);
        for (const auto& s : be.slots) h.add(contracts::pack_stack(s));
    }
    h.add(d.entities.size());
    for (const auto& e : d.entities) {
        h.add(e.id.value); h.add(e.position); h.add(e.velocity); h.add(contracts::pack_stack(e.stack));
        h.add(e.age); h.add(e.last_tick); h.add(e.on_ground);
    }
    h.add(d.ticks.size());
    for (const auto& t : d.ticks) {
        h.add(static_cast<std::uint64_t>(t.list)); h.add(t.tick.pos); h.add(static_cast<std::uint64_t>(t.tick.block));
        h.add(t.tick.due); h.add(static_cast<std::uint64_t>(t.tick.priority + 128)); h.add(t.tick.suborder);
    }
    h.add(d.block_events.size());
    for (const auto& e : d.block_events) { h.add(e.pos); h.add(static_cast<std::uint64_t>(e.block)); h.add(e.event); h.add(e.param); }
    for (const auto& t : d.tickets) { h.add(t.id); h.add(contracts::pack_chunk(t.center)); h.add(t.radius); }
    return h.value();
}

} // namespace circuit::gameplay
