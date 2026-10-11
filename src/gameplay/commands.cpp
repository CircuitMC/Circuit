// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/survival.hpp>
#include <functional>

namespace circuit::gameplay {
namespace {
using contracts::ErrorCode;
using contracts::Result;

template <class... F>
struct overloaded : F... { using F::operator()...; };

Result<void> check_loaded(RuleContext& ctx, BlockPos pos) {
    if (!in_world(ctx.world(), pos)) return std::unexpected(ErrorCode::out_of_bounds);
    if (!ctx.chunk_generation(contracts::chunk_of(pos))) return std::unexpected(ErrorCode::unloaded_chunk);
    return {};
}

bool valid_stack(ItemStack s) {
    return items::valid(s.item) && s.count >= 1 && s.count <= items::max_stack(s);
}

// Each command validates completely before its effect starts, so a rejected
// command never leaves a partial effect.
using Effect = std::function<void()>;
using Plan = Result<Effect>;

Plan plan(RuleContext& ctx, const contracts::SetBlock& c) {
    if (auto ok = check_loaded(ctx, c.pos); !ok) return std::unexpected(ok.error());
    if (c.expected_generation && ctx.chunk_generation(contracts::chunk_of(c.pos)) != c.expected_generation)
        return std::unexpected(ErrorCode::stale_generation);
    const auto state = BlockState::decode(c.state);
    if (!state) return std::unexpected(state.error());
    // Piston heads, moving pistons and extended pistons only arise from piston actions.
    if (state->is(BlockType::piston_head) || state->is(BlockType::moving_piston) || (is_piston(state->type()) && state->flag()))
        return std::unexpected(ErrorCode::invalid_block_state);
    return [&ctx, pos = c.pos, s = *state] { set_block(ctx, pos, s); };
}

Plan plan(RuleContext& ctx, const contracts::UseBlock& c) {
    if (auto ok = check_loaded(ctx, c.pos); !ok) return std::unexpected(ok.error());
    const auto s = ctx.block(c.pos);
    switch (s.type()) {
    case BlockType::lever:
        // A lever also updates around the block it hangs on.
        return [&ctx, pos = c.pos, s] {
            set_block(ctx, pos, s.with_flag(!s.flag()));
            detail::notify_neighbors(ctx, pos);
            detail::notify_neighbors(ctx, pos.relative(detail::lever_attachment(s)));
        };
    case BlockType::redstone_wire:
        if (!detail::is_dot(s) && !detail::is_cross(s)) return std::unexpected(ErrorCode::not_usable);
        return [&ctx, pos = c.pos, s] { detail::toggle_wire(ctx, pos, s); };
    case BlockType::repeater:
        return [&ctx, pos = c.pos, s] { set_block(ctx, pos, s.with_delay(static_cast<std::uint8_t>(s.delay() % 4 + 1))); };
    case BlockType::comparator:
        return [&ctx, pos = c.pos, s] {
            const auto toggled = s.with_level(s.level() == 0 ? 1 : 0);
            detail::set_block_quiet(ctx, pos, toggled);
            detail::refresh_comparator(ctx, pos, toggled);
        };
    default: return std::unexpected(ErrorCode::not_usable);
    }
}

Plan plan(RuleContext& ctx, const contracts::InsertItem& c) {
    if (auto ok = check_loaded(ctx, c.pos); !ok) return std::unexpected(ok.error());
    const auto* be = ctx.peek_block_entity(c.pos);
    if (!be || !is_container(ctx.block(c.pos).type())) return std::unexpected(ErrorCode::not_a_container);
    if (c.slot >= be->slots.size()) return std::unexpected(ErrorCode::invalid_slot);
    if (!valid_stack(c.stack)) return std::unexpected(ErrorCode::invalid_argument);
    const auto existing = be->slots[c.slot];
    if (!existing.empty() && !existing.same_kind(c.stack)) return std::unexpected(ErrorCode::item_mismatch);
    const unsigned total = (existing.empty() ? 0u : existing.count) + c.stack.count;
    if (total > std::min(contracts::max_stack_size, items::max_stack(c.stack))) return std::unexpected(ErrorCode::stack_overflow);
    return [&ctx, c, existing, total] {
        auto* target = ctx.block_entity(c.pos);
        auto after = c.stack;
        after.count = static_cast<std::uint8_t>(total);
        target->slots[c.slot] = after;
        ctx.record(TraceKind::inventory_changed, c.pos, c.slot, contracts::pack_stack(existing), contracts::pack_stack(after));
        for (const auto part : detail::container_parts(ctx, c.pos)) detail::container_changed(ctx, part);
    };
}

Plan plan(RuleContext& ctx, const contracts::SpawnItem& c) {
    // The full-width position is checked before it is narrowed to a block.
    if (!contracts::fixed_within_border(c.position) || !ctx.world().contains_fixed_y(c.position.y))
        return std::unexpected(ErrorCode::out_of_bounds);
    const auto cell = contracts::block_of(c.position);
    if (auto ok = check_loaded(ctx, cell); !ok) return std::unexpected(ok.error());
    if (!valid_stack(c.stack)) return std::unexpected(ErrorCode::invalid_argument);
    for (auto v : {c.velocity.x, c.velocity.y, c.velocity.z})
        if (v <= -contracts::fixed_one || v >= contracts::fixed_one) return std::unexpected(ErrorCode::invalid_argument);
    return [&ctx, c] {
        const Entity e{{}, c.position, c.velocity, c.stack, 0, 0, false};
        detail::record_spawn(ctx, ctx.spawn_entity(e), e);
    };
}

Plan plan(RuleContext& ctx, const contracts::RemoveEntity& c) {
    const auto* e = ctx.peek_entity(c.id);
    if (!e) return std::unexpected(ErrorCode::unknown_entity);
    return [&ctx, id = c.id, cell = contracts::block_of(e->position)] {
        ctx.record(TraceKind::entity_removed, cell, id.value, static_cast<std::uint64_t>(RemovalReason::command));
        ctx.remove_entity(id);
    };
}

// ---- player actions -------------------------------------------------------
//
// Resolved against the state earlier commands left, so that a player's
// actions mean the same however they are grouped into ticks.

// Blocks a placement replaces instead of placing next to.
bool replaceable(BlockState s) { return s.is_air() || s.is(BlockType::water); }
bool usable(BlockState s) {
    return s.is(BlockType::lever) || s.is(BlockType::repeater) || s.is(BlockType::comparator) || detail::is_dot(s)
        || detail::is_cross(s);
}

Plan plan(RuleContext& ctx, const contracts::UseBlock& c);

Plan plan(RuleContext& ctx, const contracts::PlayerUse& c) {
    if (c.slot < contracts::hotbar_first || c.slot >= contracts::player_slots) return std::unexpected(ErrorCode::invalid_slot);
    if (auto ok = check_loaded(ctx, c.pos); !ok) return std::unexpected(ok.error());
    const auto* inventory = ctx.peek_player(c.player);
    const auto held = inventory ? inventory->slots[c.slot] : ItemStack{};
    const bool off_hand = c.slot == contracts::offhand_slot;
    const bool holding = !held.empty() || (inventory && !inventory->slots[contracts::offhand_slot].empty());
    const auto clicked = ctx.block(c.pos);
    // Blocks are used with the main hand only, and not while sneaking with an item.
    if (!off_hand && !(c.sneaking && holding)) {
        if (usable(clicked)) return plan(ctx, contracts::UseBlock{c.pos});
        if (is_container(clicked.type())) {
            // A chest under a solid block does not open.
            for (const auto part : detail::container_parts(ctx, c.pos))
                if (ctx.block(part).is(BlockType::chest) && detail::is_conductor(ctx.block(part.relative(Direction::up))))
                    return std::unexpected(ErrorCode::not_usable);
            return [&ctx, c] { ctx.record(TraceKind::container_opened, c.pos, c.player.hi, c.player.lo); };
        }
    }
    auto state = held.empty() ? std::nullopt : detail::placed_by(held.item, c.face, c.horizontal, c.look);
    if (!state) return std::unexpected(ErrorCode::not_usable);
    const bool replace = replaceable(clicked);
    const auto target = replace ? c.pos : c.pos.relative(c.face);
    const std::uint8_t bit = replace ? 1 : 2;
    if ((c.reach & bit) == 0) return std::unexpected(ErrorCode::out_of_reach);
    if (detail::has_collision(*state) && (c.free & bit) == 0) return std::unexpected(ErrorCode::occupied);
    if (auto ok = check_loaded(ctx, target); !ok) return std::unexpected(ok.error());
    if (!replaceable(ctx.block(target))) return std::unexpected(ErrorCode::occupied);
    // The placed block must be able to stand where it goes.
    if (state->is(BlockType::lever) && (replace || !detail::face_sturdy(clicked, c.face))) return std::unexpected(ErrorCode::unsupported);
    if (!detail::can_survive(ctx, target, *state)) return std::unexpected(ErrorCode::unsupported);
    // States the placement takes from the surroundings.
    switch (state->type()) {
    case BlockType::chest: state = detail::placed_chest(ctx, target, *state, c.face, c.sneaking); break;
    case BlockType::redstone_wire: state = detail::fresh_wire_shape(ctx, target, *state, false); break;
    case BlockType::repeater: state = state->with_level(detail::repeater_locked_now(ctx, target, *state) ? 1 : 0); break;
    case BlockType::redstone_lamp: state = state->with_flag(detail::has_neighbor_signal(ctx, target)); break;
    default: break;
    }
    return [&ctx, target, s = *state, c, held] {
        // In survival the item is used up; a water bucket leaves a bucket.
        if (c.survival) {
            auto& inv = ctx.player(c.player);
            auto& slot = inv.slots[c.slot];
            if (held.item == items::water_bucket) slot = ItemStack{items::bucket, 1, 0};
            else if (--slot.count == 0) slot = {};
            detail::record_slot(ctx, inv, c.slot);
        }
        set_block(ctx, target, s);
        // A diode placed with its input on starts after one tick.
        const auto placed = ctx.block(target);
        if ((placed.is(BlockType::repeater) && detail::repeater_input(ctx, target, placed))
            || (placed.is(BlockType::comparator) && detail::comparator_output(ctx, target, placed) > 0))
            detail::schedule(ctx, TickList::block, target, placed.type(), 1, 0);
    };
}

Plan plan(RuleContext& ctx, const contracts::SetPlayerSlot& c) {
    if (c.slot >= contracts::player_slots) return std::unexpected(ErrorCode::invalid_slot);
    const bool clear = c.stack.count == 0;
    if (!clear) {
        const auto* info = items::info(c.stack.item);
        if (!info || c.stack.count > info->max_stack || c.stack.damage > info->max_damage) return std::unexpected(ErrorCode::invalid_argument);
    }
    return [&ctx, c, clear] {
        auto& inv = ctx.player(c.player);
        inv.slots[c.slot] = clear ? ItemStack{} : c.stack;
        detail::record_slot(ctx, inv, c.slot);
        if (c.slot <= 4) detail::update_crafting(ctx, inv);
    };
}

Plan plan(RuleContext& ctx, const contracts::PlayerBreak& c) {
    if (c.slot < contracts::hotbar_first || c.slot >= contracts::offhand_slot) return std::unexpected(ErrorCode::invalid_slot);
    if (auto ok = check_loaded(ctx, c.pos); !ok) return std::unexpected(ok.error());
    const auto s = ctx.block(c.pos);
    if (s.is_air() || s.is(BlockType::moving_piston) || s.is(BlockType::water)) return std::unexpected(ErrorCode::not_breakable);
    const auto* inventory = ctx.peek_player(c.player);
    const auto tool = inventory ? inventory->slots[c.slot] : ItemStack{};
    return [&ctx, c, s, tool] {
        // A piston head takes its base along: a creative player's base goes
        // first and drops nothing; in survival the base breaks with its drop.
        const auto base_pos = c.pos.relative(contracts::opposite(s.facing()));
        const bool head_with_base = s.is(BlockType::piston_head) && detail::head_has_base(ctx, c.pos, s);
        if (head_with_base && !c.survival) set_block(ctx, base_pos, blocks::air());
        if (c.survival && hardness(s) != 0.0f && tool.item != 0) {
            // The tool wears by one and breaks at its durability.
            if (const auto* info = items::info(tool.item); info && info->max_damage > 0) {
                auto& inv = ctx.player(c.player);
                auto& held = inv.slots[c.slot];
                if (++held.damage >= info->max_damage) held = {};
                detail::record_slot(ctx, inv, c.slot);
            }
        }
        set_block(ctx, c.pos, blocks::air());
        if (c.survival) detail::drop_items(ctx, c.pos, s, tool);
        if (head_with_base && c.survival && is_piston(ctx.block(base_pos).type())) detail::destroy_block(ctx, base_pos);
    };
}

Plan plan(RuleContext& ctx, const contracts::PickUp& c) {
    if (c.selected < contracts::hotbar_first || c.selected >= contracts::offhand_slot) return std::unexpected(ErrorCode::invalid_slot);
    const auto* e = ctx.peek_entity(c.entity);
    if (!e || e->stack.empty()) return std::unexpected(ErrorCode::unknown_entity);
    if (e->pickup_delay > 0) return std::unexpected(ErrorCode::not_usable);
    // Nothing happens when the inventory has no room for any of it.
    if (const auto* inv = ctx.peek_player(c.player); inv && !c.creative) {
        const bool room = detail::player_has_room(*inv, e->stack, c.selected);
        if (!room) return std::unexpected(ErrorCode::occupied);
    }
    return [&ctx, c] {
        auto* entity = ctx.entity(c.entity);
        const auto before = entity->stack;
        const auto where = contracts::block_of(entity->position);
        auto left = detail::add_to_player(ctx, ctx.player(c.player), before, c.selected);
        if (c.creative) left = {};
        entity = ctx.entity(c.entity);
        if (left.empty()) {
            ctx.record(TraceKind::entity_removed, where, c.entity.value, static_cast<std::uint64_t>(RemovalReason::picked_up));
            ctx.remove_entity(c.entity);
        } else {
            entity->stack = left;
            ctx.record(TraceKind::entity_stack_changed, where, c.entity.value, contracts::pack_stack(before), contracts::pack_stack(left));
        }
    };
}

Plan plan(RuleContext& ctx, const contracts::DropItem& c) {
    if (c.slot >= contracts::player_slots) return std::unexpected(ErrorCode::invalid_slot);
    if (!contracts::fixed_within_border(c.position) || !ctx.world().contains_fixed_y(c.position.y))
        return std::unexpected(ErrorCode::out_of_bounds);
    const auto cell = contracts::block_of(c.position);
    if (auto ok = check_loaded(ctx, cell); !ok) return std::unexpected(ok.error());
    for (auto v : {c.velocity.x, c.velocity.y, c.velocity.z})
        if (v < -max_entity_speed || v > max_entity_speed) return std::unexpected(ErrorCode::invalid_argument);
    const auto* inv = ctx.peek_player(c.player);
    if (!inv || inv->slots[c.slot].empty()) return std::unexpected(ErrorCode::not_usable);
    return [&ctx, c, cell] {
        auto& player = ctx.player(c.player);
        auto& slot = player.slots[c.slot];
        auto dropped = slot;
        if (!c.whole_stack) dropped.count = 1;
        slot.count = static_cast<std::uint8_t>(slot.count - dropped.count);
        if (slot.count == 0) slot = {};
        detail::record_slot(ctx, player, c.slot);
        // Items a player throws wait 40 ticks before anyone picks them up.
        detail::spawn_dropped(ctx, cell, c.position, c.velocity, dropped, 40);
    };
}

Plan plan(RuleContext& ctx, const contracts::PlayerDeath& c) { return detail::plan_death(ctx, c); }
Plan plan(RuleContext& ctx, const contracts::ContainerClick& c) { return detail::plan_click(ctx, c); }
Plan plan(RuleContext& ctx, const contracts::CloseContainer& c) { return detail::plan_close(ctx, c); }

Plan plan(RuleContext&, const contracts::AddTicket&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::RemoveTicket&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::PlayerPresence&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::LevelsStarted&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::SetGameRule&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::SetDayTime&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::SetWeather&) { return std::unexpected(ErrorCode::unknown_command); }
} // namespace

void apply_command(RuleContext& ctx, const contracts::Command& command) {
    const auto planned = std::visit([&](const auto& body) { return plan(ctx, body); }, command.body);
    if (!planned) {
        ctx.record(TraceKind::command_rejected, {}, command.admission, static_cast<std::uint64_t>(planned.error()));
        return;
    }
    ctx.record(TraceKind::command_applied, {}, command.admission);
    (*planned)();
}

} // namespace circuit::gameplay
