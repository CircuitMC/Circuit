// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/loot.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>

namespace circuit::gameplay::loot {
namespace {
// No value: the evaluation needs a random number and has no source.
struct Unresolved {};

RandomSequence& random(const BreakContext& ctx) {
    if (!ctx.random) throw Unresolved{};
    return *ctx.random;
}

long level_of(const BreakContext& ctx, std::uint32_t enchantment) {
    return ctx.tool.empty() || !ctx.enchantment_level ? 0 : ctx.enchantment_level(enchantment);
}

long number(const Number& n, const BreakContext& ctx) {
    switch (n.kind) {
    case Number::Kind::constant: return std::lround(n.value);
    case Number::Kind::uniform: {
        const auto lo = static_cast<long>(std::floor(n.min)), hi = static_cast<long>(std::floor(n.max));
        if (hi < lo) return lo;
        return random(ctx).next_int(static_cast<std::int32_t>(hi - lo + 1)) + lo;
    }
    case Number::Kind::binomial: {
        long count = 0;
        for (long i = 0; i < n.n; ++i)
            if (random(ctx).next_float() < static_cast<float>(n.p)) ++count;
        return count;
    }
    }
    return 0;
}

bool test(const Condition& c, const BreakContext& ctx);
bool all(const std::vector<Condition>& cs, const BreakContext& ctx) {
    return std::ranges::all_of(cs, [&](const Condition& c) { return test(c, ctx); });
}

bool test(const Condition& c, const BreakContext& ctx) {
    switch (c.kind) {
    case Condition::Kind::survives_explosion: return true; // a player's break, or TNT (no drop decay)
    case Condition::Kind::block_state:
        if (c.other_block) return false;
        for (const auto& p : c.properties) {
            const auto it = std::ranges::find(ctx.properties, p.name, &std::pair<std::string, std::string>::first);
            if (it == ctx.properties.end()) return false;
            if (p.exact && it->second != *p.exact) return false;
            if (p.min || p.max) {
                long v{};
                const auto [end, error] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
                if (error != std::errc{} || (p.min && v < *p.min) || (p.max && v > *p.max)) return false;
            }
        }
        return true;
    case Condition::Kind::tool:
        if (!c.items.empty() && (ctx.tool.empty() || std::ranges::find(c.items, ctx.tool.item) == c.items.end())) return false;
        for (const auto& e : c.enchantments) {
            const long level = level_of(ctx, e.id);
            if (level < e.min || level > e.max) return false;
        }
        return true;
    case Condition::Kind::any_of: return std::ranges::any_of(c.children, [&](const Condition& x) { return test(x, ctx); });
    case Condition::Kind::all_of: return all(c.children, ctx);
    case Condition::Kind::inverted: return !c.children.empty() && !test(c.children.front(), ctx);
    case Condition::Kind::random_chance: return random(ctx).next_float() < static_cast<float>(c.chance);
    case Condition::Kind::table_bonus: {
        const auto level = static_cast<std::size_t>(std::max(0L, level_of(ctx, c.bonus_enchantment)));
        return random(ctx).next_float() < static_cast<float>(c.chances[std::min(level, c.chances.size() - 1)]);
    }
    case Condition::Kind::location: return ctx.location && ctx.location(c);
    case Condition::Kind::breaker: return ctx.by_entity;
    }
    return false;
}

void apply(const std::vector<Function>& fs, long& count, const BreakContext& ctx) {
    for (const auto& f : fs) {
        if (!all(f.conditions, ctx)) continue;
        switch (f.kind) {
        case Function::Kind::set_count: {
            const long n = number(f.count, ctx);
            count = f.add ? count + n : n;
            break;
        }
        case Function::Kind::limit_count:
            if (f.min) count = std::max(count, *f.min);
            if (f.max) count = std::min(count, *f.max);
            break;
        case Function::Kind::explosion_decay: break; // no explosion, or TNT's (no drop decay)
        case Function::Kind::ore_bonus: {
            // Fortune multiplies by 1 + max(0, an int below level + 2, less 1)
            // (minecraft.wiki "Loot table"); nothing is drawn at level 0.
            const long level = level_of(ctx, f.enchantment);
            if (level > 0) count *= std::max<long>(0, random(ctx).next_int(static_cast<std::int32_t>(level + 2)) - 1) + 1;
            break;
        }
        case Function::Kind::uniform_bonus:
            // Drawn at level 0 too (loot_many).
            count += random(ctx).next_int(static_cast<std::int32_t>(f.multiplier * level_of(ctx, f.enchantment) + 1));
            break;
        case Function::Kind::binomial_bonus:
            for (long i = 0, n = level_of(ctx, f.enchantment) + f.extra; i < n; ++i)
                if (random(ctx).next_float() < static_cast<float>(f.probability)) ++count;
            break;
        }
    }
}

// The entries a roll may choose, after conditions; composites expand.
bool expand(const Entry& e, const BreakContext& ctx, std::vector<const Entry*>& out) {
    if (!all(e.conditions, ctx)) return false;
    switch (e.kind) {
    case Entry::Kind::item: case Entry::Kind::empty: out.push_back(&e); return true;
    case Entry::Kind::alternatives:
        for (const auto& c : e.children)
            if (expand(c, ctx, out)) return true;
        return false;
    case Entry::Kind::group: // every child
        for (const auto& c : e.children) expand(c, ctx, out);
        return true;
    case Entry::Kind::sequence: // children until one fails
        for (const auto& c : e.children)
            if (!expand(c, ctx, out)) return false;
        return true;
    }
    return false;
}
} // namespace

std::optional<std::vector<contracts::ItemStack>> drops(const Table& table, const BreakContext& ctx) {
    std::vector<contracts::ItemStack> out;
    try {
        for (const auto& pool : table.pools) {
            if (!all(pool.conditions, ctx)) continue;
            for (long roll = 0; roll < pool.rolls; ++roll) {
                std::vector<const Entry*> candidates;
                for (const auto& e : pool.entries) expand(e, ctx, candidates);
                if (candidates.empty()) continue;
                if (candidates.size() > 1) return std::nullopt; // a weighted random choice
                const auto& chosen = *candidates.front();
                if (chosen.kind != Entry::Kind::item) continue;
                contracts::ItemStack stack{chosen.item, 1};
                long count = 1;
                apply(chosen.functions, count, ctx);
                apply(pool.functions, count, ctx);
                apply(table.functions, count, ctx);
                // Stacks above the item's limit split, as the target version
                // splits them; one of none drops as it is.
                const long limit = std::max<long>(1, items::max_stack(stack.item));
                if (count <= 0) {
                    stack.count = 0;
                    out.push_back(stack);
                }
                while (count > 0) {
                    const long n = std::min(count, limit);
                    stack.count = static_cast<std::uint8_t>(n);
                    out.push_back(stack);
                    count -= n;
                }
            }
        }
    } catch (const Unresolved&) {
        return std::nullopt;
    }
    return out;
}

} // namespace circuit::gameplay::loot
