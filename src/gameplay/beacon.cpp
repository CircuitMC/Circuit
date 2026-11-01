// SPDX-License-Identifier: AGPL-3.0-only
// Beacons, as both official servers run them (circuit-clienttest/beacon.js
// and beacon-dbg.js, both versions identical; minecraft.wiki "Beacon").
//
// A beacon's block entity runs a cycle on every tick whose game time is a
// multiple of 80. It looks up its beam: open to the top of the world unless
// a block that stops light fully (bedrock excepted) stands over it. With the
// beam open it counts the levels of its pyramid: layer k is the (2k + 1)²
// cells k below it, all beacon base blocks, up to four layers; a blocked beam
// keeps the levels it had. With levels and the beam open it gives its powers
// to the players in range (the server reads beacon_effects) and plays its
// ambient sound; rising from no levels it then plays the activate sound,
// falling to none the deactivate sound. A beacon that goes plays the
// deactivate sound at once, also without levels (client/beacon2).
//
// Its window chooses the powers: a payment in its slot (an item of the
// beacon_payment_items tag; thrown out when the window closes) is used up, and the powers chosen are kept
// whatever the levels (the levels decide what is given: the primary power
// from one level; at four the secondary, or the primary at amplifier 1 when
// both are the same); a choice plays the power_select sound while the beam
// is open.
#include "internal.hpp"
#include <circuit/gameplay/effects.hpp>
#include <circuit/gameplay/light.hpp>
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
using contracts::SoundEvent;

// The beacon_base_blocks tag of the official data packs, or the same blocks
// from minecraft.wiki without them; internal registry blocks, ascending.
std::vector<std::uint32_t> base_blocks(RuleContext& ctx) {
    if (const auto* data = ctx.data(); data && !data->beacon_base_blocks.empty()) return data->beacon_base_blocks;
    std::vector<std::uint32_t> out;
    for (const auto* name : {"minecraft:iron_block", "minecraft:gold_block", "minecraft:emerald_block", "minecraft:diamond_block",
                             "minecraft:netherite_block"})
        if (const auto b = registry::find_block(ctx.bundle().schema.content_release, name)) out.push_back(*b);
    std::ranges::sort(out);
    return out;
}

bool block_in(const std::vector<std::uint32_t>& blocks, BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block && std::ranges::binary_search(blocks, *block);
}

// The beam goes up to the top of the world past every block but those that
// stop light fully; bedrock lets it through.
bool beam_open(RuleContext& ctx, BlockPos pos) {
    const auto& world = ctx.world();
    const auto bedrock = registry::find_block(ctx.bundle().schema.content_release, "minecraft:bedrock");
    for (std::int32_t y = pos.y + 1; y < world.min_y + world.height; ++y) {
        const auto s = ctx.block({pos.x, y, pos.z});
        if (s.is_unloaded() || light_info(s).dampening < 15) continue;
        const auto state = physics::registry_state_of(s);
        if (bedrock && state && registry::block_of(*state) == bedrock) continue;
        return false;
    }
    return true;
}

// Complete layers of beacon base blocks under the beacon, up to four.
std::uint8_t pyramid_levels(RuleContext& ctx, BlockPos pos) {
    const auto bases = base_blocks(ctx);
    std::uint8_t levels = 0;
    for (std::int32_t k = 1; k <= 4; ++k) {
        const auto y = pos.y - k;
        if (y < ctx.world().min_y) break;
        for (std::int32_t x = pos.x - k; x <= pos.x + k; ++x)
            for (std::int32_t z = pos.z - k; z <= pos.z + k; ++z)
                if (!block_in(bases, ctx.block({x, y, z}))) return levels;
        levels = static_cast<std::uint8_t>(k);
    }
    return levels;
}

void play(RuleContext& ctx, BlockPos pos, SoundEvent sound) { ctx.record(TraceKind::sound_played, pos, static_cast<std::uint64_t>(sound)); }
} // namespace

std::uint8_t beacon_power(std::int32_t effect) {
    if (effect < 0 || effect > 255 || !effects::is_beacon_power(static_cast<std::uint8_t>(effect))) return 0;
    return static_cast<std::uint8_t>(effect + 1);
}

bool is_beacon_payment(RuleContext& ctx, std::uint16_t item) {
    if (const auto* data = ctx.data(); data && !data->beacon_payment_items.empty())
        return std::ranges::binary_search(data->beacon_payment_items, item);
    for (const auto* name : {"minecraft:iron_ingot", "minecraft:gold_ingot", "minecraft:emerald", "minecraft:diamond", "minecraft:netherite_ingot"})
        if (items::by_name(name) == item) return true;
    return false;
}

void tick_beacon(RuleContext& ctx, BlockPos pos) {
    if (ctx.now() % 80 != 0) return;
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    const auto before = be->levels;
    be->beam = beam_open(ctx, pos);
    if (be->beam) be->levels = pyramid_levels(ctx, pos);
    if (be->levels > 0 && be->beam) {
        if (be->primary != 0) ctx.record(TraceKind::beacon_effects, pos, be->levels, be->primary, be->secondary);
        play(ctx, pos, SoundEvent::beacon_ambient);
    }
    if (before == 0 && be->levels > 0) play(ctx, pos, SoundEvent::beacon_activate);
    else if (before > 0 && be->levels == 0) play(ctx, pos, SoundEvent::beacon_deactivate);
}

void beacon_removed(RuleContext& ctx, BlockPos pos) { play(ctx, pos, SoundEvent::beacon_deactivate); }

void choose_beacon_powers(RuleContext& ctx, BlockPos pos, std::uint8_t primary, std::uint8_t secondary) {
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    if (be->beam) play(ctx, pos, SoundEvent::beacon_power_select);
    be->primary = primary;
    be->secondary = secondary;
}

} // namespace circuit::gameplay::detail
