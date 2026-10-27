// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/trace.hpp>
#include <format>

namespace circuit::contracts {
namespace {
std::string_view kind_name(TraceKind kind) {
    switch (kind) {
    case TraceKind::command_applied: return "command_applied";
    case TraceKind::command_rejected: return "command_rejected";
    case TraceKind::block_changed: return "block_changed";
    case TraceKind::tick_scheduled: return "tick_scheduled";
    case TraceKind::tick_executed: return "tick_executed";
    case TraceKind::block_event_queued: return "block_event_queued";
    case TraceKind::block_event_executed: return "block_event_executed";
    case TraceKind::chain_truncated: return "chain_truncated";
    case TraceKind::inventory_changed: return "inventory_changed";
    case TraceKind::entity_spawned: return "entity_spawned";
    case TraceKind::entity_changed_chunk: return "entity_changed_chunk";
    case TraceKind::entity_removed: return "entity_removed";
    case TraceKind::chunk_loaded: return "chunk_loaded";
    case TraceKind::chunk_unloaded: return "chunk_unloaded";
    case TraceKind::chunk_level_changed: return "chunk_level_changed";
    case TraceKind::piston_blocked: return "piston_blocked";
    case TraceKind::entity_stack_changed: return "entity_stack_changed";
    case TraceKind::signal_changed: return "signal_changed";
    case TraceKind::player_slot_changed: return "player_slot_changed";
    case TraceKind::container_opened: return "container_opened";
    case TraceKind::entity_spawn_position: return "entity_spawn_position";
    case TraceKind::entity_spawn_velocity: return "entity_spawn_velocity";
    case TraceKind::level_random_draw: return "level_random_draw";
    case TraceKind::entity_loaded: return "entity_loaded";
    case TraceKind::explosion: return "explosion";
    case TraceKind::player_experience_changed: return "player_experience_changed";
    case TraceKind::level_event: return "level_event";
    }
    return "unknown";
}
} // namespace

std::string describe(const TraceRecord& r) {
    // Overworld records keep their short form; other levels are named.
    const auto level = r.key.level == 0 ? std::string{} : std::format(" level={}", dimension_name(static_cast<DimensionId>(r.key.level)));
    const auto in = r.dimension == DimensionId::overworld ? std::string{} : std::format(" in {}", dimension_name(r.dimension));
    return std::format("t={}{} {} root={} seq={} {} pos=({},{},{}){} a={} b={} c={}", r.key.tick, level,
        phase_name(r.key.phase), r.key.root, r.key.seq, kind_name(r.kind), r.pos.x, r.pos.y, r.pos.z, in,
        r.a, r.b, r.c);
}

std::optional<std::size_t> first_divergence(std::span<const TraceRecord> a, std::span<const TraceRecord> b) {
    const auto n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i)
        if (!(a[i] == b[i])) return i;
    if (a.size() != b.size()) return n;
    return std::nullopt;
}

} // namespace circuit::contracts
