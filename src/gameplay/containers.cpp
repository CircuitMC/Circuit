// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <cmath>

namespace circuit::gameplay::detail {
namespace {
using contracts::pack_stack;
using blocks::ChestType;
constexpr std::uint8_t hopper_cooldown = 8;

// A container slot holds at most 64 of an item, and no more than the item stacks.
std::uint8_t slot_limit(const ItemStack& s) { return std::min(contracts::max_stack_size, items::max_stack(s)); }

// Chests pair with chests of their kind (a trapped chest with a trapped chest).
bool paired(BlockState a, BlockState b) {
    if (!a.is(BlockType::chest) || !b.is(BlockType::chest) || a.facing() != b.facing() || a.variant() != b.variant()) return false;
    const auto ta = static_cast<ChestType>(a.level()), tb = static_cast<ChestType>(b.level());
    return (ta == ChestType::left && tb == ChestType::right) || (ta == ChestType::right && tb == ChestType::left);
}

// One container seen through its parts in combined slot order; a furnace
// through the face an item comes in or goes out by (`face`), which picks
// its slots.
struct Inventory {
    std::vector<BlockPos> parts;
    std::vector<BlockEntity*> entities;
    std::optional<Direction> face;
    bool furnace = false;
    bool crafter = false;
    bool brewing = false;
    bool shulker = false;
    const GameData* data = nullptr;

    static Inventory open(RuleContext& ctx, BlockPos pos, std::optional<Direction> face = std::nullopt) {
        Inventory inv;
        for (const auto p : container_parts(ctx, pos)) {
            auto* be = ctx.block_entity(p);
            if (!be) return {};
            inv.parts.push_back(p);
            inv.entities.push_back(be);
        }
        inv.face = face;
        inv.furnace = inv.parts.size() == 1 && ctx.block(pos).is(BlockType::furnace);
        inv.crafter = inv.parts.size() == 1 && ctx.block(pos).is(BlockType::crafter);
        inv.brewing = inv.parts.size() == 1 && ctx.block(pos).is(BlockType::brewing_stand);
        inv.shulker = inv.parts.size() == 1 && ctx.block(pos).is(BlockType::shulker_box);
        inv.data = ctx.data();
        return inv;
    }
    [[nodiscard]] bool valid() const { return !parts.empty(); }
    // The slots of part `i` in the order items go in or out.
    [[nodiscard]] std::vector<std::size_t> slots(std::size_t i) const {
        if (furnace) return furnace_slots(face.value_or(Direction::up));
        if (brewing) return brewing_slots(face.value_or(Direction::up));
        std::vector<std::size_t> out(entities[i]->slots.size());
        for (std::size_t j = 0; j < out.size(); ++j) out[j] = j;
        return out;
    }
    // First (part, slot), in combined order, that accepts one more item of
    // the kind of `stack` (same item, damage and components).
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> insert_slot(const ItemStack& stack) const {
        for (std::size_t i = 0; i < entities.size(); ++i)
            for (const auto j : slots(i)) {
                const auto& s = entities[i]->slots[j];
                if (furnace && !furnace_accepts(*entities[i], j, stack)) continue;
                if (crafter && !crafter_accepts(*entities[i], j, stack)) continue;
                if (brewing && !brewing_accepts(data, *entities[i], j, stack)) continue;
                if (shulker && is_shulker_box_item(stack.item)) continue;
                if (s.empty() || (s.same_kind(stack) && s.count < slot_limit(s))) return std::pair{i, j};
            }
        return std::nullopt;
    }
    // Whether an item may go out of part `i`'s slot `j`.
    [[nodiscard]] bool gives(std::size_t j, const ItemStack& stack) const {
        if (brewing) return brewing_gives(j, stack);
        return !furnace || furnace_gives(j, stack, face.value_or(Direction::down));
    }
    // A furnace's input set anew starts its cooking over.
    void set_into(RuleContext& ctx, std::size_t i, std::size_t j, const ItemStack& before) const {
        if (furnace && j == 0) furnace_input_set(ctx, parts[i], before);
    }
    void changed(RuleContext& ctx) const {
        for (const auto p : parts) container_changed(ctx, p);
    }
};

// Adds one item of the kind of `kind` to `s` (empty, or of that kind).
void add_one(ItemStack& s, const ItemStack& kind) {
    if (s.empty()) {
        s = kind;
        s.count = 1;
    } else {
        s.count = static_cast<std::uint8_t>(s.count + 1);
    }
}

void take_one(ItemStack& s) {
    s.count = static_cast<std::uint8_t>(s.count - 1);
    if (s.count == 0) s = {};
}

bool all_empty(const BlockEntity& be) {
    return std::ranges::all_of(be.slots, [](const ItemStack& s) { return s.empty(); });
}

// Moves one item from `from` to `to`, trying source slots in combined order.
// A hopper that receives into an empty inventory waits 8 game ticks before it
// moves the item on, or 7 when it already ticked in this tick before the
// pushing hopper (hopper_chained): either way it passes the item on 8 ticks later.
bool transfer_one(RuleContext& ctx, const Inventory& from, const Inventory& to, bool from_hopper = false) {
    BlockEntity* receiving_hopper = nullptr;
    if (to.parts.size() == 1 && ctx.block(to.parts[0]).is(BlockType::hopper) && all_empty(*to.entities[0]))
        receiving_hopper = to.entities[0];
    for (std::size_t i = 0; i < from.entities.size(); ++i)
        for (const auto j : from.slots(i)) {
            auto& source = from.entities[i]->slots[j];
            if (source.empty() || !from.gives(j, source)) continue;
            const auto target = to.insert_slot(source);
            if (!target) continue;
            auto& destination = to.entities[target->first]->slots[target->second];
            const auto before_from = source, before_to = destination;
            const auto kind = source;
            take_one(source);
            add_one(destination, kind);
            ctx.record(TraceKind::inventory_changed, from.parts[i], j, pack_stack(before_from), pack_stack(source));
            ctx.record(TraceKind::inventory_changed, to.parts[target->first], target->second, pack_stack(before_to),
                       pack_stack(destination));
            to.set_into(ctx, target->first, target->second, before_to);
            if (receiving_hopper)
                receiving_hopper->cooldown = static_cast<std::uint8_t>(hopper_cooldown - (from_hopper && receiving_hopper->ticked >= ctx.now() ? 1 : 0));
            from.changed(ctx);
            to.changed(ctx);
            return true;
        }
    return false;
}

// The first item of the hopper's that a composter in front takes (through
// its top; composters_items).
bool compost_one(RuleContext& ctx, const Inventory& from, BlockPos composter, Direction face) {
    for (std::size_t i = 0; i < from.entities.size(); ++i)
        for (const auto j : from.slots(i)) {
            auto& source = from.entities[i]->slots[j];
            if (source.empty() || face != Direction::up || !composter_takes(ctx.block(composter), source)) continue;
            const auto before = source;
            take_one(source);
            ctx.record(TraceKind::inventory_changed, from.parts[i], j, pack_stack(before), pack_stack(source));
            compost(ctx, composter, before);
            from.changed(ctx);
            return true;
        }
    return false;
}

// A ready composter's bone meal into the hopper below it, emptying it.
bool take_bone_meal(RuleContext& ctx, const Inventory& to, BlockPos composter) {
    const ItemStack meal{items::bone_meal, 1};
    const auto target = to.insert_slot(meal);
    if (!target) return false;
    auto& slot = to.entities[target->first]->slots[target->second];
    const auto before = slot;
    add_one(slot, meal);
    ctx.record(TraceKind::inventory_changed, to.parts[target->first], target->second, pack_stack(before), pack_stack(slot));
    empty_composter(ctx, composter);
    to.changed(ctx);
    return true;
}

// Moves what fits of an item entity into a container, one item at a time.
// Returns whether anything moved.
bool take_entity(RuleContext& ctx, const Inventory& self, EntityId id) {
    auto* e = ctx.entity(id);
    if (!e || e->stack.empty()) return false;
    const auto before = e->stack;
    bool moved = false;
    while (!e->stack.empty()) {
        const auto target = self.insert_slot(e->stack);
        if (!target) break;
        auto& slot = self.entities[target->first]->slots[target->second];
        const auto slot_before = slot;
        add_one(slot, e->stack);
        take_one(e->stack);
        ctx.record(TraceKind::inventory_changed, self.parts[target->first], target->second, pack_stack(slot_before), pack_stack(slot));
        moved = true;
    }
    if (!moved) return false;
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

// The space a hopper collects items from: the cell above it and its bowl,
// whose floor is 11/16 up (both official servers: items overlapping the cell
// above by 0.025 are taken, 0.005 beside it not; items rest in the bowl at
// 11/16).
constexpr double bowl_floor = 11.0 / 16.0, bowl_wall = 2.0 / 16.0;
bool reaches_hopper(BlockPos pos, const physics::Aabb& box) {
    const double x = pos.x, y = pos.y, z = pos.z;
    const physics::Aabb above{x, y + 1.0, z, x + 1.0, y + 2.0, z + 1.0};
    const physics::Aabb bowl{x + bowl_wall, y + bowl_floor, z + bowl_wall, x + 1.0 - bowl_wall, y + 1.0, z + 1.0 - bowl_wall};
    return box.intersects(above) || box.intersects(bowl);
}

// A hopper collecting the first item entity, by id, that reaches it and of
// which something fits.
bool pick_up(RuleContext& ctx, const Inventory& self, BlockPos pos) {
    const double x = pos.x, y = pos.y, z = pos.z;
    const physics::Aabb area{x, y + bowl_floor, z, x + 1.0, y + 2.0, z + 1.0};
    for (const auto id : entities_intersecting(ctx, area)) {
        const auto* e = ctx.peek_entity(id);
        if (e->kind != EntityKind::item || !reaches_hopper(pos, entity_box(*e))) continue;
        if (take_entity(ctx, self, id)) return true;
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

// Fullness as the target version computes it, in single precision: the sum of
// each slot's count over its limit, divided by the number of slots; 0 when
// empty, else 1 + floor(14 * fullness).
std::uint8_t container_signal(RuleContext& ctx, BlockPos pos) {
    // A crafter's: its slots that hold something or are disabled.
    if (ctx.block(pos).is(BlockType::crafter)) {
        const auto* be = ctx.peek_block_entity(pos);
        std::uint8_t n = 0;
        for (std::size_t k = 0; be && k < be->slots.size(); ++k)
            if (!be->slots[k].empty() || (be->disabled >> k & 1)) ++n;
        return n;
    }
    float fullness = 0.0f;
    std::size_t slots = 0;
    for (const auto p : container_parts(ctx, pos)) {
        const auto* be = ctx.peek_block_entity(p);
        if (!be) continue;
        slots += be->slots.size();
        for (const auto& s : be->slots)
            if (!s.empty()) fullness += static_cast<float>(s.count) / static_cast<float>(slot_limit(s));
    }
    if (slots == 0) return 0;
    fullness /= static_cast<float>(slots);
    if (fullness <= 0.0f) return 0;
    return static_cast<std::uint8_t>(static_cast<int>(std::floor(fullness * 14.0f)) + 1);
}

void check_chest(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::chest)) return;
    if (const auto d = chest_partner(s)) {
        const auto partner_pos = pos.relative(*d);
        const auto partner = ctx.block(partner_pos);
        if (paired(s, partner)) return;
        if (partner.is(BlockType::chest) && partner.facing() == s.facing() && partner.variant() == s.variant()
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
        if (!n.is(BlockType::chest) || n.facing() != s.facing() || n.variant() != s.variant() || chest_partner(n) != contracts::opposite(dir))
            continue;
        const auto type = dir == contracts::clockwise(s.facing()) ? ChestType::left : ChestType::right;
        set_block_quiet(ctx, pos, s.with_level(static_cast<std::uint8_t>(type)));
        return;
    }
}

// Placement of a player's chest (documented behavior of the target version,
// checked with the client harness against the official server):
// - Sneaking and clicking the side of a single chest whose facing is
//   perpendicular to the clicked face joins that chest, taking its facing.
// - Otherwise, without sneaking, the chest joins a single chest of the same
//   facing beside it, clockwise first (seen from above), so it becomes the left
//   half; else counter-clockwise, as the right half.
// The partner turns into the other half through the shape update (check_chest).
BlockState placed_chest(RuleContext& ctx, BlockPos pos, BlockState placed, Direction face, bool sneaking) {
    auto facing = placed.facing();
    auto type = ChestType::single;
    const auto single_facing = [&](Direction d) -> std::optional<Direction> {
        const auto n = ctx.block(pos.relative(d));
        if (n.is(BlockType::chest) && n.variant() == placed.variant() && static_cast<ChestType>(n.level()) == ChestType::single) return n.facing();
        return std::nullopt;
    };
    if (sneaking && contracts::is_horizontal(face)) {
        const auto toward = contracts::opposite(face);
        if (const auto f = single_facing(toward); f && *f != face && *f != toward) {
            facing = *f;
            type = contracts::counter_clockwise(*f) == toward ? ChestType::right : ChestType::left;
        }
    }
    if (type == ChestType::single && !sneaking) {
        if (single_facing(contracts::clockwise(facing)) == facing) type = ChestType::left;
        else if (single_facing(contracts::counter_clockwise(facing)) == facing) type = ChestType::right;
    }
    return blocks::chest(facing, type, placed.variant() == 1);
}

void check_hopper(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::hopper)) return;
    const bool enabled = !has_neighbor_signal(ctx, pos);
    if (enabled != s.flag()) set_block_quiet(ctx, pos, s.with_flag(enabled));
}

void tick_hopper(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::hopper)) return;
    auto* self_entity = ctx.block_entity(pos);
    if (!self_entity) return;
    self_entity->ticked = ctx.now();
    if (self_entity->cooldown > 0) {
        --self_entity->cooldown;
        if (self_entity->cooldown > 0) return;
    }
    if (!s.flag()) return;
    const auto self = Inventory::open(ctx, pos);
    bool moved = false;
    const auto front = pos.relative(s.facing());
    if (ctx.block(front).is(BlockType::composter)) moved = compost_one(ctx, self, front, contracts::opposite(s.facing()));
    else if (const auto target = Inventory::open(ctx, front, contracts::opposite(s.facing())); target.valid())
        moved = transfer_one(ctx, self, target, true);
    const auto above_pos = pos.relative(Direction::up);
    const auto above = ctx.block(above_pos);
    // A composter above gives its bone meal when it is ready; the hopper takes
    // no items through it.
    if (above.is(BlockType::composter)) {
        if (composter_ready(above)) moved = take_bone_meal(ctx, self, above_pos) || moved;
    } else if (is_container(above.type())) {
        if (const auto source = Inventory::open(ctx, above_pos, Direction::down); source.valid())
            moved = transfer_one(ctx, source, self) || moved;
    } else if (!above.is_unloaded() && !is_solid(above)) {
        moved = pick_up(ctx, self, pos) || moved;
    }
    if (moved) self_entity->cooldown = hopper_cooldown;
}

bool insert_one(RuleContext& ctx, BlockPos pos, const ItemStack& stack, std::optional<Direction> face) {
    const auto to = Inventory::open(ctx, pos, face);
    if (!to.valid() || stack.empty()) return false;
    const auto target = to.insert_slot(stack);
    if (!target) return false;
    BlockEntity* receiving_hopper = nullptr;
    if (to.parts.size() == 1 && ctx.block(to.parts[0]).is(BlockType::hopper) && all_empty(*to.entities[0])) receiving_hopper = to.entities[0];
    auto& slot = to.entities[target->first]->slots[target->second];
    const auto before = slot;
    add_one(slot, stack);
    ctx.record(TraceKind::inventory_changed, to.parts[target->first], target->second, pack_stack(before), pack_stack(slot));
    to.set_into(ctx, target->first, target->second, before);
    if (receiving_hopper) receiving_hopper->cooldown = hopper_cooldown;
    to.changed(ctx);
    return true;
}

ItemStack insert_stack(RuleContext& ctx, BlockPos pos, ItemStack stack, std::optional<Direction> face) {
    const auto to = Inventory::open(ctx, pos, face);
    if (!to.valid()) return stack;
    BlockEntity* receiving_hopper = nullptr;
    if (to.parts.size() == 1 && ctx.block(to.parts[0]).is(BlockType::hopper) && all_empty(*to.entities[0])) receiving_hopper = to.entities[0];
    bool moved = false;
    while (!stack.empty()) {
        const auto target = to.insert_slot(stack);
        if (!target) break;
        auto& slot = to.entities[target->first]->slots[target->second];
        const auto before = slot;
        const int room = slot.empty() ? slot_limit(stack) : slot_limit(slot) - slot.count;
        const int n = std::min<int>(room, stack.count);
        if (slot.empty()) {
            slot = stack;
            slot.count = static_cast<std::uint8_t>(n);
        } else {
            slot.count = static_cast<std::uint8_t>(slot.count + n);
        }
        stack.count = static_cast<std::uint8_t>(stack.count - n);
        if (stack.count == 0) stack = {};
        ctx.record(TraceKind::inventory_changed, to.parts[target->first], target->second, pack_stack(before), pack_stack(slot));
        to.set_into(ctx, target->first, target->second, before);
        moved = true;
    }
    if (!moved) return stack;
    if (receiving_hopper) receiving_hopper->cooldown = hopper_cooldown;
    to.changed(ctx);
    return stack;
}

bool hopper_takes_item(RuleContext& ctx, BlockPos pos, EntityId item) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::hopper) || !s.flag()) return false;
    auto* self_entity = ctx.block_entity(pos);
    if (!self_entity || self_entity->cooldown > 0) return false;
    const auto* e = ctx.peek_entity(item);
    if (!e || e->kind != EntityKind::item || !reaches_hopper(pos, entity_box(*e))) return false;
    const auto self = Inventory::open(ctx, pos);
    if (!self.valid() || !take_entity(ctx, self, item)) return false;
    self_entity->cooldown = hopper_cooldown;
    return ctx.peek_entity(item) == nullptr;
}

} // namespace circuit::gameplay::detail
