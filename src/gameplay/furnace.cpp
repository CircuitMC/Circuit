// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Furnaces, smokers and blast furnaces (both official servers; furnaces;
// minecraft.wiki "Furnace", "Smelting"). The block entity holds the input
// (slot 0), the fuel (1) and the result (2), the ticks its fuel still burns
// and that fuel's burn time, and the input's cooking progress and the time it
// needs. In its block-entity tick:
// - it burns a tick of fuel when lit;
// - lit, or with fuel and input: when not lit and the input can be cooked
//   (a recipe of the furnace's type takes it and its result fits), it lights
//   by a fuel item for its burn time (half of it in smokers and blast
//   furnaces), which it uses up (a lava bucket leaves its bucket); when
//   lit and the input can be cooked, the progress grows, and at the cooking
//   time the input becomes its result and the progress starts over; otherwise
//   the progress goes back to 0;
// - neither lit nor with fuel and input, the progress falls by 2;
// - it shows whether it is lit, with full updates, and its comparators read
//   it again when anything changed.
// A different item set into the input slot starts the progress over and takes
// that item's cooking time (200 ticks without a recipe).
namespace circuit::gameplay::detail {
namespace {
using contracts::pack_stack;

std::string_view bare(std::uint16_t item) {
    const auto* info = items::info(item);
    if (!info) return {};
    auto n = info->name;
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    return n;
}

// Burn times (both official servers' reports for every item,
// tests/fixtures/collision/fuel-*.txt).
std::int32_t burn_rule(std::string_view n) {
    static constexpr std::array<std::string_view, 10> woods{"oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "cherry",
                                                           "pale_oak", "bamboo"};
    const auto of_wood = [&](std::string_view suffix) {
        auto b = n;
        if (b.starts_with("stripped_")) b.remove_prefix(9);
        for (const auto w : woods)
            if (b.size() == w.size() + suffix.size() && b.starts_with(w) && b.ends_with(suffix)) return true;
        return false;
    };
    if (n == "lava_bucket") return 20000;
    if (n == "coal_block") return 16000;
    if (n == "dried_kelp_block") return 4001;
    if (n == "blaze_rod") return 2400;
    if (n == "coal" || n == "charcoal") return 1600;
    if (of_wood("_boat") || of_wood("_chest_boat") || n == "bamboo_raft" || n == "bamboo_chest_raft") return 1200;
    if (of_wood("_hanging_sign")) return 800;
    for (const auto s : {"_fence", "_fence_gate", "_log", "_planks", "_pressure_plate", "_stairs", "_trapdoor", "_wood"})
        if (of_wood(s) && n != "bamboo_log" && n != "bamboo_wood") return 300;
    static constexpr std::array<std::string_view, 25> three_hundred{
        "bamboo_block", "stripped_bamboo_block", "bamboo_mosaic", "bamboo_mosaic_stairs", "barrel", "bookshelf", "bow",
        "cartography_table", "chest", "chiseled_bookshelf", "composter", "crafting_table", "crossbow", "daylight_detector",
        "fishing_rod", "fletching_table", "jukebox", "ladder", "lectern", "loom", "mangrove_roots", "note_block", "smithing_table",
        "trapped_chest", "bamboo_mosaic_slab"};
    if (n == "bamboo_mosaic_slab") return 150;
    if (std::ranges::find(three_hundred, n) != three_hundred.end()) return 300;
    if (n.ends_with("_banner") && !n.ends_with("_wall_banner")) return 300;
    if (of_wood("_door") || of_wood("_sign") || n.starts_with("wooden_")) return 200;
    if (of_wood("_slab")) return 150;
    if (n.ends_with("_wool") || of_wood("_button") || (of_wood("_sapling") && n != "bamboo_sapling") || n == "mangrove_propagule"
        || n == "azalea" || n == "flowering_azalea" || n == "bowl" || n == "dead_bush" || n == "leaf_litter" || n == "short_dry_grass"
        || n == "tall_dry_grass" || n == "stick")
        return 100;
    if (n.ends_with("_carpet") && n != "moss_carpet" && n != "pale_moss_carpet") return 67;
    if (n == "bamboo" || n == "scaffolding") return 50;
    return 0;
}

constexpr std::array<recipes::Cooking::Kind, 3> cooking_kinds{recipes::Cooking::Kind::smelting, recipes::Cooking::Kind::smoking,
                                                               recipes::Cooking::Kind::blasting};

const recipes::Cooking* recipe_for(RuleContext& ctx, BlockState furnace, const ItemStack& input) {
    if (input.empty() || !ctx.data()) return nullptr;
    const auto kind = cooking_kinds[furnace.variant() % 3];
    for (const auto& r : ctx.data()->cooking)
        if (r.kind == kind && r.ingredient.test(input.item)) return &r;
    return nullptr;
}

std::uint8_t limit(const ItemStack& s) { return std::min(contracts::max_stack_size, items::max_stack(s)); }

bool can_cook(const recipes::Cooking* r, const BlockEntity& be) {
    if (!r || be.slots[0].empty()) return false;
    const auto& out = be.slots[2];
    if (out.empty()) return true;
    if (!out.same_kind(r->result)) return false;
    return out.count + r->result.count <= limit(out);
}

void set_slot(RuleContext& ctx, BlockPos pos, BlockEntity& be, std::size_t slot, ItemStack value) {
    const auto before = be.slots[slot];
    be.slots[slot] = std::move(value);
    ctx.record(TraceKind::inventory_changed, pos, slot, pack_stack(before), pack_stack(be.slots[slot]));
}
} // namespace

std::int32_t burn_time(std::uint16_t item) {
    static const std::vector<std::int32_t> table = [] {
        std::vector<std::int32_t> v(static_cast<std::size_t>(items::max_id) + 1, 0);
        for (std::uint16_t id = 1; id <= items::max_id; ++id) v[id] = burn_rule(bare(id));
        return v;
    }();
    return item < table.size() ? table[item] : 0;
}

std::int32_t cooking_time(RuleContext& ctx, BlockState furnace, const ItemStack& input, bool recipe_only) {
    const auto* r = recipe_for(ctx, furnace, input);
    return r ? r->time : recipe_only ? 0 : 200;
}

void furnace_input_set(RuleContext& ctx, BlockPos pos, const ItemStack& before) {
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    const auto& now = be->slots[0];
    if (!now.empty() && now.same_kind(before)) return;
    be->cook_total = cooking_time(ctx, ctx.block(pos), now);
    be->cook_time = 0;
}

void tick_furnace(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto* be = ctx.block_entity(pos);
    if (!be || be->slots.size() < 3) return;
    const bool was_lit = be->lit_time > 0;
    bool changed = false;
    if (was_lit) --be->lit_time;
    const bool has_input = !be->slots[0].empty(), has_fuel = !be->slots[1].empty();
    if (be->lit_time > 0 || (has_fuel && has_input)) {
        const auto* r = recipe_for(ctx, s, be->slots[0]);
        if (be->lit_time <= 0 && can_cook(r, *be)) {
            // Smokers and blast furnaces burn their fuel twice as fast.
            const auto burn = burn_time(be->slots[1].item);
            be->lit_time = s.variant() == static_cast<std::uint8_t>(blocks::FurnaceKind::furnace) ? burn : burn / 2;
            be->lit_total = be->lit_time;
            if (be->lit_time > 0) {
                changed = true;
                auto fuel = be->slots[1];
                const auto item = fuel.item;
                fuel.count = static_cast<std::uint8_t>(fuel.count - 1);
                if (fuel.count == 0) fuel = item == items::lava_bucket ? ItemStack{items::bucket, 1} : ItemStack{};
                set_slot(ctx, pos, *be, 1, fuel);
            }
        }
        if (be->lit_time > 0 && can_cook(r, *be)) {
            if (++be->cook_time == be->cook_total) {
                be->cook_time = 0;
                be->cook_total = cooking_time(ctx, s, be->slots[0]);
                auto out = be->slots[2];
                if (out.empty()) out = r->result;
                else out.count = static_cast<std::uint8_t>(out.count + r->result.count);
                // A wet sponge dried with an empty bucket as fuel fills it.
                const bool sponge = bare(be->slots[0].item) == "wet_sponge";
                auto in = be->slots[0];
                in.count = static_cast<std::uint8_t>(in.count - 1);
                if (in.count == 0) in = {};
                set_slot(ctx, pos, *be, 2, out);
                set_slot(ctx, pos, *be, 0, in);
                // It keeps the recipe for the experience a player takes.
                auto used = std::ranges::lower_bound(be->recipes_used, r->id, {}, &std::pair<std::string, std::int32_t>::first);
                if (used == be->recipes_used.end() || used->first != r->id) used = be->recipes_used.insert(used, {r->id, 0});
                if (used->second < INT32_MAX) ++used->second;
                if (sponge && !be->slots[1].empty() && be->slots[1].item == items::bucket)
                    set_slot(ctx, pos, *be, 1, ItemStack{items::water_bucket, 1});
                changed = true;
            }
        } else {
            be->cook_time = 0;
        }
    } else if (be->lit_time <= 0 && be->cook_time > 0) {
        be->cook_time = std::clamp(be->cook_time - 2, 0, be->cook_total);
    }
    const bool lit = be->lit_time > 0;
    if (was_lit != lit) {
        changed = true;
        set_block(ctx, pos, s.with_flag(lit));
    }
    if (changed) container_changed(ctx, pos);
}

void award_cooked(RuleContext& ctx, const std::vector<std::pair<std::string, std::int32_t>>& recipes, contracts::Vec3 at) {
    // Per recipe its count times its experience, in single precision; the
    // official servers give the rest below 1 with that chance by an unseeded
    // random, Circuit by a random of its own from the tick and the place.
    const auto* data = ctx.data();
    if (!data) return;
    for (const auto& [id, count] : recipes) {
        const auto it = std::ranges::find(data->cooking, id, &recipes::Cooking::id);
        if (it == data->cooking.end()) continue;
        const float amount = static_cast<float>(count) * it->experience;
        auto whole = static_cast<std::int32_t>(std::floor(amount));
        const float rest = amount - static_cast<float>(whole);
        if (rest > 0.0f) {
            auto chance = Rng::derive({ctx.now(), std::bit_cast<std::uint64_t>(at.x), std::bit_cast<std::uint64_t>(at.y),
                                       std::bit_cast<std::uint64_t>(at.z), std::hash<std::string>{}(id)});
            if (static_cast<float>(chance.bounded(1u << 24)) / static_cast<float>(1u << 24) < rest) ++whole;
        }
        if (whole > 0) award_experience(ctx, at, whole);
    }
}

void furnace_result_taken(RuleContext& ctx, BlockPos pos, contracts::PlayerKey player) {
    auto* be = ctx.block_entity(pos);
    if (!be || be->recipes_used.empty()) return;
    const auto recipes = std::move(be->recipes_used);
    be->recipes_used.clear();
    // At the player's feet.
    const auto presence = ctx.presence_of(player);
    award_cooked(ctx, recipes, presence ? presence->position : contracts::Vec3{pos.x + 0.5, pos.y + 0.5, pos.z + 0.5});
}

// Which slots a face of a furnace reaches, in order: the top its input, the
// bottom its result and then its fuel, the sides its fuel.
std::vector<std::size_t> furnace_slots(Direction face) {
    if (face == Direction::up) return {0};
    if (face == Direction::down) return {2, 1};
    return {1};
}

bool furnace_accepts(const BlockEntity& be, std::size_t slot, const ItemStack& stack) {
    if (slot == 2) return false;
    if (slot == 1) return burn_time(stack.item) > 0 || (stack.item == items::bucket && be.slots[1].item != items::bucket);
    return true;
}

bool furnace_gives(std::size_t slot, const ItemStack& stack, Direction face) {
    if (face == Direction::down && slot == 1) return stack.item == items::bucket || stack.item == items::water_bucket;
    return true;
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
std::int32_t fuel_burn_time(std::uint16_t item) { return detail::burn_time(item); }
} // namespace circuit::gameplay
