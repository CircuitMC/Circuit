// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
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
    default: return {};
    }
}

// The tool kind that is effective on a block (its mineable tag).
Tool effective_tool(BlockType t) noexcept {
    switch (t) {
    case BlockType::stone: case BlockType::obsidian: case BlockType::redstone_block: case BlockType::observer:
    case BlockType::piston: case BlockType::sticky_piston: case BlockType::piston_head: case BlockType::hopper:
        return Tool::pickaxe;
    case BlockType::chest: return Tool::axe;
    default: return Tool::none;
    }
}

// Lowest tier a block needs for its drops; 0 when it drops with any tool.
int required_tier(BlockType t) noexcept {
    switch (t) {
    case BlockType::stone: case BlockType::redstone_block: case BlockType::observer: case BlockType::hopper: return 1;
    case BlockType::obsidian: return 4;
    default: return 0;
    }
}
} // namespace

float hardness(BlockType t) noexcept {
    switch (t) {
    case BlockType::stone: case BlockType::piston: case BlockType::sticky_piston: case BlockType::piston_head: return 1.5f;
    case BlockType::glass: case BlockType::redstone_lamp: return 0.3f;
    case BlockType::obsidian: return 50.0f;
    case BlockType::redstone_block: return 5.0f;
    case BlockType::lever: return 0.5f;
    case BlockType::observer: case BlockType::hopper: return 3.0f;
    case BlockType::chest: return 2.5f;
    case BlockType::slime_block: case BlockType::redstone_wire: case BlockType::repeater: case BlockType::comparator:
    case BlockType::wheat:
        return 0.0f;
    default: return -1.0f; // air, water, moving pistons
    }
}

bool harvests(BlockState s, contracts::ItemStack tool) noexcept {
    const auto need = required_tier(s.type());
    if (need == 0) return true;
    const auto t = tool.empty() ? ToolInfo{} : tool_of(tool.item);
    return t.kind == effective_tool(s.type()) && t.tier >= need;
}

float destroy_progress(BlockState s, contracts::ItemStack tool, bool on_ground, bool eyes_in_water) noexcept {
    const float h = hardness(s.type());
    if (h < 0.0f) return 0.0f;
    if (h == 0.0f) return 1.0f;
    const auto t = tool.empty() ? ToolInfo{} : tool_of(tool.item);
    float speed = t.kind != Tool::none && t.kind == effective_tool(s.type()) ? t.speed : 1.0f;
    if (eyes_in_water) speed *= 0.2f; // the submerged mining speed
    if (!on_ground) speed /= 5.0f;
    return speed / h / (harvests(s, tool) ? 30.0f : 100.0f);
}

float fluid_height(BlockState s, BlockState above) noexcept {
    if (!s.is(BlockType::water)) return 0.0f;
    if (above.is(BlockType::water)) return 1.0f;
    const int amount = s.flag() || s.level() == 0 ? 8 : 8 - s.level();
    return static_cast<float>(amount) / 9.0f;
}

} // namespace circuit::gameplay
