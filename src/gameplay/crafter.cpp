// SPDX-License-Identifier: AGPL-3.0-only
// Crafters, as both official servers run them (checked against their
// traces; minecraft.wiki "Crafter").
//
// A neighbor update that finds power at the block (not above it) sets it
// triggered without neighbor updates and schedules a tick 4 ticks later;
// one that finds none clears triggered. The tick crafts the grid by the
// first crafting recipe (in id order) that takes it, disabled slots counting
// as empty: every grid slot gives one item, the crafter shows that it
// crafted for 6 block-entity ticks (from the same tick on), and the result,
// then each remainder (an empty bucket of a milk bucket) in slot order, goes
// into the container in front, slot by slot, or what it does not take is
// thrown out through the front whole, as dispensers throw items. Without a
// recipe it only fails (a sound).
//
// Hoppers and droppers put an item into the first enabled slot that is
// empty, or holds its kind with room while no other enabled slot is empty
// or holds fewer of it, so that they fill the grid evenly. A comparator
// reads the slots that hold something or are disabled.
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <array>
#include <vector>

namespace circuit::gameplay::detail {
namespace {
constexpr std::uint32_t craft_delay = 4;
constexpr std::int32_t crafting_ticks = 6;

bool disabled(const BlockEntity& be, std::size_t slot) { return (be.disabled >> slot & 1) != 0; }

// The crafter's state set without neighbor updates, as both official servers set it.
void set_crafter(RuleContext& ctx, BlockPos pos, BlockState now) {
    if (ctx.block(pos) != now) set_block_quiet(ctx, pos, now);
}

// The result, or what is left of it, through the front.
void deliver(RuleContext& ctx, BlockPos pos, Direction front, ItemStack stack) {
    const auto there = pos.relative(front);
    if (is_container(ctx.block(there).type())) stack = insert_stack(ctx, there, stack, contracts::opposite(front));
    // A composter below takes one item of it; the rest goes out (composter_inputs2).
    if (ctx.block(there).is(BlockType::composter) && compost_through(ctx, there, contracts::opposite(front), stack))
        if (--stack.count == 0) stack = {};
    if (!stack.empty()) eject_from(ctx, pos, front, stack, true);
}
} // namespace

bool crafter_accepts(const BlockEntity& be, std::size_t slot, const ItemStack& stack) {
    if (disabled(be, slot)) return false;
    const auto& s = be.slots[slot];
    if (s.empty()) return true;
    if (!s.same_kind(stack) || s.count >= std::min(contracts::max_stack_size, items::max_stack(s))) return false;
    for (std::size_t k = 0; k < be.slots.size(); ++k) {
        if (k == slot || disabled(be, k)) continue;
        const auto& other = be.slots[k];
        if (other.empty() || (other.same_kind(s) && other.count < s.count)) return false;
    }
    return true;
}

void check_crafter(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::crafter)) return;
    const bool powered = has_neighbor_signal(ctx, pos);
    if (powered && !s.flag()) {
        schedule(ctx, TickList::block, pos, BlockType::crafter, craft_delay, 0);
        set_crafter(ctx, pos, s.with_flag(true));
    } else if (!powered && s.flag()) {
        // It stops showing that it crafted, too (composter_inputs).
        set_crafter(ctx, pos, s.with_flag(false).with_extra(0));
    }
}

void tick_crafter(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto* be = ctx.block_entity(pos);
    const auto* data = ctx.data();
    if (!be || be->slots.size() != 9 || !data) return;
    std::array<ItemStack, 9> grid{};
    for (std::size_t k = 0; k < grid.size(); ++k)
        if (!disabled(*be, k)) grid[k] = be->slots[k];
    // The first recipe in id order, a special one too; a stack a special
    // recipe keeps (a banner copied) goes out with the result.
    const auto [made, special] = grid_recipe(*data, {3, 3, grid});
    const auto result = made ? made->result : special ? special->result : ItemStack{};
    if (result.empty()) return; // it fails
    be->crafting_ticks = crafting_ticks;
    set_crafter(ctx, pos, s.with_extra(1));
    std::vector<ItemStack> remainders;
    for (std::size_t k = 0; k < grid.size(); ++k) {
        if (grid[k].empty()) continue;
        auto next = be->slots[k];
        next.count = static_cast<std::uint8_t>(next.count - 1);
        if (next.count == 0) next = {};
        ctx.record(TraceKind::inventory_changed, pos, k, contracts::pack_stack(be->slots[k]), contracts::pack_stack(next));
        be->slots[k] = next;
        if (special && special->kept == k) {
            auto kept = grid[k];
            kept.count = 1;
            remainders.push_back(kept);
        } else if (const auto left = recipes::remainder(grid[k].item); !left.empty()) {
            remainders.push_back(left);
        }
    }
    container_changed(ctx, pos);
    deliver(ctx, pos, s.facing(), result);
    for (const auto& r : remainders) deliver(ctx, pos, s.facing(), r);
}

void tick_crafter_entity(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto* be = ctx.block_entity(pos);
    if (!be || be->crafting_ticks <= 0) return;
    if (--be->crafting_ticks == 0) set_crafter(ctx, pos, s.with_extra(0));
}

} // namespace circuit::gameplay::detail
