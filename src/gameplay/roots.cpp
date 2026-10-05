// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"

namespace circuit::gameplay {
using namespace detail;

void run_scheduled_tick(RuleContext& ctx, TickList list, const ScheduledTick& tick) {
    const auto s = ctx.block(tick.pos);
    const bool matches = s.type() == tick.block;
    ctx.record(TraceKind::tick_executed, tick.pos, tick_code(list, tick.block), matches);
    if (!matches) return;
    if (list == TickList::fluid) {
        if (tick.block == BlockType::water) tick_water(ctx, tick.pos, s);
        return;
    }
    switch (tick.block) {
    case BlockType::repeater: tick_repeater(ctx, tick.pos, s); break;
    case BlockType::redstone_lamp: tick_lamp(ctx, tick.pos, s); break;
    case BlockType::observer: tick_observer(ctx, tick.pos, s); break;
    case BlockType::comparator: tick_comparator(ctx, tick.pos, s); break;
    default: break;
    }
}

void run_random_ticks(RuleContext& ctx, ChunkPos chunk, std::int32_t section, Rng& stream) {
    const auto& bundle = ctx.bundle();
    const auto base_y = bundle.world.min_y + section * 16;
    for (std::uint32_t k = 0; k < bundle.limits.random_tick_speed; ++k) {
        const auto r = stream.bounded(4096);
        const BlockPos pos{chunk.x * 16 + static_cast<std::int32_t>(r & 15),
                           base_y + static_cast<std::int32_t>((r >> 8) & 15),
                           chunk.z * 16 + static_cast<std::int32_t>((r >> 4) & 15)};
        const auto s = ctx.block(pos);
        if (s.is(BlockType::wheat)) wheat_random_tick(ctx, pos, s, stream);
    }
}

void run_block_event(RuleContext& ctx, const BlockEvent& event) {
    const auto s = ctx.block(event.pos);
    const bool matches = s.type() == event.block;
    ctx.record(TraceKind::block_event_executed, event.pos, static_cast<std::uint64_t>(event.block), event.event, matches);
    if (!matches) return;
    if (is_piston(event.block)) run_piston_event(ctx, event, s);
}

void run_block_entity(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (s.is(BlockType::hopper)) tick_hopper(ctx, pos);
    else if (s.is(BlockType::moving_piston)) tick_moving_piston(ctx, pos);
}

} // namespace circuit::gameplay
