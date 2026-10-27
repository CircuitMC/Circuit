// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/experience.hpp>
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
            detail::notify_neighbors(ctx, pos.relative(detail::lever_attachment(s)), std::nullopt, s);
        };
    // Using a pressed button does nothing.
    case BlockType::button: return [&ctx, pos = c.pos, s] { detail::press_button(ctx, pos, s); };
    // A command uses blocks as a player facing south does; iron doors and
    // trapdoors do not move.
    case BlockType::door: case BlockType::trapdoor: case BlockType::fence_gate:
        return [&ctx, pos = c.pos, s] {
            if (opens_by_hand(s)) detail::use_opening(ctx, pos, s, Direction::south);
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
    case BlockType::daylight_detector: return [&ctx, pos = c.pos, s] { detail::use_daylight_detector(ctx, pos, s); };
    case BlockType::note_block: return [&ctx, pos = c.pos, s] { detail::use_note_block(ctx, pos, s); };
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
        // A furnace's new input takes its cooking time.
        if (ctx.block(c.pos).is(BlockType::furnace) && c.slot == 0) detail::furnace_input_set(ctx, c.pos, existing);
        for (const auto part : detail::container_parts(ctx, c.pos)) detail::container_changed(ctx, part);
    };
}

Plan plan(RuleContext& ctx, const contracts::SpawnItem& c) {
    // The position is checked before it is narrowed to a block.
    if (!contracts::within_border(c.position) || !ctx.world().contains_position_y(c.position.y))
        return std::unexpected(ErrorCode::out_of_bounds);
    const auto cell = contracts::block_of(c.position);
    if (auto ok = check_loaded(ctx, cell); !ok) return std::unexpected(ok.error());
    if (!valid_stack(c.stack)) return std::unexpected(ErrorCode::invalid_argument);
    if (!valid_velocity(c.velocity)) return std::unexpected(ErrorCode::invalid_argument);
    if (c.pickup_delay > item_pickup_never || !valid_item_age(c.age)) return std::unexpected(ErrorCode::invalid_argument);
    return [&ctx, c] {
        Entity e;
        e.position = c.position;
        e.velocity = c.velocity;
        e.stack = c.stack;
        e.pickup_delay = c.pickup_delay;
        e.age = c.age;
        detail::record_spawn(ctx, ctx.spawn_entity(e), e);
    };
}

Plan plan(RuleContext& ctx, const contracts::SummonEntity& c) {
    if (!contracts::within_border(c.position) || !ctx.world().contains_position_y(c.position.y))
        return std::unexpected(ErrorCode::out_of_bounds);
    if (auto ok = check_loaded(ctx, contracts::block_of(c.position)); !ok) return std::unexpected(ok.error());
    if (!valid_velocity(c.velocity) || c.fuse < -32768 || c.fuse > 32767 || c.fire < -32768 || c.fire > 32767)
        return std::unexpected(ErrorCode::invalid_argument);
    const bool orb = c.type == "minecraft:experience_orb";
    if ((c.value != 0 || c.count != 1 || c.age != 0) && !orb) return std::unexpected(ErrorCode::invalid_argument);
    if (orb && (c.value < 0 || c.count < 1 || c.age < -32768 || c.age > 32767)) return std::unexpected(ErrorCode::invalid_argument);
    Entity e;
    if (c.type == "minecraft:tnt") e.kind = EntityKind::primed_tnt, e.fuse = c.fuse;
    else if (c.type == "minecraft:arrow") e.kind = EntityKind::arrow, e.fire = c.fire;
    else if (c.type == "minecraft:snowball") e.kind = EntityKind::snowball;
    else if (c.type == "minecraft:minecart") e.kind = EntityKind::minecart;
    else if (orb) e.kind = EntityKind::experience_orb, e.experience = c.value, e.orbs = c.count, e.age = c.age;
    else if (const auto boat = std::ranges::find(boat_types, c.type); boat != boat_types.end())
        e.kind = EntityKind::boat, e.variant = static_cast<std::uint8_t>(boat - boat_types.begin());
    else return std::unexpected(ErrorCode::unknown_command);
    e.position = c.position;
    e.velocity = c.velocity;
    return [&ctx, e] { detail::record_spawn(ctx, ctx.spawn_entity(e), e); };
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
bool replaceable(BlockState s) { return s.is_air() || s.is(BlockType::water) || is_lava(s); }
bool usable(BlockState s) {
    return s.is(BlockType::lever) || s.is(BlockType::button) || s.is(BlockType::repeater) || s.is(BlockType::comparator)
        || s.is(BlockType::daylight_detector) || s.is(BlockType::note_block) || detail::is_dot(s) || detail::is_cross(s) || (is_opening(s.type()) && opens_by_hand(s));
}

Plan plan(RuleContext& ctx, const contracts::UseBlock& c);

// A building block without rules of its own placed by a player: into the
// clicked cell when it is replaceable or a slab the click doubles, else
// beside it.
Plan plan_building(RuleContext& ctx, const contracts::PlayerUse& c, const ItemStack& held, BlockState clicked, std::uint32_t block) {
    const bool replace = replaceable(clicked) || detail::doubles_slab(block, clicked, c, true) || detail::snow_replaced(block, clicked, c.face, true);
    const auto target = replace ? c.pos : c.pos.relative(c.face);
    const std::uint8_t bit = replace ? 1 : 2;
    if ((c.reach & bit) == 0) return std::unexpected(ErrorCode::out_of_reach);
    if (auto ok = check_loaded(ctx, target); !ok) return std::unexpected(ok.error());
    const auto here = ctx.block(target);
    if (!replaceable(here)
        && !(target == c.pos ? replace : detail::doubles_slab(block, here, c, false) || detail::snow_replaced(block, here, c.face, false)))
        return std::unexpected(ErrorCode::occupied);
    const auto placed = detail::placed_building(ctx, block, c, target);
    if (!placed) return std::unexpected(ErrorCode::unsupported);
    const auto state = *placed;
    // Its block entity would not keep an item's components (contents, a
    // name, patterns): such an item places nothing.
    if (!held.components.empty() && state.is(BlockType::other) && !registry::block_entity_type(state.registry_state()).empty())
        return std::unexpected(ErrorCode::unsupported);
    if (detail::has_collision(state) && (c.free & bit) == 0) return std::unexpected(ErrorCode::occupied);
    // A tall flower needs room for its upper half, a bed for its head.
    const auto second = detail::second_part(state, target);
    if (second) {
        if (!in_world(ctx.world(), second->first)) return std::unexpected(ErrorCode::occupied);
        if (auto ok = check_loaded(ctx, second->first); !ok) return std::unexpected(ok.error());
        if (!replaceable(ctx.block(second->first))) return std::unexpected(ErrorCode::occupied);
    }
    // A block entity of a block without rules of its own starts empty
    // (written out as its type's defaults).
    const auto with_entity = [&ctx](BlockPos pos, BlockState s) {
        set_block(ctx, pos, s);
        if (s.is(BlockType::other) && !registry::block_entity_type(s.registry_state()).empty() && !ctx.peek_block_entity(pos))
            ctx.create_block_entity(pos, BlockType::other);
    };
    return [&ctx, target, state, second, with_entity, c, held] {
        if (c.survival) {
            auto& inv = ctx.player(c.player);
            auto& slot = inv.slots[c.slot];
            if (--slot.count == 0) slot = {};
            detail::record_slot(ctx, inv, c.slot);
        }
        with_entity(target, state);
        if (second) with_entity(second->first, second->second);
    };
}

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
        if (is_opening(clicked.type()) && opens_by_hand(clicked))
            return [&ctx, c, clicked] { detail::use_opening(ctx, c.pos, clicked, c.horizontal); };
        if (usable(clicked)) return plan(ctx, contracts::UseBlock{c.pos});
        if (is_container(clicked.type())) {
            // A chest under a solid block does not open.
            for (const auto part : detail::container_parts(ctx, c.pos))
                if (ctx.block(part).is(BlockType::chest) && detail::is_conductor(ctx.block(part.relative(Direction::up))))
                    return std::unexpected(ErrorCode::not_usable);
            return [&ctx, c] { ctx.record(TraceKind::container_opened, c.pos, c.player.hi, c.player.lo); };
        }
        if (is_crafting_table(clicked) || is_stonecutter(clicked) || is_smithing_table(clicked) || is_grindstone(clicked) || is_loom(clicked)
            || is_anvil(clicked) || is_enchanting_table(clicked))
            return [&ctx, c] { ctx.record(TraceKind::container_opened, c.pos, c.player.hi, c.player.lo); };
        // A lectern with a book opens its window; one without takes a book
        // (the item tag lectern_books; used up outside creative mode), and
        // another item goes on to its own use.
        if (clicked.is(BlockType::lectern)) {
            if (clicked.extra() != 0) return [&ctx, c] { ctx.record(TraceKind::container_opened, c.pos, c.player.hi, c.player.lo); };
            if (detail::lectern_takes(ctx, held))
                return [&ctx, c, held] {
                    detail::put_lectern_book(ctx, c.pos, held);
                    if (!c.survival) return;
                    auto& inv = ctx.player(c.player);
                    auto& slot = inv.slots[c.slot];
                    if (--slot.count == 0) slot = {};
                    detail::record_slot(ctx, inv, c.slot);
                };
        }
    }
    // Bone meal acts on the clicked block as a dispenser's does, used up in
    // survival when it acts (not compared with a client yet).
    static const auto bone_meal = items::by_name("minecraft:bone_meal");
    if (!held.empty() && bone_meal && held.item == *bone_meal) {
        if ((c.reach & 1) == 0) return std::unexpected(ErrorCode::out_of_reach);
        return [&ctx, c] {
            if (!detail::apply_bone_meal(ctx, c.pos) || !c.survival) return;
            auto& inv = ctx.player(c.player);
            auto& slot = inv.slots[c.slot];
            if (--slot.count == 0) slot = {};
            detail::record_slot(ctx, inv, c.slot);
        };
    }
    auto state = held.empty() ? std::nullopt : detail::placed_by(held.item, c);
    if (!state && held.item == items::tnt) state = detail::tnt_block(ctx);
    if (!state && !held.empty())
        if (const auto building = detail::building_block(ctx, held.item)) return plan_building(ctx, c, held, clicked, *building);
    if (!state) return std::unexpected(ErrorCode::not_usable);
    // A single snow layer is replaced as air is (place_snow).
    const auto replaced = [](BlockState s) { return replaceable(s) || detail::single_snow_layer(s); };
    const bool replace = replaced(clicked);
    const auto target = replace ? c.pos : c.pos.relative(c.face);
    const std::uint8_t bit = replace ? 1 : 2;
    if ((c.reach & bit) == 0) return std::unexpected(ErrorCode::out_of_reach);
    if (detail::has_collision(*state) && (c.free & bit) == 0) return std::unexpected(ErrorCode::occupied);
    if (auto ok = check_loaded(ctx, target); !ok) return std::unexpected(ok.error());
    if (!replaced(ctx.block(target))) return std::unexpected(ErrorCode::occupied);
    // The placed block must be able to stand where it goes.
    if ((state->is(BlockType::lever) || state->is(BlockType::button)) && (replace || !detail::face_sturdy(clicked, c.face)))
        return std::unexpected(ErrorCode::unsupported);
    if (!detail::can_survive(ctx, target, *state)) return std::unexpected(ErrorCode::unsupported);
    // A door needs room for its upper half.
    const auto above = target.relative(Direction::up);
    if (state->is(BlockType::door) && (!in_world(ctx.world(), above) || !replaceable(ctx.block(above))))
        return std::unexpected(ErrorCode::occupied);
    // States the placement takes from the surroundings.
    switch (state->type()) {
    case BlockType::chest: state = detail::placed_chest(ctx, target, *state, c.face, c.sneaking); break;
    case BlockType::redstone_wire: state = detail::fresh_wire_shape(ctx, target, *state, false); break;
    case BlockType::repeater: state = state->with_level(detail::repeater_locked_now(ctx, target, *state) ? 1 : 0); break;
    case BlockType::redstone_lamp: state = state->with_flag(detail::has_neighbor_signal(ctx, target)); break;
    case BlockType::door: case BlockType::trapdoor: case BlockType::fence_gate: state = detail::placed_opening(ctx, target, *state, c); break;
    case BlockType::note_block: state = detail::tuned_note_block(ctx, target, *state); break;
    default: break;
    }
    return [&ctx, target, s = *state, c, held] {
        // In survival the item is used up.
        if (c.survival) {
            auto& inv = ctx.player(c.player);
            auto& slot = inv.slots[c.slot];
            if (--slot.count == 0) slot = {};
            detail::record_slot(ctx, inv, c.slot);
        }
        set_block(ctx, target, s);
        // A door's upper half follows its lower half.
        if (s.is(BlockType::door)) set_block(ctx, target.relative(Direction::up), s.with_level(static_cast<std::uint8_t>(s.level() | 1)));
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
    if (s.is_air() || s.is(BlockType::moving_piston) || s.is(BlockType::water) || is_lava(s)) return std::unexpected(ErrorCode::not_breakable);
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
        if (c.survival) {
            detail::drop_items(ctx, c.pos, s, tool, true);
            // What the tool harvests gives experience too.
            if (harvests(s, tool, ctx.data())) detail::drop_experience(ctx, c.pos, s, tool);
            detail::ice_broken(ctx, c.pos, s, tool);
        }
        if (head_with_base && c.survival && is_piston(ctx.block(base_pos).type())) detail::destroy_block(ctx, base_pos);
    };
}

Plan plan(RuleContext& ctx, const contracts::PickUp& c) {
    if (c.selected < contracts::hotbar_first || c.selected >= contracts::offhand_slot) return std::unexpected(ErrorCode::invalid_slot);
    const auto* e = ctx.peek_entity(c.entity);
    // An experience orb: a player takes one of its orbs every 2 ticks, gaining
    // its experience (the official servers' levels and progress, checked with
    // a protocol client); the last one is gone. Mending is not modeled.
    if (e && e->kind == EntityKind::experience_orb) {
        if (const auto* inv = ctx.peek_player(c.player); inv && ctx.now() < inv->xp_ready) return std::unexpected(ErrorCode::not_usable);
        return [&ctx, c] {
            auto& player = ctx.player(c.player);
            auto* orb = ctx.entity(c.entity);
            player.xp_ready = ctx.now() + 2;
            experience::give_points(player, orb->experience);
            ctx.record(TraceKind::player_experience_changed, contracts::block_of(orb->position), c.player.hi, c.player.lo,
                       static_cast<std::uint64_t>(static_cast<std::uint32_t>(player.xp_total)));
            if (--orb->orbs > 0) return;
            ctx.record(TraceKind::entity_removed, contracts::block_of(orb->position), c.entity.value,
                       static_cast<std::uint64_t>(RemovalReason::picked_up));
            ctx.remove_entity(c.entity);
        };
    }
    if (!e || e->kind != EntityKind::item || e->stack.empty()) return std::unexpected(ErrorCode::unknown_entity);
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
    if (!contracts::within_border(c.position) || !ctx.world().contains_position_y(c.position.y))
        return std::unexpected(ErrorCode::out_of_bounds);
    const auto cell = contracts::block_of(c.position);
    if (auto ok = check_loaded(ctx, cell); !ok) return std::unexpected(ok.error());
    if (!valid_velocity(c.velocity)) return std::unexpected(ErrorCode::invalid_argument);
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
Plan plan(RuleContext& ctx, const contracts::ContainerButton& c) { return detail::plan_button(ctx, c); }
Plan plan(RuleContext& ctx, const contracts::SetSlotEnabled& c) { return detail::plan_slot_enabled(ctx, c); }
Plan plan(RuleContext& ctx, const contracts::PlayerUseItem& c) { return detail::plan_use_item(ctx, c); }

// The name a player types into their anvil window: the result follows it.
Plan plan(RuleContext& ctx, const contracts::RenameItem& c) {
    if (!detail::valid_item_name(c.name)) return std::unexpected(ErrorCode::invalid_argument);
    return [&ctx, c] {
        auto& inv = ctx.player(c.player);
        if (inv.anvil_name == c.name) return;
        inv.anvil_name = c.name;
        detail::set_anvil_result(ctx, inv, c.creative);
    };
}

// The /xp command: levels set or added (below 0 none, and no experience at
// all), points added (taking levels away when negative) or set within the
// level (below what it needs).
Plan plan(RuleContext& ctx, const contracts::ChangeExperience& c) {
    const auto* inv = ctx.peek_player(c.player);
    if (c.set && c.amount < 0) return std::unexpected(ErrorCode::invalid_argument);
    if (c.set && !c.levels && c.amount >= experience::needed(inv ? inv->xp_level : 0)) return std::unexpected(ErrorCode::invalid_argument);
    return [&ctx, c] {
        auto& player = ctx.player(c.player);
        const auto before = player.xp_total;
        if (c.levels && c.set) player.xp_level = c.amount;
        else if (c.levels) experience::give_levels(player, c.amount);
        else if (c.set) experience::set_points(player, c.amount);
        else experience::give_points(player, c.amount);
        if (player.xp_total != before)
            ctx.record(TraceKind::player_experience_changed, {}, c.player.hi, c.player.lo,
                       static_cast<std::uint64_t>(static_cast<std::uint32_t>(player.xp_total)));
    };
}

Plan plan(RuleContext&, const contracts::AddTicket&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::RemoveTicket&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::PlayerPresence&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::LevelsStarted&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::SetGameRule&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::SetDayTime&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::SetWeather&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::SetDifficulty&) { return std::unexpected(ErrorCode::unknown_command); }
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
