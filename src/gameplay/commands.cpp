// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <functional>

namespace circuit::gameplay {
namespace {
using contracts::ErrorCode;
using contracts::Result;

template <class... F>
struct overloaded : F... { using F::operator()...; };

Result<void> check_loaded(RuleContext& ctx, BlockPos pos) {
    if (!in_world(ctx.bundle(), pos)) return std::unexpected(ErrorCode::out_of_bounds);
    if (!ctx.chunk_generation(contracts::chunk_of(pos))) return std::unexpected(ErrorCode::unloaded_chunk);
    return {};
}

bool valid_stack(ItemStack s) {
    return s.item >= 1 && s.item <= items::max_id && s.count >= 1 && s.count <= contracts::max_stack_size;
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
    if (!existing.empty() && existing.item != c.stack.item) return std::unexpected(ErrorCode::item_mismatch);
    const unsigned total = (existing.empty() ? 0u : existing.count) + c.stack.count;
    if (total > contracts::max_stack_size) return std::unexpected(ErrorCode::stack_overflow);
    return [&ctx, c, existing, total] {
        auto* target = ctx.block_entity(c.pos);
        const ItemStack after{c.stack.item, static_cast<std::uint8_t>(total)};
        target->slots[c.slot] = after;
        ctx.record(TraceKind::inventory_changed, c.pos, c.slot, contracts::pack_stack(existing), contracts::pack_stack(after));
        for (const auto part : detail::container_parts(ctx, c.pos)) detail::container_changed(ctx, part);
    };
}

Plan plan(RuleContext& ctx, const contracts::SpawnItem& c) {
    // The full-width position is checked before it is narrowed to a block.
    if (!contracts::fixed_within_border(c.position) || !ctx.bundle().world.contains_fixed_y(c.position.y))
        return std::unexpected(ErrorCode::out_of_bounds);
    const auto cell = contracts::block_of(c.position);
    if (auto ok = check_loaded(ctx, cell); !ok) return std::unexpected(ok.error());
    if (!valid_stack(c.stack)) return std::unexpected(ErrorCode::invalid_argument);
    for (auto v : {c.velocity.x, c.velocity.y, c.velocity.z})
        if (v <= -contracts::fixed_one || v >= contracts::fixed_one) return std::unexpected(ErrorCode::invalid_argument);
    return [&ctx, c, cell] {
        const auto id = ctx.spawn_entity(Entity{{}, c.position, c.velocity, c.stack, 0, 0, false});
        ctx.record(TraceKind::entity_spawned, cell, id.value, contracts::pack_stack(c.stack));
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
    if (auto ok = check_loaded(ctx, c.pos); !ok) return std::unexpected(ok.error());
    const auto clicked = ctx.block(c.pos);
    if (usable(clicked) && !(c.sneaking && c.holding)) return plan(ctx, contracts::UseBlock{c.pos});
    if (!c.place) return std::unexpected(ErrorCode::not_usable);
    auto state = BlockState::decode(*c.place);
    if (!state) return std::unexpected(state.error());
    if (state->is_air() || state->is(BlockType::piston_head) || state->is(BlockType::moving_piston)
        || (is_piston(state->type()) && state->flag()))
        return std::unexpected(ErrorCode::invalid_block_state);
    const bool replace = replaceable(clicked);
    const auto target = replace ? c.pos : c.pos.relative(c.face);
    if ((c.allowed & (replace ? 1 : 2)) == 0) return std::unexpected(ErrorCode::out_of_reach);
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
    return [&ctx, target, s = *state] {
        set_block(ctx, target, s);
        // A diode placed with its input on starts after one tick.
        const auto placed = ctx.block(target);
        if ((placed.is(BlockType::repeater) && detail::repeater_input(ctx, target, placed))
            || (placed.is(BlockType::comparator) && detail::comparator_output(ctx, target, placed) > 0))
            detail::schedule(ctx, TickList::block, target, placed.type(), 1, 0);
    };
}

Plan plan(RuleContext& ctx, const contracts::PlayerBreak& c) {
    if (auto ok = check_loaded(ctx, c.pos); !ok) return std::unexpected(ok.error());
    const auto s = ctx.block(c.pos);
    if (s.is_air() || s.is(BlockType::piston_head) || s.is(BlockType::moving_piston)) return std::unexpected(ErrorCode::not_breakable);
    return [&ctx, pos = c.pos] { set_block(ctx, pos, blocks::air()); };
}

Plan plan(RuleContext&, const contracts::AddTicket&) { return std::unexpected(ErrorCode::unknown_command); }
Plan plan(RuleContext&, const contracts::RemoveTicket&) { return std::unexpected(ErrorCode::unknown_command); }
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
