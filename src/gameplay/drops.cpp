// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"

namespace circuit::gameplay::detail {
namespace {
void spawn(RuleContext& ctx, BlockPos pos, ItemStack stack) {
    const contracts::FixedVec center{pos.x * contracts::fixed_one + contracts::fixed_one / 2,
                                     pos.y * contracts::fixed_one + contracts::fixed_one / 2,
                                     pos.z * contracts::fixed_one + contracts::fixed_one / 2};
    const auto id = ctx.spawn_entity(Entity{{}, center, {}, stack, 0, 0, false});
    ctx.record(TraceKind::entity_spawned, pos, id.value, contracts::pack_stack(stack));
}
} // namespace

// Fixed drops without random quantities or offsets (declared simplification).
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed) {
    switch (destroyed.type()) {
    case BlockType::redstone_wire: spawn(ctx, pos, {items::redstone, 1}); break;
    case BlockType::lever: spawn(ctx, pos, {items::lever, 1}); break;
    case BlockType::repeater: spawn(ctx, pos, {items::repeater, 1}); break;
    case BlockType::comparator: spawn(ctx, pos, {items::comparator, 1}); break;
    case BlockType::wheat:
        if (destroyed.level() >= 7) spawn(ctx, pos, {items::wheat, 1});
        spawn(ctx, pos, {items::wheat_seeds, 1});
        break;
    default: break;
    }
}

} // namespace circuit::gameplay::detail
