// SPDX-License-Identifier: AGPL-3.0-only
// Container openers: the players viewing a chest, counted by each chest half,
// a barrel or an ender chest, as both official servers count them
// (client/openers, client/barrel, client/enderchest; minecraft.wiki "Chest",
// "Trapped Chest", "Barrel", "Ender Chest"):
// - A player opening a chest views each of its halves; closing its window
//   (leaving too), dying or opening another container stops that.
// - Every change of a half's count, and every recheck while it is viewed,
//   queues the half's block event 1 with the count (the lid clients show);
//   an ender chest's count is of the players viewing their ender inventory
//   through it. A barrel queues none: it is open from its first viewer to
//   its last.
// - The first viewer schedules a recheck 5 ticks later, which keeps the
//   viewers still in the level, and comes again while any remain. A viewer
//   stays one as far as their window reaches (client/openers2: 9 blocks
//   away, the window closing beyond); closing a window, dying and leaving
//   stop viewing in the command phase.
// - A trapped chest gives its count as power on every side and strongly to
//   the block below; a count that changed tells its neighbors and the
//   neighbors of the block below.
#include "internal.hpp"
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
constexpr std::uint32_t recheck_ticks = 5;
constexpr std::uint8_t lid_event = 1;

bool is_trapped(BlockState s) { return s.is(BlockType::chest) && s.variant() == 1; }

// The first viewer opens a barrel, the last one gone closes it.
void opened(RuleContext& ctx, BlockPos pos, bool open) {
    const auto s = ctx.block(pos);
    if (s.is(BlockType::barrel)) set_block(ctx, pos, s.with_flag(open));
}

// The blocks a viewer of the container at `pos` views: a chest's halves, a
// barrel, an ender chest.
std::vector<BlockPos> viewed_parts(RuleContext& ctx, BlockPos pos) {
    if (ctx.block(pos).is(BlockType::ender_chest)) return {pos};
    return container_parts(ctx, pos);
}

void count_changed(RuleContext& ctx, BlockPos pos, std::size_t before, std::size_t now) {
    const auto type = ctx.block(pos).type();
    if (type != BlockType::chest && type != BlockType::ender_chest) return;
    queue_block_event(ctx, {pos, type, lid_event, static_cast<std::uint8_t>(std::min<std::size_t>(now, 255))});
    if (before == now || !is_trapped(ctx.block(pos))) return;
    notify_neighbors(ctx, pos);
    notify_neighbors(ctx, pos.relative(Direction::down), std::nullopt, ctx.block(pos));
}

// Whether a viewer is still in the level.
bool still_here(RuleContext& ctx, contracts::PlayerKey player) {
    const auto presence = ctx.presence_of(player);
    return presence && !presence->spectator;
}
} // namespace

bool counts_viewers(BlockState s) { return s.is(BlockType::chest) || s.is(BlockType::barrel) || s.is(BlockType::ender_chest); }

void start_viewing(RuleContext& ctx, PlayerInventory& inv, BlockPos pos) {
    stop_viewing(ctx, inv);
    if (!counts_viewers(ctx.block(pos))) return;
    inv.viewing = pos;
    for (const auto part : viewed_parts(ctx, pos)) {
        auto* be = ctx.block_entity(part);
        if (!be || std::ranges::find(be->viewers, inv.player) != be->viewers.end()) continue;
        be->viewers.push_back(inv.player);
        const auto now = be->viewers.size();
        if (now == 1) {
            opened(ctx, part, true);
            schedule(ctx, TickList::block, part, ctx.block(part).type(), recheck_ticks, 0);
        }
        count_changed(ctx, part, now - 1, now);
    }
}

void stop_viewing(RuleContext& ctx, PlayerInventory& inv) {
    if (!inv.viewing) return;
    const auto pos = *inv.viewing;
    inv.viewing.reset();
    for (const auto part : viewed_parts(ctx, pos)) {
        auto* be = ctx.block_entity(part);
        if (!be) continue;
        const auto it = std::ranges::find(be->viewers, inv.player);
        if (it == be->viewers.end()) continue;
        be->viewers.erase(it);
        const auto now = be->viewers.size();
        if (now == 0) opened(ctx, part, false);
        count_changed(ctx, part, now + 1, now);
    }
}

std::uint8_t viewer_count(RuleContext& ctx, BlockPos pos) {
    const auto* be = ctx.peek_block_entity(pos);
    return be ? static_cast<std::uint8_t>(std::min<std::size_t>(be->viewers.size(), 15)) : 0;
}

std::uint8_t trapped_power(RuleContext& ctx, BlockPos pos, BlockState s) { return is_trapped(s) ? viewer_count(ctx, pos) : 0; }

void tick_viewers(RuleContext& ctx, BlockPos pos) {
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    const auto before = be->viewers.size();
    std::erase_if(be->viewers, [&](contracts::PlayerKey v) { return !still_here(ctx, v); });
    const auto now = be->viewers.size();
    if (before > 0 && now == 0) opened(ctx, pos, false);
    count_changed(ctx, pos, before, now);
    if (now > 0) schedule(ctx, TickList::block, pos, ctx.block(pos).type(), recheck_ticks, 0);
}

} // namespace circuit::gameplay::detail
