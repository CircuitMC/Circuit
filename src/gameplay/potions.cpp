// SPDX-License-Identifier: AGPL-3.0-only
// The potion registry and the brewing mixes (minecraft.wiki "Brewing"),
// checked against both official servers' brewing report for every potion,
// bottle and ingredient.
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/potions.hpp>
#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace circuit::gameplay::potions {
namespace {
constexpr std::array<std::string_view, 46> potion_names{
    "minecraft:water", "minecraft:mundane", "minecraft:thick", "minecraft:awkward", "minecraft:night_vision",
    "minecraft:long_night_vision", "minecraft:invisibility", "minecraft:long_invisibility", "minecraft:leaping",
    "minecraft:long_leaping", "minecraft:strong_leaping", "minecraft:fire_resistance", "minecraft:long_fire_resistance",
    "minecraft:swiftness", "minecraft:long_swiftness", "minecraft:strong_swiftness", "minecraft:slowness",
    "minecraft:long_slowness", "minecraft:strong_slowness", "minecraft:turtle_master", "minecraft:long_turtle_master",
    "minecraft:strong_turtle_master", "minecraft:water_breathing", "minecraft:long_water_breathing", "minecraft:healing",
    "minecraft:strong_healing", "minecraft:harming", "minecraft:strong_harming", "minecraft:poison", "minecraft:long_poison",
    "minecraft:strong_poison", "minecraft:regeneration", "minecraft:long_regeneration", "minecraft:strong_regeneration",
    "minecraft:strength", "minecraft:long_strength", "minecraft:strong_strength", "minecraft:weakness",
    "minecraft:long_weakness", "minecraft:luck", "minecraft:slow_falling", "minecraft:long_slow_falling",
    "minecraft:wind_charged", "minecraft:weaving", "minecraft:oozing", "minecraft:infested"};

// Water with an ingredient: awkward, thick, weakness or mundane.
std::optional<std::string_view> from_water(std::string_view ingredient) {
    if (ingredient == "nether_wart") return "minecraft:awkward";
    if (ingredient == "glowstone_dust") return "minecraft:thick";
    if (ingredient == "fermented_spider_eye") return "minecraft:weakness";
    static constexpr std::array<std::string_view, 12> mundane{"stone", "cobweb", "redstone", "slime_block", "sugar", "ghast_tear",
                                                              "spider_eye", "blaze_powder", "magma_cream", "glistering_melon_slice",
                                                              "breeze_rod", "rabbit_foot"};
    if (std::ranges::find(mundane, ingredient) != mundane.end()) return "minecraft:mundane";
    return std::nullopt;
}
// Awkward potions with an ingredient: the potion of its effect.
constexpr std::array<std::pair<std::string_view, std::string_view>, 15> from_awkward{{
    {"stone", "minecraft:infested"}, {"cobweb", "minecraft:weaving"}, {"slime_block", "minecraft:oozing"},
    {"phantom_membrane", "minecraft:slow_falling"}, {"turtle_helmet", "minecraft:turtle_master"},
    {"pufferfish", "minecraft:water_breathing"}, {"sugar", "minecraft:swiftness"}, {"ghast_tear", "minecraft:regeneration"},
    {"spider_eye", "minecraft:poison"}, {"blaze_powder", "minecraft:strength"}, {"magma_cream", "minecraft:fire_resistance"},
    {"glistering_melon_slice", "minecraft:healing"}, {"breeze_rod", "minecraft:wind_charged"},
    {"golden_carrot", "minecraft:night_vision"}, {"rabbit_foot", "minecraft:leaping"}}};
// Redstone lengthens, glowstone strengthens and a fermented spider eye
// corrupts: (potion, ingredient, result).
struct Change {
    std::string_view from, ingredient, to;
};
constexpr std::array<Change, 33> changes{{
    {"night_vision", "redstone", "long_night_vision"}, {"night_vision", "fermented_spider_eye", "invisibility"},
    {"long_night_vision", "fermented_spider_eye", "long_invisibility"}, {"invisibility", "redstone", "long_invisibility"},
    {"leaping", "redstone", "long_leaping"}, {"leaping", "glowstone_dust", "strong_leaping"},
    {"leaping", "fermented_spider_eye", "slowness"}, {"long_leaping", "fermented_spider_eye", "long_slowness"},
    {"fire_resistance", "redstone", "long_fire_resistance"}, {"swiftness", "redstone", "long_swiftness"},
    {"swiftness", "glowstone_dust", "strong_swiftness"}, {"swiftness", "fermented_spider_eye", "slowness"},
    {"long_swiftness", "fermented_spider_eye", "long_slowness"}, {"slowness", "redstone", "long_slowness"},
    {"slowness", "glowstone_dust", "strong_slowness"}, {"turtle_master", "redstone", "long_turtle_master"},
    {"turtle_master", "glowstone_dust", "strong_turtle_master"}, {"water_breathing", "redstone", "long_water_breathing"},
    {"healing", "glowstone_dust", "strong_healing"}, {"healing", "fermented_spider_eye", "harming"},
    {"strong_healing", "fermented_spider_eye", "strong_harming"}, {"harming", "glowstone_dust", "strong_harming"},
    {"poison", "redstone", "long_poison"}, {"poison", "glowstone_dust", "strong_poison"},
    {"poison", "fermented_spider_eye", "harming"}, {"long_poison", "fermented_spider_eye", "harming"},
    {"strong_poison", "fermented_spider_eye", "strong_harming"}, {"regeneration", "redstone", "long_regeneration"},
    {"regeneration", "glowstone_dust", "strong_regeneration"}, {"strength", "redstone", "long_strength"},
    {"strength", "glowstone_dust", "strong_strength"}, {"weakness", "redstone", "long_weakness"},
    {"slow_falling", "redstone", "long_slow_falling"}}};
std::string_view bare(std::string_view name) { return name.starts_with("minecraft:") ? name.substr(10) : name; }

// Each potion's effects, in id order (minecraft.wiki "Potion").
struct Effects {
    std::array<Effect, 2> list;
    std::size_t count{};
};
constexpr Effects none{};
constexpr Effects one(std::string_view effect, std::int32_t amplifier, std::int32_t duration) {
    return {{{{effect, amplifier, duration}, {}}}, 1};
}
constexpr std::array<Effects, 46> potion_effects{{
    none, none, none, none,                                                             // water, mundane, thick, awkward
    one("night_vision", 0, 3600), one("night_vision", 0, 9600),                         // night vision
    one("invisibility", 0, 3600), one("invisibility", 0, 9600),                         // invisibility
    one("jump_boost", 0, 3600), one("jump_boost", 0, 9600), one("jump_boost", 1, 1800), // leaping
    one("fire_resistance", 0, 3600), one("fire_resistance", 0, 9600),                   // fire resistance
    one("speed", 0, 3600), one("speed", 0, 9600), one("speed", 1, 1800),                // swiftness
    one("slowness", 0, 1800), one("slowness", 0, 4800), one("slowness", 3, 400),        // slowness
    {{{{"slowness", 3, 400}, {"resistance", 2, 400}}}, 2},                              // turtle master
    {{{{"slowness", 3, 800}, {"resistance", 2, 800}}}, 2},
    {{{{"slowness", 5, 400}, {"resistance", 3, 400}}}, 2},
    one("water_breathing", 0, 3600), one("water_breathing", 0, 9600),                   // water breathing
    one("instant_health", 0, 1), one("instant_health", 1, 1),                           // healing
    one("instant_damage", 0, 1), one("instant_damage", 1, 1),                           // harming
    one("poison", 0, 900), one("poison", 0, 1800), one("poison", 1, 432),               // poison
    one("regeneration", 0, 900), one("regeneration", 0, 1800), one("regeneration", 1, 450), // regeneration
    one("strength", 0, 3600), one("strength", 0, 9600), one("strength", 1, 1800),       // strength
    one("weakness", 0, 1800), one("weakness", 0, 4800),                                 // weakness
    one("luck", 0, 6000),                                                               // luck
    one("slow_falling", 0, 1800), one("slow_falling", 0, 4800),                         // slow falling
    one("wind_charged", 0, 3600), one("weaving", 0, 3600), one("oozing", 0, 3600), one("infested", 0, 3600),
}};
} // namespace

std::span<const Effect> effects(std::uint32_t potion) noexcept {
    if (potion >= potion_effects.size()) return {};
    const auto& e = potion_effects[potion];
    return std::span(e.list).first(e.count);
}

std::span<const std::string_view> names() noexcept { return potion_names; }

std::optional<std::uint32_t> id(std::string_view name) noexcept {
    const auto it = std::ranges::find(potion_names, name);
    if (it == potion_names.end()) return std::nullopt;
    return static_cast<std::uint32_t>(it - potion_names.begin());
}

bool ingredient(std::uint16_t item) noexcept {
    static constexpr std::array<std::string_view, 21> all{
        "stone", "cobweb", "redstone", "slime_block", "phantom_membrane", "turtle_helmet", "gunpowder", "glowstone_dust",
        "pufferfish", "sugar", "ghast_tear", "nether_wart", "spider_eye", "fermented_spider_eye", "blaze_powder", "magma_cream",
        "glistering_melon_slice", "breeze_rod", "golden_carrot", "rabbit_foot", "dragon_breath"};
    const auto* info = items::info(item);
    return info && std::ranges::find(all, bare(info->name)) != all.end();
}

std::optional<Bottle> mix(const Bottle& bottle, std::uint16_t ingredient_item) noexcept {
    const auto* info = items::info(ingredient_item);
    if (!info || !id(bottle.potion)) return std::nullopt;
    const auto with = bare(info->name);
    constexpr auto potion = items::potion, splash = items::splash_potion, lingering = items::lingering_potion;
    if (bottle.item != potion && bottle.item != splash && bottle.item != lingering) return std::nullopt;
    // The container mixes keep the potion.
    if (with == "gunpowder") return bottle.item == potion ? std::optional(Bottle{splash, bottle.potion}) : std::nullopt;
    if (with == "dragon_breath") return bottle.item == splash ? std::optional(Bottle{lingering, bottle.potion}) : std::nullopt;
    const auto made = [&](std::string_view to) -> std::optional<Bottle> {
        const auto it = std::ranges::find(potion_names, std::string("minecraft:") + std::string(to));
        if (it == potion_names.end()) return std::nullopt;
        return Bottle{bottle.item, *it};
    };
    const auto from = bare(bottle.potion);
    if (from == "water") {
        const auto to = from_water(with);
        return to ? made(bare(*to)) : std::nullopt;
    }
    if (from == "awkward") {
        const auto it = std::ranges::find(from_awkward, with, &std::pair<std::string_view, std::string_view>::first);
        return it == from_awkward.end() ? std::nullopt : made(bare(it->second));
    }
    for (const auto& c : changes)
        if (c.from == from && c.ingredient == with) return made(c.to);
    return std::nullopt;
}

} // namespace circuit::gameplay::potions
