// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"

namespace circuit::gameplay::detail {
namespace {
using contracts::pack_stack;
using blocks::ChestType;
constexpr std::uint8_t hopper_cooldown = 8;

bool paired(BlockState a, BlockState b) {
    if (!a.is(BlockType::chest) || !b.is(BlockType::chest) || a.facing() != b.facing()) return false;
    const auto ta = static_cast<ChestType>(a.level()), tb = static_cast<ChestType>(b.level());
    return (ta == ChestType::left && tb == ChestType::right) || (ta == ChestType::right && tb == ChestType::left);
}

// One container seen through its parts in combined slot order.
struct Inventory {
    std::vector<BlockPos> parts;
    std::vector<BlockEntity*> entities;

    static Inventory open(RuleContext& ctx, BlockPos pos) {
        Inventory inv;
        for (const auto p : container_parts(ctx, pos)) {
            auto* be = ctx.block_entity(p);
            if (!be) return {};
            inv.parts.push_back(p);
            inv.entities.push_back(be);
        }
        return inv;
    }
    [[nodiscard]] bool valid() const { return !parts.empty(); }
    // First (part, slot), in combined order, that accepts one more `item`.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> insert_slot(std::uint16_t item) const {
        for (std::size_t i = 0; i < entities.size(); ++i)
            for (std::size_t j = 0; j < entities[i]->slots.size(); ++j) {
                const auto& s = entities[i]->slots[j];
                if (s.empty() || (s.item == item && s.count < contracts::max_stack_size)) return std::pair{i, j};
            }
        return std::nullopt;
    }
    void changed(RuleContext& ctx) const {
        for (const auto p : parts) container_changed(ctx, p);
    }
};

void add_one(ItemStack& s, std::uint16_t item) {
    s = s.empty() ? ItemStack{item, 1} : ItemStack{item, static_cast<std::uint8_t>(s.count + 1)};
}

void take_one(ItemStack& s) {
    s.count = static_cast<std::uint8_t>(s.count - 1);
    if (s.count == 0) s = {};
}

// Moves one item from `from` to `to`, trying source slots in combined order.
bool transfer_one(RuleContext& ctx, const Inventory& from, const Inventory& to) {
    for (std::size_t i = 0; i < from.entities.size(); ++i)
        for (std::size_t j = 0; j < from.entities[i]->slots.size(); ++j) {
            auto& source = from.entities[i]->slots[j];
            if (source.empty()) continue;
            const auto target = to.insert_slot(source.item);
            if (!target) continue;
            auto& destination = to.entities[target->first]->slots[target->second];
            const auto before_from = source, before_to = destination;
            const auto item = source.item;
            take_one(source);
            add_one(destination, item);
            ctx.record(TraceKind::inventory_changed, from.parts[i], j, pack_stack(before_from), pack_stack(source));
            ctx.record(TraceKind::inventory_changed, to.parts[target->first], target->second, pack_stack(before_to),
                       pack_stack(destination));
            from.changed(ctx);
            to.changed(ctx);
            return true;
        }
    return false;
}

bool pick_up(RuleContext& ctx, const Inventory& self, BlockPos cell) {
    for (const auto id : ctx.entities_in_cell(cell)) {
        auto* e = ctx.entity(id);
        if (!e || e->stack.empty()) continue;
        const auto before = e->stack;
        bool moved = false;
        while (!e->stack.empty()) {
            const auto target = self.insert_slot(e->stack.item);
            if (!target) break;
            auto& slot = self.entities[target->first]->slots[target->second];
            const auto slot_before = slot;
            add_one(slot, e->stack.item);
            take_one(e->stack);
            ctx.record(TraceKind::inventory_changed, self.parts[target->first], target->second, pack_stack(slot_before),
                       pack_stack(slot));
            moved = true;
        }
        if (!moved) continue;
        const auto where = contracts::block_of(e->position);
        if (e->stack.empty()) {
            ctx.record(TraceKind::entity_removed, where, id.value, static_cast<std::uint64_t>(RemovalReason::picked_up));
            ctx.remove_entity(id);
        } else {
            ctx.record(TraceKind::entity_stack_changed, where, id.value, pack_stack(before), pack_stack(e->stack));
        }
        self.changed(ctx);
        return true;
    }
    return false;
}
} // namespace

std::vector<BlockPos> container_parts(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!is_container(s.type())) return {};
    if (const auto d = chest_partner(s)) {
        const auto other = pos.relative(*d);
        if (paired(s, ctx.block(other))) {
            // The right half's slots come first (observed in target-version traces).
            if (static_cast<ChestType>(s.level()) == ChestType::right) return {pos, other};
            return {other, pos};
        }
    }
    return {pos};
}

std::uint8_t container_signal(RuleContext& ctx, BlockPos pos) {
    std::uint64_t items = 0, slots = 0;
    for (const auto p : container_parts(ctx, pos)) {
        const auto* be = ctx.peek_block_entity(p);
        if (!be) continue;
        slots += be->slots.size();
        for (const auto& s : be->slots) items += s.empty() ? 0 : s.count;
    }
    if (items == 0 || slots == 0) return 0;
    // Every reference item stacks to 64: fullness = items / (64 * slots).
    return static_cast<std::uint8_t>(1 + (14 * items) / (contracts::max_stack_size * slots));
}

void check_chest(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::chest)) return;
    if (const auto d = chest_partner(s)) {
        const auto partner_pos = pos.relative(*d);
        const auto partner = ctx.block(partner_pos);
        if (paired(s, partner)) return;
        if (partner.is(BlockType::chest) && partner.facing() == s.facing()
            && static_cast<ChestType>(partner.level()) == ChestType::single) {
            // A new half claims a single chest of the same facing as its partner.
            const auto type = static_cast<ChestType>(s.level()) == ChestType::left ? ChestType::right : ChestType::left;
            set_block_quiet(ctx, partner_pos, partner.with_level(static_cast<std::uint8_t>(type)));
            return;
        }
        // A half whose partner is gone becomes a single chest.
        set_block_quiet(ctx, pos, s.with_level(static_cast<std::uint8_t>(ChestType::single)));
        return;
    }
    // A single chest next to a half that points at it completes the pair.
    for (auto dir : {contracts::clockwise(s.facing()), contracts::counter_clockwise(s.facing())}) {
        const auto n = ctx.block(pos.relative(dir));
        if (!n.is(BlockType::chest) || n.facing() != s.facing() || chest_partner(n) != contracts::opposite(dir)) continue;
        const auto type = dir == contracts::clockwise(s.facing()) ? ChestType::left : ChestType::right;
        set_block_quiet(ctx, pos, s.with_level(static_cast<std::uint8_t>(type)));
        return;
    }
}

void check_hopper(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::hopper)) return;
    const bool enabled = received_power(ctx, pos) == 0;
    if (enabled != s.flag()) set_block_quiet(ctx, pos, s.with_flag(enabled));
}

void tick_hopper(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::hopper)) return;
    auto* self_entity = ctx.block_entity(pos);
    if (!self_entity) return;
    if (self_entity->cooldown > 0) {
        --self_entity->cooldown;
        if (self_entity->cooldown > 0) return;
    }
    if (!s.flag()) return;
    const auto self = Inventory::open(ctx, pos);
    bool moved = false;
    if (const auto target = Inventory::open(ctx, pos.relative(s.facing())); target.valid())
        moved = transfer_one(ctx, self, target);
    const auto above_pos = pos.relative(Direction::up);
    const auto above = ctx.block(above_pos);
    if (is_container(above.type())) {
        if (const auto source = Inventory::open(ctx, above_pos); source.valid())
            moved = transfer_one(ctx, source, self) || moved;
    } else if (!above.is_unloaded() && !is_solid(above.type())) {
        moved = pick_up(ctx, self, above_pos) || moved;
    }
    if (moved) self_entity->cooldown = hopper_cooldown;
}

} // namespace circuit::gameplay::detail
