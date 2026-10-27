// SPDX-License-Identifier: AGPL-3.0-only
// Lecterns: a book put on and taken, its pages turned by a window's buttons,
// the pulse a page turn gives and the signal comparators read, as both
// official servers run them (lectern_pages; client/lectern; minecraft.wiki
// "Lectern").
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <algorithm>
#include <cmath>

namespace circuit::gameplay::detail {
namespace {
// A page turn powers the lectern until its block tick this long after.
constexpr std::uint32_t pulse_ticks = 2;

std::int32_t pages_of(RuleContext& ctx, const ItemStack& book) {
    const auto* data = ctx.data();
    return data && data->book_pages && !book.empty() ? data->book_pages(book) : 0;
}

// The lectern's state set (with full updates), then the block below it told.
void set_lectern(RuleContext& ctx, BlockPos pos, BlockState next) {
    set_block(ctx, pos, next);
    notify_neighbors(ctx, pos.relative(Direction::down), std::nullopt, next);
}

// A page turn: the page, the comparators around, then the pulse (powered,
// with its block tick to end it).
void turn_to(RuleContext& ctx, BlockPos pos, std::int32_t page) {
    auto* be = ctx.block_entity(pos);
    if (!be || be->slots.empty() || page == be->cook_time) return;
    be->cook_time = page;
    container_changed(ctx, pos);
    // Powered already, it stays so; the block below is told either way.
    const auto s = ctx.block(pos);
    if (!s.flag()) set_block(ctx, pos, s.with_flag(true));
    notify_neighbors(ctx, pos.relative(Direction::down), std::nullopt, ctx.block(pos));
    schedule(ctx, TickList::block, pos, BlockType::lectern, pulse_ticks, 0);
}
} // namespace

bool lectern_takes(RuleContext& ctx, const ItemStack& held) {
    const auto* data = ctx.data();
    return !held.empty() && data && std::ranges::binary_search(data->lectern_books, held.item);
}

void put_lectern_book(RuleContext& ctx, BlockPos pos, ItemStack book) {
    auto* be = ctx.block_entity(pos);
    if (!be) return;
    book.count = 1;
    be->slots = {book};
    be->cook_time = 0;
    container_changed(ctx, pos);
    const auto s = ctx.block(pos);
    set_lectern(ctx, pos, s.with_extra(1).with_flag(false));
}

std::uint8_t lectern_signal(RuleContext& ctx, BlockPos pos) {
    // 1 more than 14 times the page's place among them (in single precision),
    // 15 for a book of one page; nothing without a book.
    const auto s = ctx.block(pos);
    const auto* be = ctx.peek_block_entity(pos);
    if (!s.is(BlockType::lectern) || s.extra() == 0 || !be || be->slots.empty()) return 0;
    const auto pages = pages_of(ctx, be->slots[0]);
    const float f = pages > 1 ? static_cast<float>(be->cook_time) / (static_cast<float>(pages) - 1.0f) : 1.0f;
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::floor(f * 14.0f)) + 1, 0, 15));
}

void press_lectern_button(RuleContext& ctx, PlayerInventory& inv, BlockPos pos, std::int32_t button) {
    auto* be = ctx.block_entity(pos);
    if (!be || be->slots.empty()) return;
    const auto pages = pages_of(ctx, be->slots[0]);
    const auto page = be->cook_time;
    if (button == 1) {
        if (page > 0) turn_to(ctx, pos, page - 1);
    } else if (button == 2) {
        if (page < pages - 1) turn_to(ctx, pos, page + 1);
    } else if (button == 3) {
        // The book into the player's inventory, or thrown when it has no
        // room.
        const auto book = take_lectern_book(ctx, pos);
        const auto left = add_to_player(ctx, inv, book, contracts::hotbar_first);
        if (!left.empty())
            if (const auto presence = ctx.presence_of(inv.player))
                spawn_dropped(ctx, contracts::block_of(presence->position), presence->position, {}, left, 40);
    } else if (button >= 100) {
        // A page by its number, within the book.
        turn_to(ctx, pos, std::clamp(button - 100, 0, std::max(pages - 1, 0)));
    }
}

ItemStack take_lectern_book(RuleContext& ctx, BlockPos pos) {
    // The lectern without a book and unpowered, then the comparators told.
    auto* be = ctx.block_entity(pos);
    if (!be || be->slots.empty()) return {};
    const auto book = be->slots[0];
    be->slots.clear();
    be->cook_time = 0;
    set_lectern(ctx, pos, ctx.block(pos).with_extra(0).with_flag(false));
    container_changed(ctx, pos);
    return book;
}

void tick_lectern(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.flag()) set_lectern(ctx, pos, s.with_flag(false));
}

void drop_lectern_book(RuleContext& ctx, BlockPos pos, BlockState lectern, const ItemStack& book) {
    // On top, a quarter of a block toward its front, launched as every new
    // item is (from the level random), collectable after 10 ticks.
    if (book.empty()) return;
    const auto front = pos.relative(lectern.facing());
    const contracts::Vec3 at{pos.x + 0.5 + (front.x - pos.x) * 0.25, pos.y + 1.0, pos.z + 0.5 + (front.z - pos.z) * 0.25};
    const double vx = ctx.random_double() * 0.2 - 0.1;
    const double vz = ctx.random_double() * 0.2 - 0.1;
    spawn_dropped(ctx, contracts::block_of(at), at, {vx, 0.2, vz}, book, 10);
}

} // namespace circuit::gameplay::detail
