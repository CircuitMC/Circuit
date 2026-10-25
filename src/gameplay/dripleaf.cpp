// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"

// Big dripleaves tilting (both official servers; dripleaves): an entity on
// the ground above three quarters of the leaf's height (y + 0.6875) tilts an
// untilted, unpowered leaf to unstable, without a sound. Its block tick then
// takes it to partial after 10 ticks and to full after 10 more, where its
// collision shape lets entities through, and back after 100; each of these
// three steps plays a sound whose pitch is a level-random draw. A signal
// beside the leaf takes it back at once (with a sound when it was tilted),
// and its pending tick then only keeps it there. An arrow or a snowball
// hitting the leaf tilts it to full at once, with a sound, and it goes back
// 100 ticks later (dripleaf_arrows). The state changes send shape updates
// only, as the official servers set them.
namespace circuit::gameplay::detail {
namespace {
std::string tilt_of(BlockState s) {
    for (const auto& [k, v] : registry::properties(s.registry_state()))
        if (k == "tilt") return v;
    return {};
}

BlockState with_tilt(BlockState s, std::string_view tilt) {
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        if (k == "tilt") v = std::string(tilt);
    return BlockState::registered(registry::state_of(*registry::block_of(s.registry_state()), props).value());
}

void set_tilt(RuleContext& ctx, BlockPos pos, BlockState s, std::string_view tilt, std::uint32_t next, bool sound) {
    set_block_quiet(ctx, pos, with_tilt(s, tilt));
    if (sound) (void)ctx.random_float(); // the sound's pitch
    if (next > 0) schedule(ctx, TickList::block, pos, BlockType::other, next, 0);
}

void reset_tilt(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool tilted = tilt_of(s) != "none";
    set_tilt(ctx, pos, s, "none", 0, tilted);
}
} // namespace

bool is_big_dripleaf(BlockState s) {
    const auto block = s.is(BlockType::other) ? registry::block_of(s.registry_state()) : std::nullopt;
    return block && registry::game_name(*block) == "big_dripleaf";
}

void dripleaf_entity_inside(RuleContext& ctx, BlockPos pos, BlockState s, const Entity& e) {
    if (tilt_of(s) != "none" || !e.on_ground || e.position.y <= static_cast<double>(pos.y) + 0.6875) return;
    if (has_neighbor_signal(ctx, pos)) return;
    set_tilt(ctx, pos, s, "unstable", 10, false);
}

void tick_dripleaf(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (has_neighbor_signal(ctx, pos)) {
        reset_tilt(ctx, pos, s);
        return;
    }
    const auto tilt = tilt_of(s);
    if (tilt == "unstable") set_tilt(ctx, pos, s, "partial", 10, true);
    else if (tilt == "partial") set_tilt(ctx, pos, s, "full", 100, true);
    else if (tilt == "full") reset_tilt(ctx, pos, s);
}

void dripleaf_hit(RuleContext& ctx, BlockPos pos) {
    if (const auto s = ctx.block(pos); is_big_dripleaf(s)) set_tilt(ctx, pos, s, "full", 100, true);
}

void dripleaf_neighbor_changed(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (has_neighbor_signal(ctx, pos)) reset_tilt(ctx, pos, s);
}

} // namespace circuit::gameplay::detail
