// SPDX-License-Identifier: AGPL-3.0-only
// The items players eat and drink (client/eat).
#include <circuit/gameplay/food.hpp>
#include <circuit/gameplay/items.hpp>
#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>

namespace circuit::gameplay::food {
namespace {
using enum Sound;
using enum Special;

// The effects of the foods that give some (minecraft.wiki, each food's
// page), in the order both official servers give them (client/eat: an
// enchanted golden apple's regeneration, resistance, fire resistance and
// absorption; a pufferfish's poison, hunger and nausea).
constexpr std::array<Chance, 2> golden_apple{{{"regeneration", 1, 100, 1.0f}, {"absorption", 0, 2400, 1.0f}}};
constexpr std::array<Chance, 4> enchanted_golden_apple{
    {{"regeneration", 1, 400, 1.0f}, {"resistance", 0, 6000, 1.0f}, {"fire_resistance", 0, 6000, 1.0f}, {"absorption", 3, 2400, 1.0f}}};
constexpr std::array<Chance, 3> pufferfish{{{"poison", 1, 1200, 1.0f}, {"hunger", 2, 300, 1.0f}, {"nausea", 0, 300, 1.0f}}};
constexpr std::array<Chance, 1> rotten_flesh{{{"hunger", 0, 600, 0.8f}}};
constexpr std::array<Chance, 1> chicken{{{"hunger", 0, 600, 0.3f}}};
constexpr std::array<Chance, 1> spider_eye{{{"poison", 0, 100, 1.0f}}};
constexpr std::array<Chance, 1> poisonous_potato{{{"poison", 0, 100, 0.6f}}};

constexpr Consumable make(std::string_view item, std::int32_t nutrition, float modifier, bool always = false, std::int32_t ticks = 32,
                          Sound sound = eat, std::string_view remainder = "", std::span<const Chance> effects = {}, Special special = none) {
    return Consumable{item, nutrition, modifier, always, ticks, sound, remainder, effects, special};
}
// Nutrition and saturation modifiers (minecraft-data foods.json: half its
// saturationRatio over twice the nutrition), whether eaten with full
// hunger, the use's ticks (dried kelp 16, honey 40; the others 32), the
// sound, the remainder, the effects and what else consuming does.
const std::array<Consumable, 43> table{
    make("apple", 4, 0.3f),
    make("baked_potato", 5, 0.6f),
    make("beef", 3, 0.3f),
    make("beetroot", 1, 0.6f),
    make("beetroot_soup", 6, 0.6f, false, 32, eat, "bowl"),
    make("bread", 5, 0.6f),
    make("carrot", 3, 0.6f),
    make("chicken", 2, 0.3f, false, 32, eat, "", chicken),
    make("chorus_fruit", 4, 0.3f, true, 32, eat, "", {}, teleport),
    make("cod", 2, 0.1f),
    make("cooked_beef", 8, 0.8f),
    make("cooked_chicken", 6, 0.6f),
    make("cooked_cod", 5, 0.6f),
    make("cooked_mutton", 6, 0.8f),
    make("cooked_porkchop", 8, 0.8f),
    make("cooked_rabbit", 5, 0.6f),
    make("cooked_salmon", 6, 0.8f),
    make("cookie", 2, 0.1f),
    make("dried_kelp", 1, 0.3f, false, 16),
    make("enchanted_golden_apple", 4, 1.2f, true, 32, eat, "", enchanted_golden_apple),
    make("glow_berries", 2, 0.1f),
    make("golden_apple", 4, 1.2f, true, 32, eat, "", golden_apple),
    make("golden_carrot", 6, 1.2f),
    make("honey_bottle", 6, 0.1f, true, 40, honey_drink, "glass_bottle", {}, remove_poison),
    make("melon_slice", 2, 0.3f),
    make("mushroom_stew", 6, 0.6f, false, 32, eat, "bowl"),
    make("mutton", 2, 0.3f),
    make("poisonous_potato", 2, 0.3f, false, 32, eat, "", poisonous_potato),
    make("porkchop", 3, 0.3f),
    make("potato", 1, 0.3f),
    make("pufferfish", 1, 0.1f, false, 32, eat, "", pufferfish),
    make("pumpkin_pie", 8, 0.3f),
    make("rabbit", 3, 0.3f),
    make("rabbit_stew", 10, 0.6f, false, 32, eat, "bowl"),
    make("rotten_flesh", 4, 0.1f, false, 32, eat, "", rotten_flesh),
    make("salmon", 2, 0.1f),
    make("spider_eye", 2, 0.8f, false, 32, eat, "", spider_eye),
    make("suspicious_stew", 6, 0.6f, true, 32, eat, "bowl", {}, stew),
    make("sweet_berries", 2, 0.1f),
    make("tropical_fish", 1, 0.1f),
    // What is drunk but is no food: always, and without sounds at the end.
    make("milk_bucket", 0, 0.0f, true, 32, drink, "bucket", {}, clear_effects),
    make("potion", 0, 0.0f, true, 32, drink, "glass_bottle", {}, potion),
    make("ominous_bottle", 0, 0.0f, true, 32, drink, "", {}, ominous),
};

const std::unordered_map<std::uint16_t, const Consumable*>& by_item() {
    static const auto map = [] {
        std::unordered_map<std::uint16_t, const Consumable*> m;
        for (const auto& c : table)
            if (const auto id = items::by_name("minecraft:" + std::string(c.item))) m.emplace(*id, &c);
        return m;
    }();
    return map;
}
} // namespace

const Consumable* of(std::uint16_t item) {
    const auto& map = by_item();
    const auto it = map.find(item);
    return it == map.end() ? nullptr : it->second;
}

std::optional<std::uint16_t> remainder(std::uint16_t item) {
    const auto* c = of(item);
    if (!c || c->remainder.empty()) return std::nullopt;
    return items::by_name("minecraft:" + std::string(c->remainder));
}

bool use_sound(std::int32_t ticks, std::int32_t left) noexcept {
    if (left == 0) return true;
    const auto before = left + 1;
    return left > 0 && before % 4 == 0 && ticks - before > ticks / 5;
}

} // namespace circuit::gameplay::food
