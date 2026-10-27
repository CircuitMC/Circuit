// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"

namespace circuit::gameplay {
using namespace detail;

void run_scheduled_tick(RuleContext& ctx, TickList list, const ScheduledTick& tick) {
    const auto s = ctx.block(tick.pos);
    // A fluid tick of a registry block is lava's or a waterlogged block's.
    const bool matches = s.type() == tick.block && (list != TickList::fluid || tick.block != BlockType::other || is_lava(s) || waterlogged(s));
    ctx.record(TraceKind::tick_executed, tick.pos, tick_code(list, tick.block), matches);
    if (!matches) return;
    if (list == TickList::fluid) {
        if (tick.block == BlockType::water || waterlogged(s)) tick_water(ctx, tick.pos, s);
        else if (is_lava(s)) tick_lava(ctx, tick.pos, s);
        return;
    }
    switch (tick.block) {
    case BlockType::repeater: tick_repeater(ctx, tick.pos, s); break;
    case BlockType::redstone_lamp: tick_lamp(ctx, tick.pos, s); break;
    case BlockType::observer: tick_observer(ctx, tick.pos, s); break;
    case BlockType::comparator: tick_comparator(ctx, tick.pos, s); break;
    case BlockType::redstone_torch: case BlockType::redstone_wall_torch: tick_torch(ctx, tick.pos, s); break;
    case BlockType::button: tick_button(ctx, tick.pos, s); break;
    case BlockType::pressure_plate: case BlockType::weighted_plate: tick_plate(ctx, tick.pos, s); break;
    case BlockType::target: tick_target(ctx, tick.pos, s); break;
    case BlockType::rail: tick_rail(ctx, tick.pos, s); break;
    case BlockType::coral_fan: case BlockType::coral_wall_fan: tick_coral(ctx, tick.pos, s); break;
    case BlockType::dispenser: tick_dispenser(ctx, tick.pos, s); break;
    case BlockType::crafter: tick_crafter(ctx, tick.pos, s); break;
    case BlockType::lectern: tick_lectern(ctx, tick.pos, s); break;
    case BlockType::chest: case BlockType::barrel: tick_viewers(ctx, tick.pos); break;
    case BlockType::composter: tick_composter(ctx, tick.pos, s); break;
    case BlockType::other:
        if (checks_later(s)) support_tick(ctx, tick.pos, s);
        else if (is_big_dripleaf(s)) tick_dripleaf(ctx, tick.pos, s);
        else if (is_farmland(s)) tick_farmland(ctx, tick.pos);
        else if (is_fire(s)) tick_fire(ctx, tick.pos, s);
        else if (is_frosted_ice(s)) tick_frosted_ice(ctx, tick.pos, s);
        else if (is_leaves(s)) tick_leaves(ctx, tick.pos, s);
        else if (is_pointed_dripstone(s)) tick_dripstone(ctx, tick.pos, s);
        else if (is_cauldron(s)) tick_cauldron(ctx, tick.pos, s);
        else if (is_growing_plant(s) && registry::game_name(*registry::block_of(s.registry_state())).ends_with("eyeblossom"))
            (void)turn_eyeblossom(ctx, tick.pos, s);
        else tick_falling_block(ctx, tick.pos, s);
        break;
    default: break;
    }
}

void run_block_event(RuleContext& ctx, const BlockEvent& event) {
    const auto s = ctx.block(event.pos);
    const bool matches = s.type() == event.block;
    ctx.record(TraceKind::block_event_executed, event.pos, static_cast<std::uint64_t>(event.block), event.event, matches);
    if (!matches) return;
    if (is_piston(event.block)) run_piston_event(ctx, event, s);
    else if (event.block == BlockType::note_block) run_note_event(ctx, s);
}

void run_block_entity(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (s.is(BlockType::hopper)) tick_hopper(ctx, pos);
    else if (s.is(BlockType::moving_piston)) tick_moving_piston(ctx, pos);
    else if (s.is(BlockType::daylight_detector)) tick_daylight_detector(ctx, pos, s);
    else if (s.is(BlockType::furnace)) tick_furnace(ctx, pos, s);
    else if (s.is(BlockType::crafter)) tick_crafter_entity(ctx, pos, s);
    else if (s.is(BlockType::brewing_stand)) tick_brewing_stand(ctx, pos, s);
}

} // namespace circuit::gameplay
