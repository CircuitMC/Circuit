// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <algorithm>
#include <span>
#include <vector>

// Fixture helpers shared by rule and differential tests.
namespace circuit::test {
using namespace circuit::contracts;
using namespace circuit::gameplay;

inline CommandBody place(BlockPos p, BlockState s) { return SetBlock{p, s.raw(), std::nullopt}; }
inline CommandBody use_lever(BlockPos p) { return UseLever{p}; }
inline CommandBody ticket(std::uint32_t id, ChunkPos center, std::uint8_t radius) { return AddTicket{id, center, radius}; }
inline CommandBody insert(BlockPos p, std::uint8_t slot, std::uint16_t item, std::uint8_t count) {
    return InsertItem{p, slot, {item, count}};
}
inline FixedVec at_block(double x, double y, double z) {
    return {static_cast<std::int64_t>(x * fixed_one), static_cast<std::int64_t>(y * fixed_one),
            static_cast<std::int64_t>(z * fixed_one)};
}

// Assigns increasing admission ids, as a coordinator would.
class Admissions {
public:
    std::vector<Command> operator()(std::vector<CommandBody> bodies) {
        std::vector<Command> out;
        for (auto& b : bodies) out.push_back({next_++, std::move(b)});
        return out;
    }

private:
    std::uint64_t next_{1};
};

inline BlockState state_at(const WorldDump& d, BlockPos p) {
    const auto it = std::ranges::lower_bound(d.blocks, p, {}, [](const auto& b) { return b.first; });
    if (it == d.blocks.end() || it->first != p) return blocks::air();
    return BlockState::decode(it->second).value();
}
inline const BlockEntity* block_entity_at(const WorldDump& d, BlockPos p) {
    for (const auto& [pos, be] : d.block_entities)
        if (pos == p) return &be;
    return nullptr;
}
inline const Entity* entity_in(const WorldDump& d, EntityId id) {
    for (const auto& e : d.entities)
        if (e.id == id) return &e;
    return nullptr;
}
inline std::vector<TraceRecord> of_kind(std::span<const TraceRecord> trace, TraceKind kind) {
    std::vector<TraceRecord> out;
    for (const auto& r : trace)
        if (r.kind == kind) out.push_back(r);
    return out;
}
inline unsigned count_items(const WorldDump& d, std::uint16_t item) {
    unsigned n = 0;
    for (const auto& [pos, be] : d.block_entities)
        for (const auto& s : be.slots)
            if (s.item == item) n += s.count;
    for (const auto& e : d.entities)
        if (e.stack.item == item) n += e.stack.count;
    return n;
}

} // namespace circuit::test
