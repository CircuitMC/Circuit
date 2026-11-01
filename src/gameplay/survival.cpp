// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/survival.hpp>

namespace circuit::gameplay {
namespace {
enum class Tool : std::uint8_t { none, pickaxe, axe, shovel, hoe, sword };
struct ToolInfo {
    Tool kind{Tool::none};
    float speed{1.0f};
    int tier{0}; // wood 1, gold 1, stone 2, iron 3, diamond 4, netherite 5
};

ToolInfo tool_of(std::uint16_t item) noexcept {
    switch (item) {
    case items::wooden_pickaxe: return {Tool::pickaxe, 2.0f, 1};
    case items::stone_pickaxe: return {Tool::pickaxe, 4.0f, 2};
    case items::iron_pickaxe: return {Tool::pickaxe, 6.0f, 3};
    case items::golden_pickaxe: return {Tool::pickaxe, 12.0f, 1};
    case items::diamond_pickaxe: return {Tool::pickaxe, 8.0f, 4};
    case items::netherite_pickaxe: return {Tool::pickaxe, 9.0f, 5};
    case items::wooden_axe: return {Tool::axe, 2.0f, 1};
    case items::stone_axe: return {Tool::axe, 4.0f, 2};
    case items::iron_axe: return {Tool::axe, 6.0f, 3};
    case items::golden_axe: return {Tool::axe, 12.0f, 1};
    case items::diamond_axe: return {Tool::axe, 8.0f, 4};
    case items::netherite_axe: return {Tool::axe, 9.0f, 5};
    case items::wooden_shovel: return {Tool::shovel, 2.0f, 1};
    case items::stone_shovel: return {Tool::shovel, 4.0f, 2};
    case items::iron_shovel: return {Tool::shovel, 6.0f, 3};
    case items::golden_shovel: return {Tool::shovel, 12.0f, 1};
    case items::diamond_shovel: return {Tool::shovel, 8.0f, 4};
    case items::netherite_shovel: return {Tool::shovel, 9.0f, 5};
    case items::wooden_hoe: return {Tool::hoe, 2.0f, 1};
    case items::stone_hoe: return {Tool::hoe, 4.0f, 2};
    case items::iron_hoe: return {Tool::hoe, 6.0f, 3};
    case items::golden_hoe: return {Tool::hoe, 12.0f, 1};
    case items::diamond_hoe: return {Tool::hoe, 8.0f, 4};
    case items::netherite_hoe: return {Tool::hoe, 9.0f, 5};
    default: return {};
    }
}

std::optional<registry::BlockInfo> other_info(BlockState s) noexcept {
    const auto b = registry::block_of(s.registry_state());
    return b ? std::optional(registry::block(*b)) : std::nullopt;
}

// Shears by their tool component (both official servers' item reports, the
// same on both): cobwebs 15, and harvested; leaves 15; wool 5; vines and
// glow lichen 2; other blocks 1.
struct ShearsRule {
    float speed{1.0f};
    bool harvests{};
};
ShearsRule shears_rule(BlockState s) noexcept {
    const auto info = s.is(BlockType::other) ? other_info(s) : std::nullopt;
    if (!info) return {};
    const auto n = info->name;
    if (n == "cobweb") return {15.0f, true};
    if (n.ends_with("_leaves")) return {15.0f};
    if (n.ends_with("_wool")) return {5.0f};
    if (n == "vine" || n == "glow_lichen") return {2.0f};
    return {};
}

// Data-pack tool and tier of a block without rules of its own, if any.
const BlockTool* data_tool(BlockState s, const GameData* data) noexcept {
    if (!data || !s.is(BlockType::other)) return nullptr;
    const auto b = registry::block_of(s.registry_state());
    if (!b) return nullptr;
    const auto it = data->block_tools.find(*b);
    return it == data->block_tools.end() ? nullptr : &it->second;
}

// The tool kind that is effective on a block (its mineable tag).
Tool effective_tool(BlockState s, const GameData* data) noexcept {
    switch (s.type()) {
    case BlockType::other: {
        const auto info = other_info(s);
        const auto* d = data_tool(s, data);
        switch (d ? d->tool : info ? info->tool : registry::Tool::none) {
        case registry::Tool::pickaxe: return Tool::pickaxe;
        case registry::Tool::axe: return Tool::axe;
        case registry::Tool::shovel: return Tool::shovel;
        case registry::Tool::hoe: return Tool::hoe;
        default: return Tool::none;
        }
    }
    case BlockType::stone: case BlockType::obsidian: case BlockType::redstone_block: case BlockType::observer:
    case BlockType::piston: case BlockType::sticky_piston: case BlockType::piston_head: case BlockType::hopper:
        return Tool::pickaxe;
    case BlockType::chest: return Tool::axe;
    case BlockType::button: return wooden_button(s) ? Tool::axe : Tool::pickaxe;
    case BlockType::pressure_plate:
        return s.variant() < static_cast<std::uint8_t>(blocks::ButtonKind::stone) ? Tool::axe : Tool::pickaxe;
    case BlockType::weighted_plate: return Tool::pickaxe;
    case BlockType::target: return Tool::hoe;
    case BlockType::door: case BlockType::trapdoor: case BlockType::fence_gate: case BlockType::daylight_detector:
    case BlockType::note_block: case BlockType::rail: case BlockType::dispenser: case BlockType::furnace: case BlockType::crafter:
    case BlockType::brewing_stand: case BlockType::lectern: case BlockType::barrel: case BlockType::composter:
    case BlockType::ender_chest: case BlockType::shulker_box: case BlockType::chiseled_bookshelf: case BlockType::beacon: {
        const auto info = detail::modeled_info(s);
        switch (info ? info->tool : registry::Tool::none) {
        case registry::Tool::pickaxe: return Tool::pickaxe;
        case registry::Tool::axe: return Tool::axe;
        default: return Tool::none;
        }
    }
    default: return Tool::none;
    }
}

// Lowest tier a block needs for its drops; 0 when it drops with any tool.
int required_tier(BlockState s, const GameData* data) noexcept {
    switch (s.type()) {
    case BlockType::other: {
        if (const auto* d = data_tool(s, data)) return d->tier;
        const auto info = other_info(s);
        return info ? info->tier : 0;
    }
    case BlockType::stone: case BlockType::redstone_block: case BlockType::observer: case BlockType::hopper: return 1;
    case BlockType::obsidian: return 4;
    case BlockType::door: case BlockType::trapdoor: case BlockType::fence_gate: case BlockType::dispenser: case BlockType::furnace:
    case BlockType::crafter: case BlockType::brewing_stand: case BlockType::lectern: case BlockType::barrel: case BlockType::composter:
    case BlockType::ender_chest: case BlockType::shulker_box: case BlockType::chiseled_bookshelf: case BlockType::beacon: {
        const auto info = detail::modeled_info(s);
        return info ? info->tier : 0;
    }
    default: return 0;
    }
}
} // namespace

float hardness(BlockState s) noexcept {
    switch (s.type()) {
    case BlockType::other: {
        const auto info = other_info(s);
        return info ? info->hardness : -1.0f;
    }
    case BlockType::stone: case BlockType::piston: case BlockType::sticky_piston: case BlockType::piston_head: return 1.5f;
    case BlockType::glass: case BlockType::redstone_lamp: return 0.3f;
    case BlockType::obsidian: return 50.0f;
    case BlockType::redstone_block: return 5.0f;
    case BlockType::lever: case BlockType::button: case BlockType::pressure_plate: case BlockType::weighted_plate:
    case BlockType::target:
        return 0.5f;
    case BlockType::door: case BlockType::trapdoor: case BlockType::fence_gate: case BlockType::daylight_detector:
    case BlockType::note_block: case BlockType::rail: case BlockType::coral_fan: case BlockType::coral_wall_fan:
    case BlockType::dispenser: case BlockType::furnace: case BlockType::crafter: case BlockType::brewing_stand: case BlockType::lectern:
    case BlockType::barrel: case BlockType::composter: case BlockType::ender_chest: case BlockType::shulker_box:
    case BlockType::chiseled_bookshelf: case BlockType::beacon: {
        const auto info = detail::modeled_info(s);
        return info ? info->hardness : -1.0f;
    }
    case BlockType::observer: case BlockType::hopper: return 3.0f;
    case BlockType::chest: return 2.5f;
    case BlockType::slime_block: case BlockType::honey_block: case BlockType::redstone_wire: case BlockType::repeater:
    case BlockType::comparator:
    case BlockType::wheat: case BlockType::redstone_torch: case BlockType::redstone_wall_torch:
        return 0.0f;
    default: return -1.0f; // air, water, moving pistons
    }
}

bool harvests(BlockState s, const contracts::ItemStack& tool, const GameData* data) noexcept {
    const auto need = required_tier(s, data);
    if (need == 0) return true;
    if (!tool.empty() && tool.item == items::shears && shears_rule(s).harvests) return true;
    const auto t = tool.empty() ? ToolInfo{} : tool_of(tool.item);
    return t.kind == effective_tool(s, data) && t.tier >= need;
}

float destroy_progress(BlockState s, const contracts::ItemStack& tool, bool on_ground, bool eyes_in_water,
                       const GameData* data, DigEffects effects) noexcept {
    const float h = hardness(s);
    if (h < 0.0f) return 0.0f;
    if (h == 0.0f) return 1.0f;
    const auto t = tool.empty() ? ToolInfo{} : tool_of(tool.item);
    float speed = t.kind != Tool::none && t.kind == effective_tool(s, data) ? t.speed : 1.0f;
    if (!tool.empty() && tool.item == items::shears) speed = shears_rule(s).speed;
    if (effects.haste >= 0) speed *= 1.0f + static_cast<float>(effects.haste + 1) * 0.2f;
    if (effects.fatigue >= 0) speed *= effects.fatigue == 0 ? 0.3f : effects.fatigue == 1 ? 0.09f : effects.fatigue == 2 ? 0.0027f : 8.1e-4f;
    if (eyes_in_water) speed *= 0.2f; // the submerged mining speed
    if (!on_ground) speed /= 5.0f;
    return speed / h / (harvests(s, tool, data) ? 30.0f : 100.0f);
}

float fluid_height(BlockState s, BlockState above) noexcept {
    if (!s.is(BlockType::water)) return 0.0f;
    if (above.is(BlockType::water)) return 1.0f;
    const int amount = s.flag() || s.level() == 0 ? 8 : 8 - s.level();
    return static_cast<float>(amount) / 9.0f;
}

} // namespace circuit::gameplay
