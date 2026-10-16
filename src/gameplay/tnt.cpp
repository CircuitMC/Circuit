// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

// TNT, as both official servers run it (checked against their traces;
// minecraft.wiki "TNT"). A TNT block with a redstone signal from any
// neighbor (the test redstone lamps use) when it is placed, moved in by a
// piston or updated becomes primed TNT: the entity first, launched in a
// direction from the level random, then the block goes. With the 1.21.8
// game rule tntExplodes off, redstone leaves TNT alone.
//
// Each tick primed TNT falls by 0.04, moves (0.98 wide and tall), and loses
// 2% of its speed, and 30% more of its horizontal speed on the ground (on
// any block), where it also bounces back up at half its downward speed. Its
// fuse goes down; at zero it explodes and is gone; otherwise currents push
// it once, after the move. Water neither slows it nor holds it up.
namespace circuit::gameplay::detail {
namespace {
using contracts::Vec3;
constexpr double gravity = 0.04;
constexpr double size = static_cast<double>(0.98f);
constexpr std::int32_t redstone_fuse = 80;
constexpr float explosion_power = 4.0f;

const std::vector<bool>& tnt_blocks() {
    static const std::vector<bool> out = [] {
        std::vector<bool> v(registry::block_count(), false);
        for (std::uint32_t b = 0; b < v.size(); ++b) v[b] = registry::block(b).name == "tnt";
        return v;
    }();
    return out;
}

// Whether TNT may be primed: always in 1.21.4, which has no tntExplodes.
bool tnt_explodes(RuleContext& ctx) {
    return ctx.bundle().schema.game_rule("tntExplodes") == nullptr || ctx.game_rule("tntExplodes") != 0;
}

// Primed TNT at a cell's bottom center, launched 0.2 up and 0.02 sideways
// in a direction from the level random.
Entity primed_at(RuleContext& ctx, BlockPos pos) {
    Entity e;
    e.kind = EntityKind::primed_tnt;
    e.position = {static_cast<double>(pos.x) + 0.5, static_cast<double>(pos.y), static_cast<double>(pos.z) + 0.5};
    const double angle = ctx.random_double() * static_cast<double>(static_cast<float>(2.0 * std::numbers::pi));
    e.velocity = {-std::sin(angle) * 0.02, static_cast<double>(0.2f), -std::cos(angle) * 0.02};
    return e;
}

// The TNT block at `pos` becomes primed TNT with a fuse of 80 ticks: the
// entity first, then the block goes.
void prime(RuleContext& ctx, BlockPos pos) {
    auto e = primed_at(ctx, pos);
    e.fuse = redstone_fuse;
    const auto id = ctx.spawn_entity(e);
    record_spawn(ctx, id, e);
    set_block(ctx, pos, blocks::air());
}
} // namespace

std::optional<BlockState> tnt_block(RuleContext& ctx) {
    const auto block = registry::find_block(ctx.bundle().schema.content_release, "tnt");
    if (!block) return std::nullopt;
    return BlockState::registered(registry::block(*block).default_state);
}

bool is_tnt(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto block = registry::block_of(s.registry_state());
    return block && tnt_blocks()[*block];
}

void check_tnt(RuleContext& ctx, BlockPos pos) {
    if (has_neighbor_signal(ctx, pos) && tnt_explodes(ctx)) prime(ctx, pos);
}

void prime_by_explosion(RuleContext& ctx, BlockPos pos) {
    if (!tnt_explodes(ctx)) return;
    auto e = primed_at(ctx, pos);
    e.fuse = ctx.random_int(redstone_fuse / 4) + redstone_fuse / 8;
    record_spawn(ctx, ctx.spawn_entity(e), e);
}

void run_primed_tnt(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    const auto start = e.position;
    auto v = e.velocity;
    auto p = e.position;
    v.y -= gravity;
    const bool landed = move_entity(ctx, p, v, size, size);
    v = {v.x * 0.98, v.y * 0.98, v.z * 0.98};
    if (landed) v = {v.x * 0.7, v.y * -0.5, v.z * 0.7};
    e.on_ground = landed;
    e.velocity = v;
    const auto fuse = --e.fuse;
    const auto cell = contracts::block_of(p);
    if (p != start) {
        const auto from = contracts::chunk_of(contracts::block_of(start));
        const auto to = contracts::chunk_of(cell);
        if (from != to) ctx.record(TraceKind::entity_changed_chunk, cell, id.value, contracts::pack_chunk(from), contracts::pack_chunk(to));
        ctx.relocate_entity(id, p); // `e` must not be used after relocation.
    }
    if (fuse <= 0) {
        // Gone first, then it explodes a sixteenth of its height above its feet.
        ctx.record(TraceKind::entity_removed, cell, id.value, static_cast<std::uint64_t>(RemovalReason::exploded));
        ctx.remove_entity(id);
        explode(ctx, {p.x, p.y + static_cast<double>(0.98f * 0.0625f), p.z}, explosion_power);
        return;
    }
    if (const auto water = water_at(ctx, physics::box_at(p, size, size)); water.inside) {
        auto* self = ctx.entity(id);
        self->velocity = {self->velocity.x + water.push.x, self->velocity.y + water.push.y, self->velocity.z + water.push.z};
    }
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
bool other_movable(BlockState s) noexcept { return detail::is_tnt(s); }
} // namespace circuit::gameplay
