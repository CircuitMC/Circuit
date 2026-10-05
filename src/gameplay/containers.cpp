// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"

namespace circuit::gameplay::detail {
namespace {
using contracts::pack_stack;
constexpr std::uint8_t hopper_cooldown = 8;

// First slot, in index order, that accepts one more `item`.
int insert_slot(const BlockEntity& be, std::uint16_t item) {
    for (std::size_t i = 0; i < be.slots.size(); ++i) {
        const auto& s = be.slots[i];
        if (s.empty() || (s.item == item && s.count < contracts::max_stack_size)) return static_cast<int>(i);
    }
    return -1;
}

void add_one(BlockEntity& be, std::size_t slot, std::uint16_t item) {
    auto& s = be.slots[slot];
    s = s.empty() ? ItemStack{item, 1} : ItemStack{item, static_cast<std::uint8_t>(s.count + 1)};
}

void take_one(ItemStack& s) {
    s.count = static_cast<std::uint8_t>(s.count - 1);
    if (s.count == 0) s = {};
}

// Moves one item from `from` to `to`, trying source slots in index order.
bool transfer_one(RuleContext& ctx, BlockPos from_pos, BlockEntity& from, BlockPos to_pos, BlockEntity& to) {
    for (std::size_t i = 0; i < from.slots.size(); ++i) {
        if (from.slots[i].empty()) continue;
        const auto item = from.slots[i].item;
        const auto j = insert_slot(to, item);
        if (j < 0) continue;
        const auto before_from = from.slots[i];
        const auto before_to = to.slots[static_cast<std::size_t>(j)];
        take_one(from.slots[i]);
        add_one(to, static_cast<std::size_t>(j), item);
        ctx.record(TraceKind::inventory_changed, from_pos, i, pack_stack(before_from), pack_stack(from.slots[i]));
        ctx.record(TraceKind::inventory_changed, to_pos, static_cast<std::uint64_t>(j), pack_stack(before_to),
                   pack_stack(to.slots[static_cast<std::size_t>(j)]));
        return true;
    }
    return false;
}

bool pick_up(RuleContext& ctx, BlockPos pos, BlockEntity& self, BlockPos cell) {
    for (const auto id : ctx.entities_in_cell(cell)) {
        auto* e = ctx.entity(id);
        if (!e || e->stack.empty()) continue;
        const auto before = e->stack;
        bool moved = false;
        while (!e->stack.empty()) {
            const auto j = insert_slot(self, e->stack.item);
            if (j < 0) break;
            const auto slot_before = self.slots[static_cast<std::size_t>(j)];
            add_one(self, static_cast<std::size_t>(j), e->stack.item);
            take_one(e->stack);
            ctx.record(TraceKind::inventory_changed, pos, static_cast<std::uint64_t>(j), pack_stack(slot_before),
                       pack_stack(self.slots[static_cast<std::size_t>(j)]));
            moved = true;
        }
        if (!moved) continue;
        const auto where = contracts::block_of(e->position);
        if (e->stack.empty()) {
            ctx.record(TraceKind::entity_removed, where, id.value, static_cast<std::uint64_t>(RemovalReason::picked_up));
            ctx.remove_entity(id);
        } else {
            ctx.record(TraceKind::entity_stack_changed, where, id.value, pack_stack(before), pack_stack(e->stack));
        }
        return true;
    }
    return false;
}
} // namespace

void check_hopper(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::hopper)) return;
    const bool enabled = received_power(ctx, pos) == 0;
    if (enabled != s.flag()) set_block(ctx, pos, s.with_flag(enabled));
}

void tick_hopper(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::hopper)) return;
    auto* self = ctx.block_entity(pos);
    if (!self) return;
    if (self->cooldown > 0) {
        --self->cooldown;
        if (self->cooldown > 0) return;
    }
    if (!s.flag()) return;
    bool moved = false;
    const auto target_pos = pos.relative(s.facing());
    if (auto* target = ctx.block_entity(target_pos)) moved = transfer_one(ctx, pos, *self, target_pos, *target);
    const auto above_pos = pos.relative(Direction::up);
    const auto above = ctx.block(above_pos);
    if (has_block_entity(above.type())) {
        if (auto* source = ctx.block_entity(above_pos))
            moved = transfer_one(ctx, above_pos, *source, pos, *self) || moved;
    } else if (!above.is_unloaded() && !is_solid(above.type())) {
        moved = pick_up(ctx, pos, *self, above_pos) || moved;
    }
    if (moved) self->cooldown = hopper_cooldown;
}

} // namespace circuit::gameplay::detail
