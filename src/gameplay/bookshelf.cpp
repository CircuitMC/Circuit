// SPDX-License-Identifier: AGPL-3.0-only
// Chiseled bookshelves as both official servers run them (bookshelves;
// client/bookshelf; minecraft.wiki "Chiseled Bookshelf"):
// - Six slots of one book each (the item tag bookshelf_books), shown by the
//   block's state; the comparator reads the last slot touched, plus one.
// - A player's click on the front picks a slot: the top row 0-2 and the
//   bottom row 3-5, each from the left as the player sees the front. A book
//   goes into an empty slot (used up outside creative mode); a full slot's
//   book comes out into the player's inventory, whatever the hand holds; an
//   empty slot with anything else does nothing of its own.
// - Hoppers and droppers put books into the first empty slot; hoppers below
//   take from the first full one.
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <algorithm>
#include <array>

namespace circuit::gameplay::detail {
namespace {
// The bookshelf_books tag of the official data packs, and the same items
// from minecraft.wiki without them.
const std::vector<std::uint16_t>& fallback_books() {
    static const auto books = [] {
        std::vector<std::uint16_t> out;
        for (const auto* name : {"minecraft:book", "minecraft:written_book", "minecraft:writable_book", "minecraft:enchanted_book", "minecraft:knowledge_book"})
            if (const auto id = items::by_name(name)) out.push_back(*id);
        std::ranges::sort(out);
        return out;
    }();
    return books;
}
} // namespace

bool is_bookshelf_book(RuleContext& ctx, std::uint16_t item) {
    const auto* data = ctx.data();
    const auto& books = data && !data->bookshelf_books.empty() ? data->bookshelf_books : fallback_books();
    return std::ranges::binary_search(books, item);
}

void bookshelf_slot_changed(RuleContext& ctx, BlockPos pos, std::size_t slot) {
    auto* be = ctx.block_entity(pos);
    const auto s = ctx.block(pos);
    if (!be || be->slots.size() != 6 || !s.is(BlockType::chiseled_bookshelf)) return;
    be->signal = static_cast<std::uint8_t>(slot + 1);
    std::uint8_t occupied = 0;
    for (std::size_t k = 0; k < 6; ++k)
        if (!be->slots[k].empty()) occupied = static_cast<std::uint8_t>(occupied | 1u << k);
    set_block(ctx, pos, s.with_variant(occupied));
}

std::optional<std::size_t> bookshelf_slot_at(BlockState s, Direction face, float x, float y, float z) {
    if (face != s.facing()) return std::nullopt;
    // Across the front from the left as a player facing it sees it.
    float across = 0.0f;
    switch (s.facing()) {
    case Direction::south: across = x; break;
    case Direction::north: across = 1.0f - x; break;
    case Direction::east: across = 1.0f - z; break;
    case Direction::west: across = z; break;
    default: return std::nullopt;
    }
    const auto column = static_cast<std::size_t>(std::clamp(static_cast<int>(across * 3.0f), 0, 2));
    return (y >= 0.5f ? 0 : 3) + column;
}

bool use_bookshelf(RuleContext& ctx, PlayerInventory& inv, BlockPos pos, std::size_t slot, std::uint8_t hand, bool survival) {
    auto* be = ctx.block_entity(pos);
    if (!be || be->slots.size() != 6) return false;
    auto& held = inv.slots[hand];
    if (be->slots[slot].empty()) {
        // Only a book goes in; anything else is the item's own use.
        if (held.empty() || !is_bookshelf_book(ctx, held.item)) return false;
        auto book = held;
        book.count = 1;
        if (survival) {
            if (--held.count == 0) held = {};
            record_slot(ctx, inv, hand);
        }
        be->slots[slot] = book;
        bookshelf_slot_changed(ctx, pos, slot);
        container_changed(ctx, pos);
        return true;
    }
    // A full slot's book comes out into the inventory (the hand first), or
    // drops at the player's feet without room.
    const auto book = be->slots[slot];
    be->slots[slot] = {};
    bookshelf_slot_changed(ctx, pos, slot);
    container_changed(ctx, pos);
    const auto left = add_to_player(ctx, inv, book, hand);
    if (!left.empty())
        if (const auto presence = ctx.presence_of(inv.player))
            spawn_dropped(ctx, contracts::block_of(presence->position), presence->position, {}, left, 40);
    return true;
}

} // namespace circuit::gameplay::detail
