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
    case BlockType::lever: return [&ctx, pos = c.pos, s] { set_block(ctx, pos, s.with_flag(!s.flag())); };
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
