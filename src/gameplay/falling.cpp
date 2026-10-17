// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/items.hpp>
#include <array>
#include <string>
#include <string_view>
#include <vector>

// Falling blocks, as both official servers run them (checked against their
// traces; minecraft.wiki "Falling Block"). Sand, red sand, gravel and the
// concrete powders check two ticks after they are placed or a neighbor
// changes whether the cell below is free (a block that can be replaced:
// air, water, grass, snow and the others of `block_replaceable`); if it is, the
// block becomes air and a falling block 0.98 wide and tall starts at the
// cell's bottom center. Each tick it gains 0.04 downwards, moves, and loses
// 2% of its speed. When it lands (or, for concrete powder, reaches water)
// it becomes its block again where its bottom center is, if that cell can be
// replaced and the cell below cannot; otherwise it breaks and drops its item.
namespace circuit::gameplay::detail {
namespace {
using contracts::Vec3;
constexpr double gravity = 0.04;
// Entity sizes are single-precision (primed TNT, 0.98 wide, stops at
// 0.5099999904632568 beside a block at x = 1 on both official servers).
constexpr double size = static_cast<double>(0.98f);
constexpr std::uint16_t drop_delay = 10;

enum class Gravity : std::uint8_t { none, falls, concrete_powder };

// Per registry block: whether it falls, and whether it can be replaced.
struct Kinds {
    std::vector<Gravity> gravity;
    std::vector<bool> replaceable;
};
const Kinds& kinds() {
    static const Kinds k = [] {
        // Blocks a falling block (and a placement) replaces, as both
        // official servers report them for every state.
        static constexpr std::array<std::string_view, 29> replaced{
            "air", "cave_air", "void_air", "water", "lava", "bubble_column", "fire", "soul_fire", "light", "structure_void",
            "short_grass", "tall_grass", "fern", "large_fern", "dead_bush", "bush", "short_dry_grass", "tall_dry_grass",
            "seagrass", "tall_seagrass", "vine", "glow_lichen", "resin_clump", "hanging_roots", "crimson_roots", "warped_roots",
            "nether_sprouts", "leaf_litter", "snow"};
        Kinds out;
        const auto count = registry::block_count();
        out.gravity.assign(count, Gravity::none);
        out.replaceable.assign(count, false);
        for (std::uint32_t b = 0; b < count; ++b) {
            auto name = registry::block(b).name;
            if (const auto at = name.find('@'); at != std::string_view::npos) name = name.substr(0, at);
            if (name == "sand" || name == "red_sand" || name == "gravel") out.gravity[b] = Gravity::falls;
            if (name.ends_with("_concrete_powder")) out.gravity[b] = Gravity::concrete_powder;
            for (const auto r : replaced)
                if (name == r) out.replaceable[b] = true;
        }
        return out;
    }();
    return k;
}

Gravity gravity_of(BlockState s) {
    if (!s.is(BlockType::other)) return Gravity::none;
    const auto block = registry::block_of(s.registry_state());
    return block ? kinds().gravity[*block] : Gravity::none;
}

// The block a concrete powder becomes in water.
BlockState concrete_of(RuleContext& ctx, BlockState powder) {
    const auto block = registry::block_of(powder.registry_state());
    auto name = std::string(registry::block(*block).name);
    name.resize(name.size() - std::string_view("_powder").size());
    const auto concrete = registry::find_block(ctx.bundle().schema.content_release, name);
    return concrete ? BlockState::registered(registry::block(*concrete).default_state) : powder;
}

// The item a falling block drops: its block's.
ItemStack item_of(BlockState s) {
    const auto block = registry::block_of(s.registry_state());
    if (!block) return {};
    const auto item = items::by_name(std::string("minecraft:") + std::string(registry::block(*block).name));
    return item ? ItemStack{*item, 1} : ItemStack{};
}

// Drops a falling block's item at its position, launched as every new item
// is (from the level random), collectable after 10 ticks.
void drop(RuleContext& ctx, const Entity& e) {
    const auto stack = item_of(e.carried);
    if (stack.empty()) return;
    const double vx = ctx.random_double() * 0.2 - 0.1;
    const double vz = ctx.random_double() * 0.2 - 0.1;
    spawn_dropped(ctx, contracts::block_of(e.position), e.position, Vec3{vx, 0.2, vz}, stack, drop_delay);
}

} // namespace


bool falls(BlockState s) { return gravity_of(s) != Gravity::none; }

void schedule_fall(RuleContext& ctx, BlockPos pos) { schedule(ctx, TickList::block, pos, BlockType::other, 2, 0); }

void tick_falling_block(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!falls(s) || pos.y < ctx.world().min_y) return;
    // Below the world there is nothing (void air), which a block falls into.
    const auto under = pos.relative(Direction::down);
    if (ctx.world().contains_y(under.y)) {
        const auto below = ctx.block(under);
        if (below.is_unloaded() || !block_replaceable(below)) return;
    }
    // The entity exists before the block goes (its id comes first); the
    // official servers report the block change, then the new entity.
    Entity e;
    e.kind = EntityKind::falling_block;
    e.position = {static_cast<double>(pos.x) + 0.5, static_cast<double>(pos.y), static_cast<double>(pos.z) + 0.5};
    e.carried = s;
    const auto id = ctx.spawn_entity(e);
    set_block(ctx, pos, blocks::air());
    record_spawn(ctx, id, e);
}

void run_falling_block(RuleContext& ctx, EntityId id, Entity& e) {
    ++e.tick_count;
    ++e.fall_time;
    const auto start = e.position;
    auto v = e.velocity;
    auto p = e.position;
    v.y -= gravity;
    e.on_ground = move_entity(ctx, p, v, size, size, &e);
    const auto cell = contracts::block_of(p);
    const auto here = ctx.block(cell);
    const bool in_water = gravity_of(e.carried) == Gravity::concrete_powder && here.is(BlockType::water);
    const auto carried = e.carried;
    const auto fall_time = e.fall_time;
    e.velocity = v;
    if (p != start) {
        const auto from = contracts::chunk_of(contracts::block_of(start));
        const auto to = contracts::chunk_of(cell);
        if (from != to) ctx.record(TraceKind::entity_changed_chunk, cell, id.value, contracts::pack_chunk(from), contracts::pack_chunk(to));
        ctx.relocate_entity(id, p); // `e` must not be used after relocation.
    }
    auto* self = ctx.entity(id);
    const auto remove = [&](RemovalReason reason) {
        ctx.record(TraceKind::entity_removed, cell, id.value, static_cast<std::uint64_t>(reason));
        ctx.remove_entity(id);
    };
    if (!self->on_ground && !in_water) {
        // No other limit below the world: both official servers let a
        // falling block fall on until its time is up there (falling_void).
        const bool outside = cell.y <= ctx.world().min_y || cell.y > ctx.world().min_y + ctx.world().height - 1;
        if (fall_time > 600 || (fall_time > 100 && outside)) {
            const auto copy = *self;
            remove(RemovalReason::despawned);
            drop(ctx, copy);
            return;
        }
        self->velocity = {self->velocity.x * 0.98, self->velocity.y * 0.98, self->velocity.z * 0.98};
        return;
    }
    // Landed: back to its block where its bottom center is, when that cell
    // can be replaced (snow only with a single layer) and the cell below
    // cannot; otherwise it breaks into its item.
    const auto under = cell.relative(Direction::down);
    const auto below = ctx.world().contains_y(under.y) ? ctx.block(under) : blocks::air();
    bool fits = ctx.world().contains_y(cell.y) && block_replaceable(here) && !here.is_unloaded() && !below.is_unloaded()
             && !block_replaceable(below);
    if (fits && here.is(BlockType::other)) {
        const auto block = registry::block_of(here.registry_state());
        if (registry::block(*block).name == "snow")
            for (const auto& [key, value] : registry::properties(here.registry_state()))
                if (key == "layers" && value != "1") fits = false;
    }
    if (fits) {
        // The block, then the entity goes, then the block reacts to its
        // landing: concrete powder in water becomes concrete.
        set_block(ctx, cell, carried);
        remove(RemovalReason::despawned);
        if (in_water) set_block(ctx, cell, concrete_of(ctx, carried));
        return;
    }
    const auto copy = *self;
    remove(RemovalReason::despawned);
    drop(ctx, copy);
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
bool block_replaceable(BlockState s) {
    if (s.is_air() || s.is(BlockType::water)) return true;
    if (!s.is(BlockType::other)) return false;
    const auto block = registry::block_of(s.registry_state());
    return block && detail::kinds().replaceable[*block];
}
} // namespace circuit::gameplay
