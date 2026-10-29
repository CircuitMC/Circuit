// SPDX-License-Identifier: AGPL-3.0-only
// Dispensers and droppers, as both official servers run them (checked
// against their traces; minecraft.wiki "Dispenser", "Dropper").
//
// A neighbor update that finds power at the block or at the block above it
// (which it does not otherwise watch) schedules a tick 4 ticks later and sets
// it triggered, without neighbor updates; one that finds no power clears
// triggered. Placing one next to power does nothing until an update comes.
// The tick picks a slot at random among the slots holding something (each
// next one replaces the pick when an int draw below the count of slots seen
// so far is 0) and dispenses one item from it; an empty one only clicks.
//
// A dropper puts the item into a container in front, at the first slot
// that takes it, or keeps it when none does; otherwise it ejects it, as a
// dispenser ejects items without a use of their own: from 0.7 out of the
// center toward the front, 0.15625 lower (0.125 facing up or down), after
// the two draws every new item makes, at a speed of 0.2 to 0.3 along the
// facing and 0.2 up, each axis spread by the difference of two draws times
// 0.103365. A dispenser primes TNT at the cell in front (unless tntExplodes
// is off: then it keeps it), empties a water bucket into the cell in front
// and fills a bucket from it, and sets a minecart on a rail in front or
// below the cell in front. Items that equip or saddle entities eject, as
// there are no entities for them; items of other uses stay unless the rules
// know their use fails (see `Use`).
#include "internal.hpp"
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace circuit::gameplay::detail {
namespace {
using contracts::pack_stack;
using contracts::Vec3;
constexpr std::uint32_t dispense_delay = 4;
// Spread of an ejected item's velocity: 0.0172275 per unit of speed, at speed 6.
constexpr double eject_spread = 0.0172275 * 6;

// What a dispenser does with an item. `keep`: a use the rules do not model
// (projectiles, fire charges, shears, golem and wither heads, placing
// shulker boxes, armor stands, mobs, other fluids and other minecarts); the
// item stays. The conditional ones eject unless the cell in front is one
// they act on, which the rules do not model either: boats on water,
// bottles at water or a full hive, honeycomb at copper that is not waxed,
// glowstone at a respawn anchor, a potion with data at dirt.
enum class Use : std::uint8_t { eject, keep, tnt, water, lava, bucket, minecart, boat, bottle, honeycomb, glowstone, potion, bone_meal, flint };

Use classify(std::string_view name) {
    static constexpr std::array<std::string_view, 30> kept{
        "shears", "carved_pumpkin", "brush", "wither_skeleton_skull", "arrow", "spectral_arrow",
        "tipped_arrow", "snowball", "egg", "blue_egg", "brown_egg", "experience_bottle", "splash_potion", "lingering_potion",
        "fire_charge", "firework_rocket", "wind_charge", "armor_stand", "powder_snow_bucket", "cod_bucket",
        "salmon_bucket", "pufferfish_bucket", "tropical_fish_bucket", "axolotl_bucket", "tadpole_bucket", "chest_minecart",
        "furnace_minecart", "tnt_minecart", "hopper_minecart", "command_block_minecart"};
    if (name == "tnt") return Use::tnt;
    if (name == "bone_meal") return Use::bone_meal;
    if (name == "flint_and_steel") return Use::flint;
    if (name == "water_bucket") return Use::water;
    if (name == "lava_bucket") return Use::lava;
    if (name == "bucket") return Use::bucket;
    if (name == "minecart") return Use::minecart;
    if (name == "glass_bottle") return Use::bottle;
    if (name == "honeycomb") return Use::honeycomb;
    if (name == "glowstone") return Use::glowstone;
    if (name == "potion") return Use::potion;
    if (name.ends_with("_boat") || name.ends_with("_raft")) return Use::boat;
    if (name.ends_with("_spawn_egg") || name.ends_with("shulker_box")) return Use::keep;
    for (const auto k : kept)
        if (name == k) return Use::keep;
    return Use::eject;
}

Use use_of(std::uint16_t item) {
    static const std::vector<Use> table = [] {
        std::vector<Use> out(static_cast<std::size_t>(items::max_id) + 1, Use::eject);
        for (std::uint16_t id = 1; id <= items::max_id; ++id) {
            auto name = items::info(id)->name;
            if (name.starts_with("minecraft:")) name.remove_prefix(10);
            out[id] = classify(name);
        }
        return out;
    }();
    return item < table.size() ? table[item] : Use::eject;
}

std::string_view registry_name(BlockState s) {
    if (!s.is(BlockType::other)) return {};
    const auto block = registry::block_of(s.registry_state());
    return block ? std::string_view(registry::block(*block).name) : std::string_view{};
}

std::string registry_property(BlockState s, std::string_view key) {
    if (!s.is(BlockType::other)) return {};
    for (auto& [k, v] : registry::properties(s.registry_state()))
        if (k == key) return std::move(v);
    return {};
}

bool loaded_in_world(RuleContext& ctx, BlockPos pos) { return ctx.world().contains_y(pos.y) && !ctx.block(pos).is_unloaded(); }

bool waxable(BlockState s) {
    std::string_view name;
    if (s.is(BlockType::door) || s.is(BlockType::trapdoor)) name = door_kind_name(s.variant());
    else name = registry_name(s);
    return name.find("copper") != std::string_view::npos && !name.starts_with("waxed_") && name.find("ore") == std::string_view::npos
        && name != "raw_copper_block";
}

// Whether the cell in front is one a conditional use acts on.
bool has_target(RuleContext& ctx, Use use, BlockPos front, const ItemStack& stack) {
    if (!loaded_in_world(ctx, front)) return false;
    const auto s = ctx.block(front);
    switch (use) {
    case Use::boat:
        return holds_water_fluid(s) || (s.is_air() && holds_water_fluid(ctx.block(front.relative(Direction::down))));
    case Use::bottle: {
        const auto name = registry_name(s);
        return holds_water_fluid(s) || ((name == "beehive" || name == "bee_nest") && registry_property(s, "honey_level") == "5");
    }
    case Use::honeycomb: return waxable(s);
    case Use::glowstone: return registry_name(s) == "respawn_anchor";
    case Use::potion: {
        const auto name = registry_name(s);
        return !stack.components.empty() && (name == "dirt" || name == "coarse_dirt" || name == "rooted_dirt");
    }
    default: return false;
    }
}

Vec3 center(BlockPos p) { return {p.x + 0.5, p.y + 0.5, p.z + 0.5}; }

// Ejects one item of `stack` through the front, or the whole stack.
void eject(RuleContext& ctx, BlockPos pos, Direction facing, ItemStack stack, bool whole = false) {
    if (!whole) stack.count = 1;
    const auto o = contracts::offset(facing);
    const auto c = center(pos);
    const Vec3 at{c.x + 0.7 * o.dx, c.y + 0.7 * o.dy, c.z + 0.7 * o.dz};
    const double y = at.y - (o.dy != 0 ? 0.125 : 0.15625);
    (void)ctx.random_double(); // the new item's own velocity, replaced below
    (void)ctx.random_double();
    const double speed = ctx.random_double() * 0.1 + 0.2;
    const auto triangle = [&](double mode) {
        const double a = ctx.random_double();
        const double b = ctx.random_double();
        return mode + eject_spread * (a - b);
    };
    const double vx = triangle(o.dx * speed);
    const double vy = triangle(0.2);
    const double vz = triangle(o.dz * speed);
    spawn_dropped(ctx, contracts::block_of(at), {at.x, y, at.z}, {vx, vy, vz}, stack, 0);
}

void set_slot(RuleContext& ctx, BlockPos pos, std::size_t slot, ItemStack next) {
    auto* be = ctx.block_entity(pos);
    if (!be || slot >= be->slots.size() || be->slots[slot] == next) return;
    if (next.count == 0) next = {};
    const auto before = be->slots[slot];
    be->slots[slot] = next;
    ctx.record(TraceKind::inventory_changed, pos, slot, pack_stack(before), pack_stack(next));
    container_changed(ctx, pos);
}

ItemStack less_one(ItemStack s) {
    s.count = static_cast<std::uint8_t>(s.count - 1);
    return s.count == 0 ? ItemStack{} : s;
}

// A use that leaves `remainder` (an empty bucket, a bucket of water) in
// place of the item it used: in its slot when that was the last item, else
// in the first slot that takes it, else ejected.
void use_with_remainder(RuleContext& ctx, BlockPos pos, Direction facing, std::size_t slot, const ItemStack& used, ItemStack remainder) {
    if (used.count <= 1) {
        set_slot(ctx, pos, slot, remainder);
        return;
    }
    set_slot(ctx, pos, slot, less_one(used));
    const auto* be = ctx.peek_block_entity(pos);
    if (!be) return;
    const auto limit = std::min(contracts::max_stack_size, items::max_stack(remainder));
    for (std::size_t i = 0; i < be->slots.size(); ++i) {
        const auto& s = be->slots[i];
        if (s.empty() || (s.same_kind(remainder) && s.count < limit)) {
            auto next = remainder;
            next.count = static_cast<std::uint8_t>((s.empty() ? 0 : s.count) + 1);
            set_slot(ctx, pos, i, next);
            return;
        }
    }
    eject(ctx, pos, facing, remainder);
}

void set_minecart(RuleContext& ctx, BlockPos pos, Direction facing, std::size_t slot, const ItemStack& stack) {
    const auto front = pos.relative(facing);
    const auto there = ctx.block(front);
    const auto ascending = [](BlockState rail) { return rail.level() >= 2 && rail.level() <= 5; };
    double offset = 0.0;
    if (there.is(BlockType::rail)) {
        offset = ascending(there) ? 0.6 : 0.1;
    } else if (there.is_air() && ctx.block(front.relative(Direction::down)).is(BlockType::rail)) {
        offset = facing != Direction::down && ascending(ctx.block(front.relative(Direction::down))) ? -0.4 : -0.9;
    } else {
        eject(ctx, pos, facing, stack);
        set_slot(ctx, pos, slot, less_one(stack));
        return;
    }
    const auto o = contracts::offset(facing);
    const auto c = center(pos);
    Entity e;
    e.kind = EntityKind::minecart;
    e.position = {c.x + o.dx * 1.125, std::floor(c.y) + o.dy + offset, c.z + o.dz * 1.125};
    record_spawn(ctx, ctx.spawn_entity(e), e);
    set_slot(ctx, pos, slot, less_one(stack));
}

// A dropper's item into the container in front, or out through the front.
void drop_from(RuleContext& ctx, BlockPos pos, Direction facing, std::size_t slot, const ItemStack& stack) {
    const auto front = pos.relative(facing);
    if (is_container(ctx.block(front).type())) {
        if (insert_one(ctx, front, stack, contracts::opposite(facing))) set_slot(ctx, pos, slot, less_one(stack));
        return;
    }
    // A composter takes the item through its top or keeps the dropper from
    // dropping it (composter_inputs).
    if (ctx.block(front).is(BlockType::composter)) {
        if (compost_through(ctx, front, contracts::opposite(facing), stack)) set_slot(ctx, pos, slot, less_one(stack));
        return;
    }
    eject(ctx, pos, facing, stack);
    set_slot(ctx, pos, slot, less_one(stack));
}

void dispense_from(RuleContext& ctx, BlockPos pos, Direction facing, std::size_t slot, const ItemStack& stack) {
    const auto front = pos.relative(facing);
    const auto use = use_of(stack.item);
    switch (use) {
    case Use::keep: return;
    case Use::tnt:
        if (prime_dispensed(ctx, front)) set_slot(ctx, pos, slot, less_one(stack));
        return;
    case Use::water: case Use::lava:
        if (!pour_fluid(ctx, front, use == Use::lava)) break;
        use_with_remainder(ctx, pos, facing, slot, stack, ItemStack{items::bucket, 1});
        return;
    case Use::bucket:
        if (const auto filled = take_fluid(ctx, front)) {
            use_with_remainder(ctx, pos, facing, slot, stack, ItemStack{*filled, 1});
            return;
        }
        break;
    case Use::minecart: set_minecart(ctx, pos, facing, slot, stack); return;
    // Bone meal acts on the block in front, or stays (bonemeal_dispensers).
    case Use::bone_meal:
        if (loaded_in_world(ctx, front) && apply_bone_meal(ctx, front)) set_slot(ctx, pos, slot, less_one(stack));
        return;
    // Flint and steel sets fire, lights or primes in front, wearing by one
    // (and gone at its durability), or stays (dispenser_flint).
    case Use::flint:
        if (loaded_in_world(ctx, front) && dispense_fire(ctx, front)) {
            auto worn = stack;
            if (const auto* info = items::info(worn.item); info && ++worn.damage >= info->max_damage) worn = {};
            set_slot(ctx, pos, slot, worn);
        }
        return;
    case Use::boat: case Use::bottle: case Use::honeycomb: case Use::glowstone: case Use::potion:
        if (has_target(ctx, use, front, stack)) return;
        break;
    case Use::eject: break;
    }
    eject(ctx, pos, facing, stack);
    set_slot(ctx, pos, slot, less_one(stack));
}
} // namespace

void eject_from(RuleContext& ctx, BlockPos pos, Direction facing, ItemStack stack, bool whole) { eject(ctx, pos, facing, stack, whole); }

void check_dispenser(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::dispenser)) return;
    const bool powered = has_neighbor_signal(ctx, pos) || has_neighbor_signal(ctx, pos.relative(Direction::up));
    if (powered && !s.flag()) {
        schedule(ctx, TickList::block, pos, BlockType::dispenser, dispense_delay, 0);
        set_block_quiet(ctx, pos, s.with_flag(true));
    } else if (!powered && s.flag()) {
        set_block_quiet(ctx, pos, s.with_flag(false));
    }
}

void tick_dispenser(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto* be = ctx.peek_block_entity(pos);
    if (!be) return;
    std::optional<std::size_t> chosen;
    std::int32_t seen = 1;
    for (std::size_t i = 0; i < be->slots.size(); ++i)
        if (!be->slots[i].empty() && ctx.random_int(seen++) == 0) chosen = i;
    if (!chosen) return; // an empty one only clicks
    const auto stack = be->slots[*chosen];
    if (s.variant() == 1) drop_from(ctx, pos, s.facing(), *chosen, stack);
    else dispense_from(ctx, pos, s.facing(), *chosen, stack);
}

} // namespace circuit::gameplay::detail
