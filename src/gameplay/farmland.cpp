// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"

// Cacti hurt the items whose boxes reach into their cells (not only their
// shapes), a point of health for each cactus at every move (cactus_damage);
// other entities are not hurt.
// Farmland and what falls on it (both official servers; farmland):
// - an entity counts how far it has fallen while it moves down off the
//   ground, and when a move lands it, the block under its feet (0.2 below
//   them) takes the fall: farmland draws a float from the level random (its
//   trampling check, which only living entities pass), and the count starts
//   again; water resets it;
// - farmland turns to dirt in a block tick after an update from above finds
//   a solid block there (not a fence gate or a moving piston).
// Not modeled: moisture and drying (random ticks), trampling, entities on
// farmland pushed up when it turns.
namespace circuit::gameplay::detail {

bool is_farmland(BlockState s) {
    const auto block = s.is(BlockType::other) ? registry::block_of(s.registry_state()) : std::nullopt;
    return block && registry::game_name(*block) == "farmland";
}

void entity_fell(RuleContext& ctx, Entity& e, Vec3 position, Vec3 moved, bool on_ground) {
    if (!on_ground) {
        if (moved.y < 0.0) e.fall_distance -= moved.y;
        return;
    }
    if (e.fall_distance > 0.0) {
        // The block under its feet: the column of the block it stands on (a
        // fence, wall or gate itself), 0.2 below its feet.
        auto at = e;
        at.position = position;
        auto under = contracts::block_of(Vec3{position.x, position.y - 0.2, position.z});
        if (const auto support = supporting_block(ctx, entity_box(at), position, moved)) {
            const auto s = ctx.block(*support);
            const auto connector = s.is(BlockType::other) ? registry::connector(s.registry_state()) : registry::Connector::none;
            const bool tall = s.is(BlockType::fence_gate) || (connector != registry::Connector::none && connector != registry::Connector::pane);
            under = tall ? *support : BlockPos{support->x, under.y, support->z};
        }
        if (ctx.world().contains_y(under.y) && is_farmland(ctx.block(under))) (void)ctx.random_float(); // the trampling check
    }
    e.fall_distance = 0.0;
}

bool cactus_hurts(RuleContext& ctx, EntityId id) {
    auto* e = ctx.entity(id);
    if (!e || e->kind != EntityKind::item) return false;
    const auto box = entity_box(*e).deflate(1.0e-5);
    const auto cell = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    int hits = 0;
    for (auto x = cell(box.min_x); x <= cell(box.max_x); ++x)
        for (auto y = cell(box.min_y); y <= cell(box.max_y); ++y)
            for (auto z = cell(box.min_z); z <= cell(box.max_z); ++z) {
                if (!ctx.world().contains_y(y)) continue;
                const auto s = ctx.block({x, y, z});
                const auto block = s.is(BlockType::other) ? registry::block_of(s.registry_state()) : std::nullopt;
                if (block && registry::game_name(*block) == "cactus") ++hits;
            }
    if (hits == 0) return false;
    e->health -= hits;
    if (e->health > 0) return false;
    ctx.record(TraceKind::entity_removed, contracts::block_of(e->position), id.value, static_cast<std::uint64_t>(RemovalReason::hurt));
    ctx.remove_entity(id);
    return true;
}

namespace {
bool keeps_farmland(BlockState above) {
    if (above.is(BlockType::moving_piston) || above.is(BlockType::fence_gate)) return true;
    const auto state = physics::registry_state_of(above);
    return !state || !registry::solid(*state);
}
} // namespace

void farmland_shape_update(RuleContext& ctx, BlockPos pos, Direction d) {
    if (d == Direction::up && !keeps_farmland(ctx.block(pos.relative(d)))) schedule(ctx, TickList::block, pos, BlockType::other, 1, 0);
}

void tick_farmland(RuleContext& ctx, BlockPos pos) {
    if (keeps_farmland(ctx.block(pos.relative(Direction::up)))) return;
    const auto dirt = registry::find_block(ctx.bundle().schema.content_release, "dirt");
    set_block(ctx, pos, BlockState::registered(registry::block(*dirt).default_state));
}

} // namespace circuit::gameplay::detail
