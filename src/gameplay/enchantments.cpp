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
    case Kind::clamped: {
        // Below the minimum the minimum, else at most the maximum (a pack's
        // minimum above its maximum gives one or the other).
        const float v = part(0);
        return v < a ? a : std::min(v, b);
    }
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

bool DamageRequirement::matches(const DamageContext& damage) const {
    switch (kind) {
    case Kind::tags:
        return std::ranges::all_of(tags, [&](const Tag& t) { return std::ranges::binary_search(t.types, damage.type) == t.expected; })
               && (!direct || *direct == damage.direct);
    case Kind::all_of: return std::ranges::all_of(terms, [&](const DamageRequirement& r) { return r.matches(damage); });
    case Kind::any_of: return std::ranges::any_of(terms, [&](const DamageRequirement& r) { return r.matches(damage); });
    case Kind::inverted: return !terms.empty() && !terms.front().matches(damage);
    case Kind::entity: {
        const auto type = subject == Subject::self ? damage.entity : subject == Subject::attacker ? damage.attacker : damage.direct_attacker;
        return !type.empty() && std::ranges::binary_search(entities, type);
    }
    case Kind::chance: return damage.draw && (*damage.draw)() < chance.at(damage.level);
    }
    return false;
}

bool EntityRequirement::matches(const EntityContext& entity) const {
    switch (kind) {
    case Kind::properties: return (!on_ground || *on_ground == entity.on_ground) && (!riding || entity.riding);
    case Kind::all_of: return std::ranges::all_of(terms, [&](const EntityRequirement& r) { return r.matches(entity); });
    case Kind::any_of: return std::ranges::any_of(terms, [&](const EntityRequirement& r) { return r.matches(entity); });
    case Kind::inverted: return !terms.empty() && !terms.front().matches(entity);
    }
    return false;
}

bool ItemRequirement::matches(std::string_view item) const {
    switch (kind) {
    case Kind::items: return std::ranges::binary_search(items, item);
    case Kind::all_of: return std::ranges::all_of(terms, [&](const ItemRequirement& r) { return r.matches(item); });
    case Kind::any_of: return std::ranges::any_of(terms, [&](const ItemRequirement& r) { return r.matches(item); });
    case Kind::inverted: return !terms.empty() && !terms.front().matches(item);
    }
    return false;
}

float ValueOperation::apply(float input, std::int32_t level, const std::function<float()>& draw) const {
    switch (kind) {
    case Kind::add: return input + value.at(level);
    case Kind::multiply: return input * value.at(level);
    case Kind::set: return value.at(level);
    case Kind::remove_binomial: {
        const float chance = value.at(level);
        // The value's whole units, each a draw (at most 2^20 of them: a
        // pack's huge values must not stall the tick; Circuit's limit).
        const auto units = input >= 1.0f ? static_cast<std::int64_t>(std::min(std::floor(input), 1048576.0f)) : 0;
        std::int64_t removed = 0;
        for (std::int64_t i = 0; i < units; ++i)
            if (draw && draw() < chance) ++removed;
        return input - static_cast<float>(removed);
    }
    case Kind::all_of:
        for (const auto& p : parts) input = p.apply(input, level, draw);
        return input;
    }
    return input;
}

} // namespace circuit::gameplay::enchantments
