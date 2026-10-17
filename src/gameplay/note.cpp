// SPDX-License-Identifier: AGPL-3.0-only
// Note blocks, as both official servers run them (checked against their
// traces; minecraft.wiki "Note Block").
//
// A note block plays the instrument of a mob head above it, or else of the
// block below it (block_registry.hpp: Instrument; a head below gives the
// harp). It takes the instrument when it is placed, moved and on a shape
// update from above or below. A neighbor update that finds the power around
// it different from its powered state sets it, with updates, after playing
// it if the power rose. Using one raises its note by one (24 wraps to 0),
// sets it with updates and plays it. Playing queues a block event unless the
// block above is not air and the instrument is not a head's; the event plays
// the sound, drawing a long from the level random for it. A player head's
// sound comes from its block entity; without one nothing plays or draws.
#include "internal.hpp"

namespace circuit::gameplay::detail {
namespace {
using registry::Instrument;

Instrument instrument_of(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block ? registry::block(*block).instrument : Instrument::harp;
}

void play(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (registry::head_instrument(static_cast<Instrument>(s.variant())) || ctx.block(pos.relative(Direction::up)).is_air())
        queue_block_event(ctx, {pos, BlockType::note_block, 0, 0});
}
} // namespace

BlockState tuned_note_block(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto above = instrument_of(ctx.block(pos.relative(Direction::up)));
    if (registry::head_instrument(above)) return s.with_variant(static_cast<std::uint8_t>(above));
    const auto below = instrument_of(ctx.block(pos.relative(Direction::down)));
    return s.with_variant(static_cast<std::uint8_t>(registry::head_instrument(below) ? Instrument::harp : below));
}

void check_note_block(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool on = has_neighbor_signal(ctx, pos);
    if (on == s.flag()) return;
    if (on) play(ctx, pos, s);
    set_block(ctx, pos, s.with_flag(on));
}

void use_note_block(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto next = with_note(s, static_cast<std::uint8_t>((note_of(s) + 1) % 25));
    set_block(ctx, pos, next);
    play(ctx, pos, next);
}

void note_block_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    if (d != Direction::up && d != Direction::down) return;
    const auto tuned = tuned_note_block(ctx, pos, s);
    if (tuned != s) set_block_quiet(ctx, pos, tuned);
}

void run_note_event(RuleContext& ctx, BlockState s) {
    // Player heads without a sound of their own (the rules keep no sound for them).
    if (static_cast<Instrument>(s.variant()) == Instrument::custom_head) return;
    ctx.random_draws([](LevelRandom& r) { return r.next_long(); });
}

} // namespace circuit::gameplay::detail
