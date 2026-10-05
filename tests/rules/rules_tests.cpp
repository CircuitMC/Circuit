// SPDX-License-Identifier: AGPL-3.0-only
// Reference-rule fixtures with expectations derived by hand from the rule
// definitions (not from running the runtime). They check the rules and the
// oracle; vanilla compatibility is not established by these values.
#include "../oracle/oracle.hpp"
#include "../support/check.hpp"
#include "../support/scenario.hpp"
#include <circuit/gameplay/rules.hpp>
#include <format>
#include <set>

using namespace circuit::test;
using circuit::oracle::Oracle;
using circuit::test::require;
using D = Direction;

namespace {
const VersionBundle& bundle() { return **BundleRegistry::select_for_simulation("java-1.21.8"); }

struct Sim {
    explicit Sim(const VersionBundle& b = bundle(), std::uint64_t seed = 7, std::uint8_t radius = 2) : oracle(b, seed) {
        tick({ticket(1, {0, 0}, radius)});
    }
    void tick(std::vector<CommandBody> bodies = {}) {
        oracle.advance(admit(std::move(bodies)));
        last = oracle.trace();
    }
    void until(Tick t) {
        while (oracle.time() < t) tick();
    }
    BlockState at(BlockPos p) const { return state_at(oracle.dump(), p); }
    Oracle oracle;
    Admissions admit;
    std::vector<TraceRecord> last;
};

std::uint8_t wire_level(const Sim& s, BlockPos p) {
    const auto st = s.at(p);
    require(st.is(BlockType::redstone_wire), "expected wire");
    return st.level();
}
} // namespace

int main() {
    Suite suite("rules_tests");

    suite.run("block state registry", [] {
        require(BlockState::decode(blocks::repeater(D::east, 4, true).raw()).has_value(), "valid repeater");
        require(!BlockState::decode(blocks::repeater(D::up).raw()).has_value(), "repeater must be horizontal");
        require(!BlockState::decode(blocks::hopper(D::up).raw()).has_value(), "hopper cannot face up");
        require(!BlockState::decode(blocks::stone().with_flag(true).raw()).has_value(), "stone has no flag");
        require(!BlockState::decode(0xFFFFFFFFu).has_value(), "stray bits");
        require(!BlockState::decode(static_cast<std::uint32_t>(BlockType::count_)).has_value(), "unknown type");
        require(!BlockState::decode(blocks::water_falling().with_level(3).raw()).has_value(), "falling water has level 0");
        require(blocks::repeater(D::west, 3).delay() == 3 && blocks::wire(9).level() == 9, "accessors");
    });

    suite.run("vertical slice: line and repeater across a chunk edge", [] {
        Sim s;
        // Lever at x=13; dust x=14..17 crosses from chunk 0 into chunk 1 at x=16.
        s.tick({place({13, 0, 0}, blocks::lever()), place({14, 0, 0}, blocks::wire()), place({15, 0, 0}, blocks::wire()),
                place({16, 0, 0}, blocks::wire()), place({17, 0, 0}, blocks::wire()),
                place({18, 0, 0}, blocks::repeater(D::east, 1)), place({19, 0, 0}, blocks::wire()),
                place({20, 0, 0}, blocks::lamp())});
        require(s.oracle.dump().ticks.empty(), "unpowered setup schedules nothing");
        s.tick({use_lever({13, 0, 0})}); // tick 3
        require(wire_level(s, {14, 0, 0}) == 15 && wire_level(s, {15, 0, 0}) == 14 && wire_level(s, {16, 0, 0}) == 13
                    && wire_level(s, {17, 0, 0}) == 12, "dust decays by one per block across the edge");
        const auto scheduled = of_kind(s.last, TraceKind::tick_scheduled);
        require(scheduled.size() == 1 && scheduled[0].pos == BlockPos{18, 0, 0} && scheduled[0].b == 5 && scheduled[0].c == 127,
                "repeater schedules delay 2 at priority -1");
        s.tick(); // tick 4
        require(!s.at({18, 0, 0}).flag(), "repeater waits for its delay");
        s.tick(); // tick 5
        require(s.at({18, 0, 0}).flag() && wire_level(s, {19, 0, 0}) == 15 && s.at({20, 0, 0}).flag(),
                "repeater output powers dust and lamp on tick 5");
        s.until(6);
        s.tick({use_lever({13, 0, 0})}); // tick 7
        for (int x = 14; x <= 17; ++x) require(wire_level(s, {x, 0, 0}) == 0, "dust depowers completely");
        require(s.at({18, 0, 0}).flag() && s.at({20, 0, 0}).flag(), "repeater holds until tick 9");
        s.until(9);
        require(!s.at({18, 0, 0}).flag() && wire_level(s, {19, 0, 0}) == 0 && s.at({20, 0, 0}).flag(),
                "repeater off on tick 9; lamp delays");
        s.until(12);
        require(s.at({20, 0, 0}).flag(), "lamp still lit on tick 12");
        s.tick(); // tick 13
        require(!s.at({20, 0, 0}).flag(), "lamp off four ticks after losing power");
    });

    suite.run("immediate chain returns across the edge in one tick", [] {
        Sim s;
        // Lever and lamp in chunk 0; the dust leaves into chunk 1 and comes back.
        s.tick({place({14, 0, 0}, blocks::lever()), place({15, 0, 0}, blocks::wire()), place({16, 0, 0}, blocks::wire()),
                place({16, 0, 1}, blocks::wire()), place({16, 0, 2}, blocks::wire()), place({15, 0, 2}, blocks::wire()),
                place({14, 0, 2}, blocks::lamp())});
        s.tick({use_lever({14, 0, 0})});
        require(wire_level(s, {15, 0, 2}) == 11 && s.at({14, 0, 2}).flag(), "lamp lit in the same tick after two crossings");
        std::set<std::int32_t> chunks_seen;
        for (const auto& r : of_kind(s.last, TraceKind::block_changed)) chunks_seen.insert(chunk_of(r.pos).x);
        require(chunks_seen == std::set<std::int32_t>{0, 1}, "changes in both chunks");
        require(of_kind(s.last, TraceKind::tick_scheduled).empty(), "no deferral to a later tick");
    });

    suite.run("observer pulse is extended by a repeater", [] {
        Sim s;
        s.tick({place({0, 0, 3}, blocks::stone()), place({0, 0, 4}, blocks::observer(D::north)),
                place({0, 0, 5}, blocks::repeater(D::south, 4)), place({0, 0, 6}, blocks::lamp())});
        s.tick({place({0, 0, 3}, blocks::glass())}); // tick 3: observed block changes
        s.until(4);
        require(!s.at({0, 0, 4}).flag(), "observer delay");
        s.tick(); // 5
        require(s.at({0, 0, 4}).flag(), "observer pulse starts on tick 5");
        s.until(7);
        require(!s.at({0, 0, 4}).flag(), "two-tick observer pulse");
        s.until(12);
        require(!s.at({0, 0, 5}).flag() && !s.at({0, 0, 6}).flag(), "repeater delay 8");
        s.tick(); // 13
        require(s.at({0, 0, 5}).flag() && s.at({0, 0, 6}).flag(), "repeater and lamp on at 13");
        s.until(20);
        require(s.at({0, 0, 5}).flag(), "pulse extended to the repeater delay");
        s.tick(); // 21
        require(!s.at({0, 0, 5}).flag() && s.at({0, 0, 6}).flag(), "repeater off at 21");
        s.until(25);
        require(!s.at({0, 0, 6}).flag(), "lamp off at 25");
    });

    suite.run("scheduled tick priority before sub-order", [] {
        Sim s;
        s.tick({place({-1, 0, 0}, blocks::lamp()), place({0, 0, -1}, blocks::repeater(D::north, 2)),
                place({0, 0, 1}, blocks::repeater(D::south, 2)), place({0, 0, 0}, blocks::lever())});
        s.tick({use_lever({0, 0, 0})}); // 3: lamp lit; repeaters due 7
        require(s.at({-1, 0, 0}).flag(), "lamp lights immediately");
        s.until(7);
        const auto first = of_kind(s.last, TraceKind::tick_executed);
        require(first.size() == 2 && first[0].pos == BlockPos{0, 0, -1} && first[1].pos == BlockPos{0, 0, 1},
                "equal priority follows scheduling order");
        s.tick({use_lever({0, 0, 0})}); // 8: lamp (prio 0) scheduled before both repeaters (prio -1)
        s.until(12);
        const auto order = of_kind(s.last, TraceKind::tick_executed);
        require(order.size() == 3 && order[0].pos == BlockPos{0, 0, -1} && order[1].pos == BlockPos{0, 0, 1}
                    && order[2].pos == BlockPos{-1, 0, 0}, "priority -1 drains before an earlier priority-0 tick");
        require(order[0].key.root == 0 && order[2].key.root == 2, "roots follow drain order");
        require(!s.at({-1, 0, 0}).flag() && !s.at({0, 0, 1}).flag(), "all off on tick 12");
    });

    suite.run("neighbor chain limit truncates", [] {
        auto limited = bundle();
        limited.limits.max_chained_neighbor_updates = 12;
        Sim s(limited);
        std::vector<CommandBody> build{place({0, 0, 0}, blocks::lever())};
        for (int x = 1; x <= 10; ++x) build.push_back(place({x, 0, 0}, blocks::wire()));
        s.tick(build);
        require(of_kind(s.last, TraceKind::chain_truncated).empty(), "short placement chains are complete");
        s.tick({use_lever({0, 0, 0})});
        for (int x = 1; x <= 6; ++x) require(wire_level(s, {x, 0, 0}) == 16 - x, "updated part of the line");
        for (int x = 7; x <= 10; ++x) require(wire_level(s, {x, 0, 0}) == 0, "discarded remainder");
        const auto cut = of_kind(s.last, TraceKind::chain_truncated);
        require(cut.size() == 1 && cut[0].pos == BlockPos{6, 0, 0} && cut[0].a == 12, "truncation point is recorded");
    });

    suite.run("piston pushes a line across a chunk edge", [] {
        Sim s;
        s.tick({place({14, 0, 0}, blocks::stone()), place({15, 0, 0}, blocks::stone()), place({16, 0, 0}, blocks::stone()),
                place({13, 0, 0}, blocks::piston(D::east)), place({13, 0, 1}, blocks::lever())});
        s.tick({use_lever({13, 0, 1})}); // 3
        require(s.at({13, 0, 0}).flag(), "extended");
        require(s.at({14, 0, 0}).is(BlockType::piston_head) && s.at({14, 0, 0}).facing() == D::east, "head");
        for (int x = 15; x <= 17; ++x) require(s.at({x, 0, 0}).is(BlockType::stone), "line moved by one");
        s.tick({use_lever({13, 0, 1})}); // 4: re-check tick then retract
        require(!s.at({13, 0, 0}).flag() && s.at({14, 0, 0}).is_air(), "retracted");
        require(s.at({15, 0, 0}).is(BlockType::stone) && s.at({17, 0, 0}).is(BlockType::stone), "regular piston leaves blocks");
    });

    suite.run("piston push limit and immovable blocks", [] {
        for (int length : {12, 13}) {
            Sim s;
            std::vector<CommandBody> build;
            for (int x = 1; x <= length; ++x) build.push_back(place({x, 0, 0}, blocks::stone()));
            build.push_back(place({0, 0, 0}, blocks::piston(D::east)));
            s.tick(build);
            s.tick({place({0, 0, 1}, blocks::redstone_block())});
            const bool moved = s.at({0, 0, 0}).flag();
            require(moved == (length == 12), std::format("push limit with {} blocks", length));
            require(of_kind(s.last, TraceKind::piston_blocked).size() == (length == 12 ? 0u : 1u), "blocked record");
            s.until(6);
            require(s.at({0, 0, 0}).flag() == (length == 12), "blocked piston does not retry without an update");
        }
        Sim s;
        s.tick({place({1, 0, 0}, blocks::stone()), place({2, 0, 0}, blocks::obsidian()),
                place({0, 0, 0}, blocks::piston(D::east))});
        s.tick({place({0, 0, 1}, blocks::redstone_block())});
        require(!s.at({0, 0, 0}).flag() && s.at({1, 0, 0}).is(BlockType::stone), "obsidian stops the push");
    });

    suite.run("sticky piston pulls; slime attaches neighbors", [] {
        Sim s;
        s.tick({place({1, 0, 0}, blocks::stone()), place({0, 0, 0}, blocks::piston(D::east, true))});
        s.tick({place({0, 0, 1}, blocks::redstone_block())}); // 3: extend
        require(s.at({2, 0, 0}).is(BlockType::stone) && s.at({1, 0, 0}).is(BlockType::piston_head) && s.at({1, 0, 0}).flag(),
                "sticky head");
        s.tick(); // 4: re-check tick, still powered
        s.tick({place({0, 0, 1}, blocks::air())}); // 5: retract and pull
        require(!s.at({0, 0, 0}).flag() && s.at({1, 0, 0}).is(BlockType::stone) && s.at({2, 0, 0}).is_air(), "pulled back");

        Sim t;
        t.tick({place({1, 0, 0}, blocks::slime()), place({1, 1, 0}, blocks::stone()), place({1, 0, 1}, blocks::stone()),
                place({1, 0, -1}, blocks::obsidian()), place({0, 0, 0}, blocks::piston(D::east, true))});
        t.tick({place({0, 0, 1}, blocks::redstone_block())});
        require(t.at({2, 0, 0}).is(BlockType::slime_block) && t.at({2, 1, 0}).is(BlockType::stone)
                    && t.at({2, 0, 1}).is(BlockType::stone), "slime structure moves together");
        require(t.at({1, 0, -1}).is(BlockType::obsidian) && t.at({1, 1, 0}).is_air(), "immovable neighbor stays");
    });

    suite.run("quasi-connectivity needs an update", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::piston(D::north))});
        s.tick({place({1, 1, 0}, blocks::lever(true))}); // powers the cell above the piston only
        require(!s.at({0, 0, 0}).flag(), "no update reached the piston");
        s.tick({place({-1, 0, 0}, blocks::stone())}); // neighbor update
        require(s.at({0, 0, 0}).flag() && s.at({0, 0, -1}).is(BlockType::piston_head), "updated piston extends");
    });

    suite.run("hoppers compete for the last free space", [] {
        Sim s;
        std::vector<CommandBody> build{place({16, 0, 0}, blocks::chest())};
        for (std::uint8_t i = 0; i < 26; ++i) build.push_back(insert({16, 0, 0}, i, items::stone, 64));
        build.push_back(insert({16, 0, 0}, 26, items::stone, 63));
        build.push_back(place({15, 0, 0}, blocks::hopper(D::east)));
        build.push_back(insert({15, 0, 0}, 0, items::stone, 1));
        build.push_back(place({17, 0, 0}, blocks::hopper(D::west)));
        build.push_back(insert({17, 0, 0}, 0, items::stone, 1));
        s.tick(build);
        const auto d = s.oracle.dump();
        require(block_entity_at(d, {16, 0, 0})->slots[26].count == 64, "first hopper fills the slot");
        require(block_entity_at(d, {15, 0, 0})->slots[0].empty() && block_entity_at(d, {15, 0, 0})->cooldown == 8, "winner");
        require(block_entity_at(d, {17, 0, 0})->slots[0].count == 1 && block_entity_at(d, {17, 0, 0})->cooldown == 0, "loser");
        require(count_items(d, items::stone) == 26u * 64u + 63u + 2u, "items conserved");
    });

    suite.run("hopper transfer timing and locking", [] {
        for (bool locked : {false, true}) {
            Sim s;
            std::vector<CommandBody> build{place({0, 1, 0}, blocks::chest()), insert({0, 1, 0}, 0, items::iron_ingot, 3),
                                           place({0, 0, 0}, blocks::hopper(D::east)), place({1, 0, 0}, blocks::chest())};
            if (locked) build.push_back(place({-1, 0, 0}, blocks::lever(true)));
            s.tick(build); // tick 2
            const auto counts = [&] {
                const auto d = s.oracle.dump();
                return std::array{static_cast<int>(block_entity_at(d, {0, 1, 0})->slots[0].count),
                                  static_cast<int>(block_entity_at(d, {0, 0, 0})->slots[0].count),
                                  static_cast<int>(block_entity_at(d, {1, 0, 0})->slots[0].count)};
            };
            if (locked) {
                require(!s.at({0, 0, 0}).flag(), "powered hopper is disabled");
                s.until(30);
                require(counts() == std::array{3, 0, 0}, "locked hopper moves nothing");
                s.tick({use_lever({-1, 0, 0})});
                require(counts() == std::array{2, 1, 0}, "unlocked hopper pulls immediately");
                continue;
            }
            require(counts() == std::array{2, 1, 0}, "tick 2: pull");
            s.until(9);
            require(counts() == std::array{2, 1, 0}, "cooldown of 8");
            s.tick();
            require(counts() == std::array{1, 1, 1}, "tick 10: push and pull");
            s.until(18);
            require(counts() == std::array{0, 1, 2}, "tick 18");
            s.until(26);
            require(counts() == std::array{0, 0, 3}, "tick 26");
        }
    });

    suite.run("hopper picks up a falling item", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::hopper(D::down)),
                SpawnItem{{2048, 14336, 2048}, {}, {items::redstone, 5}}}); // tick 2, item at y=3.5
        // Independent expectation of the documented motion rule.
        std::int64_t y = 14336, vy = 0;
        Tick pickup = 0;
        for (Tick t = 2; t < 100 && !pickup; ++t) {
            vy = std::max<std::int64_t>(vy - 160, -4095);
            auto ny = y + vy;
            if (floor_block(ny) == 0) { ny = fixed_one; vy = 0; }
            y = ny;
            vy = vy * 98 / 100;
            if (floor_block(y) == 1) pickup = t;
        }
        require(pickup > 2, "expectation computed");
        s.until(pickup - 1);
        require(s.oracle.dump().entities.size() == 1, "still falling");
        s.tick();
        const auto d = s.oracle.dump();
        require(d.entities.empty() && block_entity_at(d, {0, 0, 0})->slots[0] == ItemStack{items::redstone, 5}, "picked up");
        const auto removed = of_kind(s.last, TraceKind::entity_removed);
        require(removed.size() == 1 && removed[0].b == static_cast<std::uint64_t>(RemovalReason::picked_up), "reason");
    });

    suite.run("water spreads one block per five ticks and drains", [] {
        Sim s;
        std::vector<CommandBody> build{place({-1, 1, 0}, blocks::glass())};
        for (int x = 0; x <= 9; ++x) {
            build.push_back(place({x, 0, 0}, blocks::stone()));
            build.push_back(place({x, 1, -1}, blocks::glass()));
            build.push_back(place({x, 1, 1}, blocks::glass()));
        }
        s.tick(build);
        s.tick({place({0, 1, 0}, blocks::water_source())}); // tick 3
        for (int n = 1; n <= 7; ++n) {
            s.until(3 + 5 * static_cast<Tick>(n) - 1);
            require(s.at({n, 1, 0}).is_air(), std::format("x={} not reached before tick {}", n, 3 + 5 * n));
            s.tick();
            require(s.at({n, 1, 0}) == blocks::water_flowing(static_cast<std::uint8_t>(n)), std::format("x={} level", n));
        }
        s.until(60);
        require(s.at({8, 1, 0}).is_air(), "distance 7 is the limit");
        s.tick({place({0, 1, 0}, blocks::air())});
        s.until(300);
        for (int x = 0; x <= 9; ++x) require(s.at({x, 1, 0}).is_air(), "channel drained");
    });

    suite.run("random ticks use per-section streams", [] {
        const auto run = [](std::uint64_t seed, bool neighbor_activity) {
            Sim s(bundle(), seed, 1);
            std::vector<CommandBody> build;
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z) build.push_back(place({x, 0, z}, blocks::wheat()));
            if (neighbor_activity)
                for (int x = 16; x < 32; ++x) build.push_back(place({x, 0, 3}, blocks::wheat()));
            s.tick(build);
            s.until(150);
            std::vector<std::uint8_t> ages;
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z) ages.push_back(s.at({x, 0, z}).level());
            return ages;
        };
        const auto base = run(11, false);
        unsigned grown = 0;
        for (auto a : base) grown += a;
        require(grown > 0, "wheat grows");
        require(run(11, true) == base, "another chunk's draws do not shift this chunk's stream");
        require(run(12, false) != base, "seed changes the outcome");
    });

    suite.run("entities migrate once per tick and fall out of the world", [] {
        Sim s;
        std::vector<CommandBody> build;
        for (int x = 10; x <= 20; ++x) build.push_back(place({x, 0, 0}, blocks::stone()));
        build.push_back(SpawnItem{at_block(15.5, 1.0, 0.5), {1200, 0, 0}, {items::stone, 1}});
        build.push_back(SpawnItem{at_block(5.5, 10.0, 5.5), {}, {items::wheat, 2}});
        s.tick(build); // tick 2
        std::vector<TraceRecord> moved;
        for (int i = 0; i < 40; ++i) {
            s.tick();
            for (const auto& r : of_kind(s.last, TraceKind::entity_changed_chunk)) moved.push_back(r);
        }
        const auto d = s.oracle.dump();
        const auto* e = entity_in(d, EntityId{1});
        require(e && e->age == 41 && e->last_tick == s.oracle.time(), "advanced exactly once per tick");
        require(block_of(e->position).x == 16 && e->on_ground, "came to rest in chunk 1");
        require(moved.size() == 1 && moved[0].a == 1 && moved[0].c == pack_chunk({1, 0}), "one chunk change");
        s.until(400);
        require(!entity_in(s.oracle.dump(), EntityId{2}), "fell out of the world");
    });

    suite.run("tickets, generations and deferred ticks", [] {
        Sim s(bundle(), 7, 0); // tick 1: chunk (0,0) ticking, ring border
        s.tick({place({0, 0, 0}, blocks::stone()), place({16, 0, 0}, blocks::stone()), place({40, 0, 0}, blocks::stone()),
                place({1, 0, 5}, blocks::lever(true)), place({2, 0, 5}, blocks::lamp()),
                place({17, 0, 5}, blocks::lever(true)), place({18, 0, 5}, blocks::lamp())});
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == 1 && rejected[0].b == static_cast<std::uint64_t>(ErrorCode::unloaded_chunk), "unloaded write");
        require(s.at({2, 0, 5}).flag() && s.at({18, 0, 5}).flag(), "lamps lit in ticking and border chunks");
        s.tick({use_lever({1, 0, 5}), use_lever({17, 0, 5})}); // 3: lamp ticks due 7
        s.tick({RemoveTicket{1}}); // 4
        require(of_kind(s.last, TraceKind::chunk_unloaded).size() == 9, "nine chunks unloaded");
        s.until(9);
        auto d = s.oracle.dump();
        require(d.ticks.size() == 2 && state_at(d, {2, 0, 5}).flag(), "pending ticks kept while unloaded");
        s.tick({ticket(2, {0, 0}, 0)}); // 10: reload, overdue tick drains
        d = s.oracle.dump();
        require(!state_at(d, {2, 0, 5}).flag() && state_at(d, {18, 0, 5}).flag(), "ticking chunk drains; border waits");
        require(d.chunks.size() == 9 && d.chunks[4].generation == 1, "reload increments generation");
        s.tick({SetBlock{{3, 0, 0}, blocks::stone().raw(), Generation{0}}, SetBlock{{4, 0, 0}, blocks::stone().raw(), Generation{1}}});
        const auto r = of_kind(s.last, TraceKind::command_rejected);
        require(r.size() == 1 && r[0].b == static_cast<std::uint64_t>(ErrorCode::stale_generation), "stale generation");
        require(s.at({4, 0, 0}).is(BlockType::stone), "current generation accepted");
        s.tick({ticket(3, {1, 0}, 0)});
        require(!s.at({18, 0, 5}).flag(), "border chunk tick drains once ticking");
    });

    suite.run("command validation", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::hopper()), insert({0, 0, 0}, 0, items::stone, 60)});
        const auto piston_head = BlockState::of(BlockType::piston_head).raw();
        const auto extended = blocks::piston(D::up).with_flag(true).raw();
        s.tick({place({0, 1000, 0}, blocks::stone()), place({200, 0, 0}, blocks::stone()), SetBlock{{1, 0, 0}, 0xFFFFFFFFu, {}},
                SetBlock{{1, 0, 0}, piston_head, {}}, SetBlock{{1, 0, 0}, extended, {}}, use_lever({0, 0, 0}),
                insert({5, 5, 5}, 0, items::stone, 1), insert({0, 0, 0}, 5, items::stone, 1),
                insert({0, 0, 0}, 1, 99, 1), insert({0, 0, 0}, 0, items::wheat, 1), insert({0, 0, 0}, 0, items::stone, 5),
                SpawnItem{at_block(0.5, 5, 0.5), {5000, 0, 0}, {items::stone, 1}}, RemoveEntity{{999}},
                ticket(1, {0, 0}, 1), RemoveTicket{77}, ticket(9, {0, 0}, 9)});
        const std::vector expected{ErrorCode::out_of_bounds, ErrorCode::unloaded_chunk, ErrorCode::invalid_block_state,
                                   ErrorCode::invalid_block_state, ErrorCode::invalid_block_state, ErrorCode::not_a_lever,
                                   ErrorCode::not_a_container, ErrorCode::invalid_slot, ErrorCode::invalid_argument,
                                   ErrorCode::item_mismatch, ErrorCode::stack_overflow, ErrorCode::invalid_argument,
                                   ErrorCode::unknown_entity, ErrorCode::duplicate_ticket, ErrorCode::unknown_ticket,
                                   ErrorCode::out_of_bounds};
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == expected.size(), "every invalid command rejected");
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(rejected[i].b == static_cast<std::uint64_t>(expected[i]) && rejected[i].key.root == i,
                    std::format("command {} code {}", i, rejected[i].b));
        require(of_kind(s.last, TraceKind::block_changed).empty(), "rejected commands have no effect");
    });

    suite.run("block event capacity failure is explicit", [] {
        auto limited = bundle();
        limited.limits.max_block_events_per_tick = 1;
        Sim s(limited);
        s.tick({place({0, 0, 0}, blocks::piston(D::north)), place({4, 0, 0}, blocks::piston(D::north))});
        bool failed = false;
        try {
            s.tick({place({1, 0, 0}, blocks::redstone_block()), place({5, 0, 0}, blocks::redstone_block())});
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed, "second event exceeds capacity");
        bool refused = false;
        try { s.tick(); } catch (const std::logic_error&) { refused = true; }
        require(refused, "failed oracle refuses further ticks");
    });

    return suite.finish();
}
