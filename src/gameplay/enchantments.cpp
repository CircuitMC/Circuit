// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/enchantments.hpp>
#include <algorithm>
#include <cmath>

namespace circuit::gameplay::enchantments {

float LevelValue::at(std::int32_t level) const noexcept {
    const auto part = [&](std::size_t k) { return k < parts.size() ? parts[k].at(level) : 0.0f; };
    switch (kind) {
    case Kind::constant: return a;
    case Kind::linear: return a + b * static_cast<float>(level - 1);
    case Kind::levels_squared: return static_cast<float>(level) * static_cast<float>(level) + a;
    case Kind::fraction: {
        const float denominator = part(1);
        return denominator == 0.0f ? 0.0f : part(0) / denominator;
    }
    case Kind::clamped: return std::clamp(part(0), a, b);
    case Kind::lookup:
        return level >= 1 && static_cast<std::size_t>(level) <= values.size() ? values[static_cast<std::size_t>(level - 1)] : part(0);
    case Kind::exponent: return std::pow(part(0), part(1));
    }
    return 0.0f;
}

std::optional<std::uint8_t> slot_group(std::string_view name) noexcept {
    using equipment::Slot;
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    constexpr auto armor = static_cast<std::uint8_t>(slot_bit(Slot::feet) | slot_bit(Slot::legs) | slot_bit(Slot::chest) | slot_bit(Slot::head));
    constexpr auto hand = static_cast<std::uint8_t>(slot_bit(Slot::mainhand) | slot_bit(Slot::offhand));
    if (name == "any") return static_cast<std::uint8_t>(armor | hand);
    if (name == "hand") return hand;
    if (name == "armor") return armor;
    if (name == "mainhand") return slot_bit(Slot::mainhand);
    if (name == "offhand") return slot_bit(Slot::offhand);
    if (name == "feet") return slot_bit(Slot::feet);
    if (name == "legs") return slot_bit(Slot::legs);
    if (name == "chest") return slot_bit(Slot::chest);
    if (name == "head") return slot_bit(Slot::head);
    if (name == "body" || name == "saddle") return std::uint8_t{0};
    return std::nullopt;
}

} // namespace circuit::gameplay::enchantments
