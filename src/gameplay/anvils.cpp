// SPDX-License-Identifier: AGPL-3.0-only
// Anvils: what one makes of its two inputs and the name a player typed, its
// level cost, and what taking the result uses up (minecraft.wiki "Anvil"
// and "Anvil mechanics"; compared with both official servers through a
// protocol client).
#include "internal.hpp"
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/experience.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/physics.hpp>
#include <circuit/gameplay/rng.hpp>
#include <algorithm>
#include <bit>
#include <climits>

namespace circuit::gameplay::detail {
namespace {

// Whether one of two enchantments lists the other in its exclusive set.
bool excludes(const GameData& data, std::uint32_t a, std::uint32_t b) {
    const auto lists = [&](std::uint32_t x, std::uint32_t y) {
        const auto it = data.enchantment_rules.find(x);
        return it != data.enchantment_rules.end() && std::ranges::binary_search(it->second.exclusive, y);
    };
    return lists(a, b) || lists(b, a);
}

// Whether a name is empty or white space only (the code points Java counts
// as white space; the server has already removed control characters).
bool blank(std::string_view name) {
    for (std::size_t i = 0; i < name.size();) {
        const auto lead = static_cast<unsigned char>(name[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        std::uint32_t cp = length == 1 ? lead : lead & (0x7Fu >> length);
        for (std::size_t k = 1; k < length && i + k < name.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(name[i + k]) & 0x3Fu);
        i += length;
        const bool space = cp == ' ' || (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x1F) || cp == 0x1680
                        || (cp >= 0x2000 && cp <= 0x2006) || (cp >= 0x2008 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029
                        || cp == 0x205F || cp == 0x3000;
        if (!space) return false;
    }
    return true;
}

// A stack's name as a player sees it: the server's (the jar's English
// names), else its custom name or its item's id in title case.
std::string shown_name(const GameData& data, const ItemStack& s) {
    if (data.display_name) return data.display_name(s);
    if (data.custom_name_of)
        if (auto name = data.custom_name_of(s.components)) return std::move(*name);
    const auto* info = items::info(s.item);
    std::string out;
    bool start = true;
    for (const char ch : info ? info->name.substr(info->name.find(':') + 1) : std::string_view{}) {
        out += ch == '_' ? ' ' : start && ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
        start = ch == '_';
    }
    return out;
}

struct Forged {
    ItemStack result;
    std::int32_t cost{};      // the level cost the window shows
    std::int32_t materials{}; // of the right input a repair uses; 0: all of it
};

// What an anvil makes, as both official servers make it:
// - a repair material of the left item (its material's item tag) repairs a
//   quarter of its durability per item, a level each, while it is worn
//   (none when it is not);
// - the same item that wears restores both durabilities and 12% of the
//   most (2 levels when that repairs it); with an enchanted book, or the
//   same item, each enchantment of the right input joins the left's: the
//   same level one more, else the higher, at most its highest level; one
//   the left item does not take (unless it is a book, or the player is in
//   creative mode) or that conflicts with one there is left out, a level
//   for each conflict; each joined costs its anvil cost (halved for a book,
//   at least 1) per level; when all are left out there is no result;
// - a name that differs from the shown one (a blank one removes a custom
//   name) costs a level more.
// Without any of these there is no result. The inputs' prior work
// penalties (their repair costs) add to the cost; renaming alone costs at
// most 39 levels; 40 or more is too expensive outside creative mode. The
// result's repair cost is the inputs' higher one, doubled and one more
// unless it was only renamed.
Forged forge(const GameData& data, const ItemStack& left, const ItemStack& right, const std::string& name, bool creative) {
    if (left.empty() || !data.enchantments_of || !data.with_enchantments || !data.repair_cost_of || !data.with_custom_name) return {};
    auto made = left;
    std::int32_t cost = 0;
    std::int32_t materials = 0;
    const bool book_target = left.item == items::enchanted_book;
    auto levels = data.enchantments_of(left.components, book_target);
    const std::int32_t left_rc = data.repair_cost_of(left.components);
    const std::int32_t right_rc = right.empty() ? 0 : data.repair_cost_of(right.components);
    if (!right.empty()) {
        const auto stored = data.enchantments_of(right.components, true);
        const bool book = right.item == items::enchanted_book && !stored.empty();
        const int most = items::max_damage(left);
        const auto repairs = data.repair_materials.find(left.item);
        if (most > 0 && repairs != data.repair_materials.end() && std::ranges::binary_search(repairs->second, right.item)) {
            int repair = std::min<int>(made.damage, most / 4);
            if (repair <= 0) return {};
            for (; repair > 0 && materials < right.count; ++materials, ++cost) {
                made.damage = static_cast<std::uint16_t>(made.damage - repair);
                repair = std::min<int>(made.damage, most / 4);
            }
        } else {
            if (!book && (left.item != right.item || most <= 0)) return {};
            if (most > 0 && !book) {
                const int damage = std::max(0, most - ((most - left.damage) + (most - right.damage) + most * 12 / 100));
                if (damage < made.damage) {
                    made.damage = static_cast<std::uint16_t>(damage);
                    cost += 2;
                }
            }
            bool joined = false;
            bool left_out = false;
            for (const auto& [id, level] : book ? stored : data.enchantments_of(right.components, false)) {
                const auto rules = data.enchantment_rules.find(id);
                if (rules == data.enchantment_rules.end()) continue;
                const auto there = std::ranges::find(levels, id, &std::pair<std::uint32_t, std::int32_t>::first);
                const auto now = there == levels.end() ? 0 : there->second;
                auto next = now == level ? level + 1 : std::max(now, level);
                bool takes = creative || book_target || std::ranges::binary_search(rules->second.supported, left.item);
                for (const auto& [other, other_level] : levels)
                    if (other != id && excludes(data, id, other)) {
                        takes = false;
                        ++cost;
                    }
                if (!takes) {
                    left_out = true;
                    continue;
                }
                joined = true;
                next = std::min(next, rules->second.max_level);
                if (there == levels.end()) levels.emplace_back(id, next);
                else there->second = next;
                const auto per_level = book ? std::max(1, rules->second.anvil_cost / 2) : rules->second.anvil_cost;
                cost += per_level * next;
            }
            if (left_out && !joined) return {};
        }
    }
    std::int32_t renamed = 0;
    std::optional<std::optional<std::string>> new_name;
    if (blank(name)) {
        if (data.custom_name_of && data.custom_name_of(left.components)) {
            renamed = 1;
            new_name = std::optional<std::string>{};
        }
    } else if (name != shown_name(data, left)) {
        renamed = 1;
        new_name = name;
    }
    cost += renamed;
    if (cost <= 0) return {};
    const bool only_renamed = renamed > 0 && cost == renamed;
    auto total = std::min<std::int64_t>(std::int64_t{left_rc} + right_rc + cost, INT_MAX);
    if (only_renamed && total >= 40) total = 39;
    Forged out;
    out.cost = static_cast<std::int32_t>(total);
    out.materials = materials;
    if (out.cost >= 40 && !creative) return out;
    std::int64_t repair_cost = std::max(left_rc, right_rc);
    if (!only_renamed) repair_cost = std::min<std::int64_t>(repair_cost * 2 + 1, INT_MAX);
    auto patch = data.with_enchantments(made.components, book_target, levels, false, static_cast<std::int32_t>(repair_cost));
    if (patch && new_name) patch = data.with_custom_name(*patch, *new_name);
    if (!patch) return {};
    made.components = *patch;
    out.result = made;
    return out;
}

} // namespace

std::optional<BlockState> worn_anvil(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    if (!block) return std::nullopt;
    const auto name = registry::game_name(*block);
    const std::string_view next = name == "anvil" ? "chipped_anvil" : name == "chipped_anvil" ? "damaged_anvil" : "";
    if (next.empty()) return std::nullopt;
    for (std::uint32_t b = 0; b < registry::block_count(); ++b)
        if (registry::game_name(b) == next)
            if (const auto worn_state = registry::state_of(b, registry::properties(*state))) return BlockState::registered(*worn_state);
    return std::nullopt;
}

void set_anvil_result(RuleContext& ctx, PlayerInventory& inv, bool creative) {
    const auto* data = ctx.data();
    const auto forged = data ? forge(*data, inv.anvil[0], inv.anvil[1], inv.anvil_name, creative) : Forged{};
    inv.anvil_cost = forged.cost;
    if (inv.anvil[2] == forged.result) return;
    inv.anvil[2] = forged.result;
    record_slot(ctx, inv, contracts::anvil_first + 2);
}

bool anvil_result_takeable(const PlayerInventory& inv, bool creative) {
    return inv.anvil_cost > 0 && (creative || inv.xp_level >= inv.anvil_cost);
}

void use_anvil(RuleContext& ctx, PlayerInventory& inv, std::optional<BlockPos> at, bool creative) {
    const auto* data = ctx.data();
    const auto forged = data ? forge(*data, inv.anvil[0], inv.anvil[1], inv.anvil_name, creative) : Forged{};
    // The levels it cost, outside creative mode.
    if (!creative) {
        const auto before = inv.xp_total;
        experience::give_levels(inv, -inv.anvil_cost);
        if (inv.xp_total != before)
            ctx.record(TraceKind::player_experience_changed, at.value_or(BlockPos{}), inv.player.hi, inv.player.lo,
                       static_cast<std::uint64_t>(static_cast<std::uint32_t>(inv.xp_total)));
    }
    // The left input is used up; the right one too, or as many of it as the
    // repair used.
    auto right = inv.anvil[1];
    if (forged.materials > 0 && right.count > forged.materials) right.count = static_cast<std::uint8_t>(right.count - forged.materials);
    else right = {};
    for (const auto& [slot, now] : {std::pair<std::uint8_t, ItemStack>{contracts::anvil_first, ItemStack{}},
                                    std::pair<std::uint8_t, ItemStack>{static_cast<std::uint8_t>(contracts::anvil_first + 1), right}}) {
        auto& held = inv.anvil[slot - contracts::anvil_first];
        if (held == now) continue;
        held = now;
        record_slot(ctx, inv, slot);
    }
    inv.anvil_cost = 0;
    // Outside creative mode the anvil wears with a chance of 12%, by an
    // unseeded random on the official servers and by one of Circuit's own
    // from the tick, the player and the place.
    if (!creative && at && is_anvil(ctx.block(*at))) {
        auto chance = Rng::derive({ctx.now(), inv.player.hi, inv.player.lo, static_cast<std::uint64_t>(static_cast<std::uint32_t>(at->x)),
                                   static_cast<std::uint64_t>(static_cast<std::uint32_t>(at->y)),
                                   static_cast<std::uint64_t>(static_cast<std::uint32_t>(at->z)), 0x414E56494CULL});
        if (static_cast<float>(chance.bounded(1u << 24)) / static_cast<float>(1u << 24) < 0.12f) {
            if (const auto next = worn_anvil(ctx.block(*at))) set_block_quiet(ctx, *at, *next);
            else set_block(ctx, *at, blocks::air());
        }
    }
    set_anvil_result(ctx, inv, creative);
}

bool valid_item_name(std::string_view name) {
    // UTF-8 without control characters or the section sign, of at most
    // max_item_name UTF-16 code units.
    std::size_t units = 0;
    for (std::size_t i = 0; i < name.size();) {
        const auto lead = static_cast<unsigned char>(name[i]);
        const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
        if (length == 0 || i + length > name.size()) return false;
        std::uint32_t cp = length == 1 ? lead : lead & (0x7Fu >> length);
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(name[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            cp = cp << 6 | (next & 0x3Fu);
        }
        if (cp < 0x20 || cp == 0x7F || cp == 0xA7 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return false;
        units += cp >= 0x10000 ? 2 : 1;
        i += length;
    }
    return units <= contracts::max_item_name;
}

} // namespace circuit::gameplay::detail
