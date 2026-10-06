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
        // Commands are inputs between ticks: delays they start count from the previous tick.
        s.tick({use_lever({13, 0, 0})}); // tick 3, scheduling at time 2
        require(wire_level(s, {14, 0, 0}) == 15 && wire_level(s, {15, 0, 0}) == 14 && wire_level(s, {16, 0, 0}) == 13
                    && wire_level(s, {17, 0, 0}) == 12, "dust decays by one per block across the edge");
        const auto scheduled = of_kind(s.last, TraceKind::tick_scheduled);
        require(scheduled.size() == 1 && scheduled[0].pos == BlockPos{18, 0, 0} && scheduled[0].b == 4 && scheduled[0].c == 127,
                "repeater schedules delay 2 at priority -1");
        require(!s.at({18, 0, 0}).flag(), "repeater waits for its delay");
        s.tick(); // tick 4
        require(s.at({18, 0, 0}).flag() && wire_level(s, {19, 0, 0}) == 15 && s.at({20, 0, 0}).flag(),
                "repeater output powers dust and lamp on tick 4");
        s.until(6);
        s.tick({use_lever({13, 0, 0})}); // tick 7
        for (int x = 14; x <= 17; ++x) require(wire_level(s, {x, 0, 0}) == 0, "dust depowers completely");
        require(s.at({18, 0, 0}).flag() && s.at({20, 0, 0}).flag(), "repeater holds until tick 8");
        s.tick(); // tick 8
        require(!s.at({18, 0, 0}).flag() && wire_level(s, {19, 0, 0}) == 0 && s.at({20, 0, 0}).flag(),
                "repeater off on tick 8; lamp delays");
        s.until(11);
        require(s.at({20, 0, 0}).flag(), "lamp still lit on tick 11");
        s.tick(); // tick 12
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
        s.tick({place({0, 0, 3}, blocks::glass())}); // tick 3 (time 2): observed block changes
        require(!s.at({0, 0, 4}).flag(), "observer delay");
        s.tick(); // 4
        require(s.at({0, 0, 4}).flag(), "observer pulse starts on tick 4");
        s.until(6);
        require(!s.at({0, 0, 4}).flag(), "two-tick observer pulse");
        s.until(11);
        require(!s.at({0, 0, 5}).flag() && !s.at({0, 0, 6}).flag(), "repeater delay 8");
        s.tick(); // 12
        require(s.at({0, 0, 5}).flag() && s.at({0, 0, 6}).flag(), "repeater and lamp on at 12");
        s.until(19);
        require(s.at({0, 0, 5}).flag(), "pulse extended to the repeater delay");
        s.tick(); // 20
        require(!s.at({0, 0, 5}).flag() && s.at({0, 0, 6}).flag(), "repeater off at 20");
        s.until(24);
        require(!s.at({0, 0, 6}).flag(), "lamp off at 24");
    });

    suite.run("scheduled tick priority before sub-order", [] {
        Sim s;
        s.tick({place({-1, 0, 0}, blocks::lamp()), place({0, 0, -1}, blocks::repeater(D::north, 2)),
                place({0, 0, 1}, blocks::repeater(D::south, 2)), place({0, 0, 0}, blocks::lever())});
        s.tick({use_lever({0, 0, 0})}); // 3 (time 2): lamp lit; repeaters due 6
        require(s.at({-1, 0, 0}).flag(), "lamp lights immediately");
        s.until(6);
        const auto first = of_kind(s.last, TraceKind::tick_executed);
        require(first.size() == 2 && first[0].pos == BlockPos{0, 0, -1} && first[1].pos == BlockPos{0, 0, 1},
                "equal priority follows scheduling order");
        s.tick({use_lever({0, 0, 0})}); // 7 (time 6): lamp (prio 0) scheduled before both repeaters (prio -1)
        s.until(10);
        const auto order = of_kind(s.last, TraceKind::tick_executed);
        require(order.size() == 3 && order[0].pos == BlockPos{0, 0, -1} && order[1].pos == BlockPos{0, 0, 1}
                    && order[2].pos == BlockPos{-1, 0, 0}, "priority -1 drains before an earlier priority-0 tick");
        require(order[0].key.root == 0 && order[2].key.root == 2, "roots follow drain order");
        require(!s.at({-1, 0, 0}).flag() && !s.at({0, 0, 1}).flag(), "all off on tick 10");
    });

    suite.run("diodes feeding diodes and turning off drain first", [] {
        Sim s;
        // West: a repeater feeding a lamp. East: a repeater feeding a repeater.
        s.tick({place({-2, 0, 0}, blocks::lamp()), place({-1, 0, 0}, blocks::repeater(D::west, 1)),
                place({1, 0, 0}, blocks::repeater(D::east, 1)), place({2, 0, 0}, blocks::repeater(D::east, 1)),
                place({0, 0, 0}, blocks::lever())});
        s.tick({use_lever({0, 0, 0})}); // 3 (time 2): west scheduled first, both due 4
        s.tick();
        auto order = of_kind(s.last, TraceKind::tick_executed);
        require(order.size() == 2 && order[0].pos == BlockPos{1, 0, 0} && order[1].pos == BlockPos{-1, 0, 0},
                "a repeater feeding a diode (-3) drains before one feeding a lamp (-1)");
        s.until(6);
        s.tick({use_lever({0, 0, 0})}); // 7 (time 6): both turning off, due 8
        s.tick();
        order = of_kind(s.last, TraceKind::tick_executed);
        require(order.size() == 2 && order[0].pos == BlockPos{1, 0, 0} && order[1].pos == BlockPos{-1, 0, 0},
                "feeding a diode (-3) before turning off (-2)");

        Sim back;
        // The diode in front faces back into the east repeater: no raised priority.
        back.tick({place({-2, 0, 0}, blocks::lamp()), place({-1, 0, 0}, blocks::repeater(D::west, 1)),
                   place({1, 0, 0}, blocks::repeater(D::east, 1)), place({2, 0, 0}, blocks::repeater(D::west, 1)),
                   place({0, 0, 0}, blocks::lever())});
        back.tick({use_lever({0, 0, 0})});
        back.tick();
        order = of_kind(back.last, TraceKind::tick_executed);
        require(order.size() == 2 && order[0].pos == BlockPos{-1, 0, 0}, "a diode facing back is not fed");
    });

    suite.run("an observer moved by a piston pulses at its new position", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::piston(D::east, false)), place({1, 0, 0}, blocks::observer(D::north)),
                place({-1, 0, 0}, blocks::lever())});
        s.tick(); // 3
        require(!s.at({1, 0, 0}).flag(), "placing an observer does not fire it");
        s.tick({use_lever({-1, 0, 0})}); // 4: extension starts
        s.until(6); // the move finishes in the block-entity phase
        require(s.at({2, 0, 0}) == blocks::observer(D::north), "observer moved, not yet firing");
        s.until(8);
        require(s.at({2, 0, 0}).flag(), "fires two ticks after the move");
        s.until(10);
        require(!s.at({2, 0, 0}).flag(), "two-tick pulse");
    });

    suite.run("an observer watching a piston fires on the block event", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::piston(D::east, false)), place({0, 1, 0}, blocks::observer(D::down)),
                place({-1, 0, 0}, blocks::lever())});
        s.tick({use_lever({-1, 0, 0})}); // 3: the extension writes send shape updates at once
        s.tick();
        require(!s.at({0, 1, 0}).flag(), "observer delay");
        s.tick(); // 5: the move also finishes, while the observer is on
        require(s.at({0, 1, 0}).flag(), "fires two ticks after the block event");
        s.until(7);
        require(!s.at({0, 1, 0}).flag(), "two-tick pulse");
        s.until(10);
        require(!s.at({0, 1, 0}).flag(), "the finished move does not fire it again");
    });

    suite.run("neighbor chain limit truncates", [] {
        auto limited = bundle();
        limited.limits.max_chained_neighbor_updates = 2;
        Sim s(limited);
        std::vector<CommandBody> build{place({0, 0, 0}, blocks::lever())};
        for (int x = 1; x <= 10; ++x) build.push_back(place({x, 0, 0}, blocks::wire()));
        s.tick(build); // Placement chains truncate too; the line stays unpowered.
        s.tick({use_lever({0, 0, 0})});
        // Entry 1: the lever's neighbor updates. Its east delivery powers dust 1, whose
        // first shape update is entry 2; its other shape updates and its seven
        // neighbor-update entries are dropped. The queued entries still run.
        require(wire_level(s, {1, 0, 0}) == 15, "dust 1 powered");
        for (int x = 2; x <= 10; ++x) require(wire_level(s, {x, 0, 0}) == 0, "dropped updates");
        const auto cut = of_kind(s.last, TraceKind::chain_truncated);
        require(cut.size() == 1 && cut[0].pos == BlockPos{1, 0, 0} && cut[0].a == 2, "truncation point is recorded");
    });

    suite.run("piston moves a line across a chunk edge in two ticks", [] {
        Sim s;
        s.tick({place({14, 0, 0}, blocks::stone()), place({15, 0, 0}, blocks::stone()), place({16, 0, 0}, blocks::stone()),
                place({13, 0, 0}, blocks::piston(D::east)), place({13, 0, 1}, blocks::lever())});
        s.tick({use_lever({13, 0, 1})}); // 3: extend starts
        require(s.at({13, 0, 0}).flag(), "base extends at once");
        for (int x = 14; x <= 17; ++x) require(s.at({x, 0, 0}).is(BlockType::moving_piston), "head and line are moving");
        const auto moving = s.oracle.dump(); // Keep the dump alive while pointing into it.
        const auto* head = block_entity_at(moving, {14, 0, 0});
        require(head && head->progress == 1 && Motion::unpack(head->motion).source
                    && BlockState::decode(head->moving).value().is(BlockType::piston_head), "moving head");
        s.tick(); // 4
        require(s.at({15, 0, 0}).is(BlockType::moving_piston), "still moving on tick 4");
        s.tick(); // 5
        require(s.at({14, 0, 0}).is(BlockType::piston_head) && s.at({14, 0, 0}).facing() == D::east, "head arrives on tick 5");
        for (int x = 15; x <= 17; ++x) require(s.at({x, 0, 0}).is(BlockType::stone), "line arrives on tick 5");
        require(!block_entity_at(s.oracle.dump(), {15, 0, 0}), "moving block entities are gone");
        s.tick({use_lever({13, 0, 1})}); // 6: retract starts
        require(s.at({13, 0, 0}).is(BlockType::moving_piston) && s.at({14, 0, 0}).is_air(), "base moves, head removed");
        s.tick({use_lever({13, 0, 1})}); // 7: power returns while the base is moving
        require(s.at({13, 0, 0}).is(BlockType::moving_piston), "a moving base ignores power");
        s.tick(); // 8: the base arrives and reacts to its power
        require(s.at({13, 0, 0}) == blocks::piston(D::east), "base arrives retracted");
        require(s.oracle.dump().block_events.size() == 1, "extension queued for the next block-event phase");
        s.tick(); // 9
        require(s.at({13, 0, 0}).flag() && s.at({14, 0, 0}).is(BlockType::moving_piston), "extends again on tick 9");
        require(s.at({15, 0, 0}).is(BlockType::stone) && s.at({17, 0, 0}).is(BlockType::stone), "regular piston left the line");
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
        s.until(5);
        require(s.at({2, 0, 0}).is(BlockType::stone) && s.at({1, 0, 0}).is(BlockType::piston_head) && s.at({1, 0, 0}).flag(),
                "sticky head after the move");
        s.tick({place({0, 0, 1}, blocks::air())}); // 6: retract and pull
        require(s.at({0, 0, 0}).is(BlockType::moving_piston) && s.at({1, 0, 0}).is(BlockType::moving_piston)
                    && s.at({2, 0, 0}).is_air(), "base and pulled block move");
        s.until(8);
        require(s.at({0, 0, 0}) == blocks::piston(D::east, true) && s.at({1, 0, 0}).is(BlockType::stone), "pulled back");

        Sim t;
        t.tick({place({1, 0, 0}, blocks::slime()), place({1, 1, 0}, blocks::stone()), place({1, 0, 1}, blocks::stone()),
                place({1, 0, -1}, blocks::obsidian()), place({0, 0, 0}, blocks::piston(D::east, true))});
        t.tick({place({0, 0, 1}, blocks::redstone_block())});
        t.until(5);
        require(t.at({2, 0, 0}).is(BlockType::slime_block) && t.at({2, 1, 0}).is(BlockType::stone)
                    && t.at({2, 0, 1}).is(BlockType::stone), "slime structure moves together");
        require(t.at({1, 0, -1}).is(BlockType::obsidian) && t.at({1, 1, 0}).is_air(), "immovable neighbor stays");
    });

    suite.run("sticky piston drops its block on a short pulse", [] {
        Sim s;
        s.tick({place({1, 0, 0}, blocks::stone()), place({0, 0, 0}, blocks::piston(D::east, true)), place({0, 0, 1}, blocks::lever())});
        s.tick({use_lever({0, 0, 1})}); // 3: extension starts
        s.tick({use_lever({0, 0, 1})}); // 4: retraction before the push finished
        require(s.at({2, 0, 0}).is(BlockType::stone), "pushed block placed at once");
        require(s.at({1, 0, 0}).is_air() && s.at({0, 0, 0}).is(BlockType::moving_piston), "head removed, base moving");
        s.until(6);
        require(s.at({0, 0, 0}) == blocks::piston(D::east, true) && s.at({1, 0, 0}).is_air()
                    && s.at({2, 0, 0}).is(BlockType::stone), "block left behind");
    });

    suite.run("pistons destroy dust and comparators in their path", [] {
        Sim s;
        s.tick({place({1, 0, 0}, blocks::stone()), place({2, 0, 0}, blocks::wire()), place({3, 0, 0}, blocks::stone()),
                place({0, 0, 0}, blocks::piston(D::east)), place({1, 0, 4}, blocks::comparator(D::north)),
                place({0, 0, 4}, blocks::piston(D::east))});
        require(block_entity_at(s.oracle.dump(), {1, 0, 4}), "comparator block entity");
        s.tick({place({0, 0, -1}, blocks::redstone_block()), place({0, 0, 5}, blocks::redstone_block())}); // 3
        require(s.at({2, 0, 0}).is(BlockType::moving_piston) && s.at({1, 0, 4}).is(BlockType::moving_piston), "destroyed and replaced");
        s.until(5);
        const auto d = s.oracle.dump();
        require(state_at(d, {2, 0, 0}).is(BlockType::stone) && state_at(d, {3, 0, 0}).is(BlockType::stone), "line ended at the dust");
        require(state_at(d, {1, 0, 4}).is(BlockType::piston_head) && !block_entity_at(d, {1, 0, 4}), "comparator destroyed");
    });

    suite.run("destroyed blocks drop items in event order", [] {
        Sim s;
        std::vector<CommandBody> build;
        for (int x = 0; x <= 6; ++x)
            for (int z = 0; z <= 6; ++z) build.push_back(place({x, -1, z}, blocks::stone()));
        append(build, {place({1, 0, 0}, blocks::stone()), place({2, 0, 0}, blocks::wire()), place({0, 0, 0}, blocks::piston(D::east)),
                       place({1, 0, 3}, blocks::comparator(D::north)), place({0, 0, 3}, blocks::piston(D::east)),
                       place({1, 0, 5}, blocks::wheat(7)), place({0, 0, 5}, blocks::piston(D::east)),
                       place({1, 0, 6}, blocks::water_source()), place({0, 0, 6}, blocks::piston(D::east))});
        s.tick(build);
        // One tick, four pistons: events run in queue order, so drop ids follow it; water drops nothing.
        s.tick({place({0, 1, 0}, blocks::redstone_block()), place({0, 1, 3}, blocks::redstone_block()),
                place({0, 1, 5}, blocks::redstone_block()), place({0, 1, 6}, blocks::redstone_block())});
        const auto d = s.oracle.dump();
        require(d.entities.size() == 4, std::format("four drops, got {}", d.entities.size()));
        require(d.entities[0].stack == ItemStack{items::redstone, 1} && d.entities[1].stack == ItemStack{items::comparator, 1}
                    && d.entities[2].stack == ItemStack{items::wheat, 1} && d.entities[3].stack == ItemStack{items::wheat_seeds, 1},
                "drops in event order");
        // Spawned at the dust's center inside the arriving block, then lifted onto it by the entity tick.
        require(d.entities[0].position == FixedVec{2 * fixed_one + 2048, fixed_one, 2048} && d.entities[0].on_ground,
                "drop position");
        const auto spawned = of_kind(s.last, TraceKind::entity_spawned);
        require(spawned.size() == 4 && spawned[0].key.phase == Phase::block_events && spawned[0].a == 1 && spawned[3].a == 4,
                "spawn records carry serial ids");
        require(block_entity_at(d, {1, 0, 3})->kind == BlockType::moving_piston, "comparator block entity replaced by the moving head");
        require(state_at(d, {1, 0, 6}).is(BlockType::moving_piston), "water destroyed without a drop");
    });

    suite.run("moving blocks push item entities across a chunk edge", [] {
        Sim s;
        std::vector<CommandBody> build;
        for (int x = 12; x <= 18; ++x) build.push_back(place({x, -1, 0}, blocks::stone()));
        append(build, {place({14, 0, 0}, blocks::stone()), place({13, 0, 0}, blocks::piston(D::east)),
                       SpawnItem{at_block(15.5, 0.0, 0.5), {}, {items::stone, 1}}});
        s.tick(build); // 2
        s.tick({place({13, 0, 1}, blocks::redstone_block())}); // 3: stone starts moving into cell 15
        const auto d = s.oracle.dump();
        require(d.entities.size() == 1 && d.entities[0].position.x == 16 * fixed_one, "pushed half a block, out of the cell");
        const auto crossed = of_kind(s.last, TraceKind::entity_changed_chunk);
        require(crossed.size() == 1 && crossed[0].key.phase == Phase::block_entities && crossed[0].c == pack_chunk({1, 0}),
                "pushed into chunk 1");
        s.until(5);
        require(entity_in(s.oracle.dump(), EntityId{1})->position.x == 16 * fixed_one && s.at({15, 0, 0}).is(BlockType::stone),
                "pushed once; block arrived");
    });

    suite.run("moving blocks do not conduct power", [] {
        Sim s;
        s.tick({place({1, 0, 0}, blocks::redstone_block()), place({3, 0, 0}, blocks::lamp()), place({0, 0, 0}, blocks::piston(D::east)),
                place({0, 0, 1}, blocks::lever())});
        s.tick({use_lever({0, 0, 1})}); // 3: the redstone block starts moving next to the lamp
        require(s.at({2, 0, 0}).is(BlockType::moving_piston) && !s.at({3, 0, 0}).flag(), "unlit while moving");
        s.tick();
        require(!s.at({3, 0, 0}).flag(), "unlit on tick 4");
        s.tick();
        require(s.at({2, 0, 0}).is(BlockType::redstone_block) && s.at({3, 0, 0}).flag(), "lit when it arrives on tick 5");
    });

    suite.run("quasi-connectivity needs an update", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::piston(D::north))});
        s.tick({place({1, 1, 0}, blocks::lever(true))}); // powers the cell above the piston only
        require(!s.at({0, 0, 0}).flag(), "no update reached the piston");
        s.tick({place({-1, 0, 0}, blocks::stone())}); // 4: neighbor update
        require(s.at({0, 0, 0}).flag() && s.at({0, 0, -1}).is(BlockType::moving_piston), "updated piston extends");
        s.until(6);
        require(s.at({0, 0, -1}).is(BlockType::piston_head), "head arrives");
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
        s.tick({place({0, 1, 0}, blocks::water_source())}); // tick 3, scheduling at time 2
        for (int n = 1; n <= 7; ++n) {
            s.until(2 + 5 * static_cast<Tick>(n) - 1);
            require(s.at({n, 1, 0}).is_air(), std::format("x={} not reached before tick {}", n, 2 + 5 * n));
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

    suite.run("ticket bounds are checked without overflow", [] {
        const auto& limits = bundle().limits;
        const std::int32_t limit = world_border / 16;
        const auto accepted = [&](ChunkPos center, std::uint8_t radius) {
            TicketSet tickets;
            return tickets.add({1, center, radius}, limits).has_value();
        };
        for (const auto extreme : {INT32_MIN, INT32_MAX})
            for (const std::uint8_t radius : {std::uint8_t{0}, limits.max_ticket_radius})
                require(!accepted({extreme, 0}, radius) && !accepted({0, extreme}, radius), "extreme centers");
        // A ticket of radius r reaches r + 1 chunks; every reached chunk stays inside the border.
        for (const std::uint8_t radius : {std::uint8_t{0}, limits.max_ticket_radius}) {
            const std::int32_t reach = radius + 1;
            const std::int32_t low = -limit + reach, high = limit - 1 - reach;
            require(accepted({low, 0}, radius) && accepted({high, 0}, radius) && accepted({0, low}, radius)
                        && accepted({0, high}, radius),
                    std::format("radius {} endpoints accepted", radius));
            require(!accepted({low - 1, 0}, radius) && !accepted({high + 1, 0}, radius) && !accepted({0, low - 1}, radius)
                        && !accepted({0, high + 1}, radius),
                    std::format("radius {} beyond the endpoints rejected", radius));
        }
        TicketSet tickets;
        require(tickets.add({1, {0, 0}, static_cast<std::uint8_t>(limits.max_ticket_radius + 1)}, limits).error()
                    == ErrorCode::out_of_bounds,
                "radius limit");
    });

    suite.run("spawn positions are checked before narrowing", [] {
        Sim s;
        const auto before = s.oracle.dump();
        // 2^44 fixed units is 2^32 blocks: narrowing first would alias block (0, 1, 0).
        s.tick({SpawnItem{{std::int64_t{1} << 44, fixed_one, 0}, {}, {items::stone, 1}},
                SpawnItem{{0, INT64_MIN, 0}, {}, {items::stone, 1}}});
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == 2 && rejected[0].b == static_cast<std::uint64_t>(ErrorCode::out_of_bounds)
                    && rejected[1].b == static_cast<std::uint64_t>(ErrorCode::out_of_bounds),
                "rejected as out of bounds");
        auto after = s.oracle.dump();
        after.time = before.time;
        require(after == before, "no world mutation");
        // Valid endpoints: the lowest and highest positions inside the world height.
        const auto& world = bundle().world;
        s.tick({SpawnItem{{fixed_one / 2, std::int64_t{world.min_y} * fixed_one, fixed_one / 2}, {}, {items::stone, 1}},
                SpawnItem{{fixed_one / 2, (std::int64_t{world.min_y} + world.height) * fixed_one - 1, fixed_one / 2}, {},
                          {items::stone, 1}},
                SpawnItem{{fixed_border - 1, 0, 0}, {}, {items::stone, 1}}});
        require(of_kind(s.last, TraceKind::entity_spawned).size() == 2, "height endpoints accepted");
        const auto last = of_kind(s.last, TraceKind::command_rejected);
        require(last.size() == 1 && last[0].b == static_cast<std::uint64_t>(ErrorCode::unloaded_chunk),
                "inside the border but unloaded");
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
                                   ErrorCode::invalid_block_state, ErrorCode::invalid_block_state, ErrorCode::not_usable,
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

    suite.run("comparator compares after two ticks", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::lever()), place({1, 0, 0}, blocks::comparator(D::east)), place({2, 0, 0}, blocks::lamp())});
        s.tick({use_lever({0, 0, 0})}); // 3 (time 2): due 4
        require(!s.at({1, 0, 0}).flag() && !s.at({2, 0, 0}).flag(), "comparator delay");
        s.tick(); // 4
        require(s.at({1, 0, 0}).flag() && s.at({2, 0, 0}).flag(), "output on tick 4");
        require(block_entity_at(s.oracle.dump(), {1, 0, 0})->signal == 15, "full strength from a lever");
        const auto changed = of_kind(s.last, TraceKind::signal_changed);
        require(changed.size() == 1 && changed[0].a == 0 && changed[0].b == 15, "signal change recorded");
        s.until(6);
        s.tick({use_lever({0, 0, 0})}); // 7 (time 6): due 8, lamp off at 12
        s.tick();
        require(!s.at({1, 0, 0}).flag() && s.at({2, 0, 0}).flag(), "comparator off at 8, lamp delays");
        s.until(12);
        require(!s.at({2, 0, 0}).flag(), "lamp off at 12");
    });

    suite.run("comparator subtract and compare with a side input", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::lever()), place({1, 0, 0}, blocks::comparator(D::east, true)),
                place({2, 0, 0}, blocks::wire()), place({3, 0, 0}, blocks::wire()), place({4, 0, 0}, blocks::wire()),
                place({1, 0, 1}, blocks::wire()), place({1, 0, 2}, blocks::wire()), place({1, 0, 3}, blocks::wire()),
                place({1, 0, 4}, blocks::lever())});
        s.tick({use_lever({1, 0, 4})}); // 3: side input 13, rear 0: output stays 0
        require(wire_level(s, {1, 0, 1}) == 13 && !s.at({1, 0, 0}).flag(), "side input alone gives no output");
        s.tick({use_lever({0, 0, 0})}); // 4 (time 3): 15 - 13 = 2, due 5
        s.until(5);
        require(block_entity_at(s.oracle.dump(), {1, 0, 0})->signal == 2, "subtract");
        require(wire_level(s, {2, 0, 0}) == 2 && wire_level(s, {3, 0, 0}) == 1 && wire_level(s, {4, 0, 0}) == 0,
                "output strength drives dust");
        s.tick();
        s.tick({use_block({1, 0, 0})}); // 7: compare mode takes effect at once
        require(s.at({1, 0, 0}).level() == 0, "use toggles the mode");
        require(wire_level(s, {2, 0, 0}) == 15 && wire_level(s, {4, 0, 0}) == 13, "compare passes the rear signal");
        const auto changed = of_kind(s.last, TraceKind::signal_changed);
        require(changed.size() == 1 && changed[0].a == 2 && changed[0].b == 15, "strength change without a state change");
    });

    suite.run("comparator reads container fullness", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::hopper(D::down)), place({1, 0, 0}, blocks::comparator(D::east)),
                place({2, 0, 0}, blocks::lamp()), place({0, 0, 5}, blocks::chest()), place({1, 0, 5}, blocks::stone()),
                place({2, 0, 5}, blocks::comparator(D::east))});
        s.tick({insert({0, 0, 0}, 0, items::stone, 1), insert({0, 0, 5}, 0, items::stone, 1)}); // 3
        s.until(5);
        auto d = s.oracle.dump();
        require(block_entity_at(d, {1, 0, 0})->signal == 1 && state_at(d, {2, 0, 0}).flag(), "one item reads 1");
        require(block_entity_at(d, {2, 0, 5})->signal == 1, "reads a chest through a stone block");
        s.tick({insert({0, 0, 0}, 1, items::stone, 64), insert({0, 0, 0}, 2, items::stone, 64)}); // 6
        s.until(8);
        // 129 items in 5 slots of 64: 1 + 14 * 129 / 320 = 6.
        require(block_entity_at(s.oracle.dump(), {1, 0, 0})->signal == 6, "partial fullness");
        std::vector<CommandBody> fill;
        for (std::uint8_t i = 0; i < 27; ++i) fill.push_back(insert({0, 0, 5}, i, items::stone, i == 0 ? 63 : 64));
        s.tick(fill);
        s.until(s.oracle.time() + 2);
        require(block_entity_at(s.oracle.dump(), {2, 0, 5})->signal == 15, "full chest reads 15");
    });

    suite.run("double chest pairs, splits and combines slots", [] {
        Sim s;
        s.tick({place({1, 0, 0}, blocks::chest(D::north))});
        s.tick({place({0, 0, 0}, blocks::chest(D::north, blocks::ChestType::left))}); // 3: claims the single chest
        require(s.at({0, 0, 0}) == blocks::chest(D::north, blocks::ChestType::left)
                    && s.at({1, 0, 0}) == blocks::chest(D::north, blocks::ChestType::right), "pair formed");
        s.tick({place({1, 0, 0}, blocks::air())});
        require(s.at({0, 0, 0}) == blocks::chest(D::north), "orphaned half becomes single");
        s.tick({place({1, 0, 0}, blocks::chest(D::north, blocks::ChestType::right))});
        require(s.at({0, 0, 0}) == blocks::chest(D::north, blocks::ChestType::left), "re-paired");

        // The right half's slots come first, whichever half a hopper feeds (harness-verified).
        std::vector<CommandBody> b;
        for (std::uint8_t i = 0; i < 27; ++i) b.push_back(insert({1, 0, 0}, i, items::stone, i == 26 ? 63 : 64));
        b.push_back(place({0, 1, 0}, blocks::hopper(D::down)));
        b.push_back(insert({0, 1, 0}, 0, items::stone, 2));
        s.tick(b); // T: hopper above the left half
        const auto t0 = s.oracle.time();
        auto d = s.oracle.dump();
        require(block_entity_at(d, {1, 0, 0})->slots[26].count == 64 && block_entity_at(d, {0, 0, 0})->slots[0].empty(),
                "first item goes to the right half");
        s.until(t0 + 8);
        d = s.oracle.dump();
        require(block_entity_at(d, {0, 0, 0})->slots[0] == ItemStack{items::stone, 1}, "then the left half");
    });

    suite.run("comparator reads a double chest across a chunk edge", [] {
        Sim s;
        s.tick({place({15, 0, 0}, blocks::chest(D::north, blocks::ChestType::left)),
                place({16, 0, 0}, blocks::chest(D::north, blocks::ChestType::right)),
                place({15, 0, 1}, blocks::comparator(D::south))});
        require(s.at({16, 0, 0}).level() == 2, "halves in chunks 0 and 1 are paired");
        std::vector<CommandBody> fill;
        for (std::uint8_t i = 0; i < 27; ++i) fill.push_back(insert({16, 0, 0}, i, items::stone, 64));
        s.tick(fill);
        s.until(s.oracle.time() + 2);
        // 1728 items over 54 slots: 1 + 14 * 1728 / 3456 = 8 (a single chest would read 15).
        require(block_entity_at(s.oracle.dump(), {15, 0, 1})->signal == 8, "combined fullness");
    });

    suite.run("use cycles repeater delay", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::repeater(D::east, 4))});
        s.tick({use_block({0, 0, 0})});
        require(s.at({0, 0, 0}).delay() == 1, "4 wraps to 1");
        s.tick({use_block({0, 0, 0}), use_block({5, 0, 5}), insert({0, 0, 0}, 0, items::stone, 1)});
        require(s.at({0, 0, 0}).delay() == 2, "1 to 2");
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == 2 && rejected[0].b == static_cast<std::uint64_t>(ErrorCode::not_usable)
                    && rejected[1].b == static_cast<std::uint64_t>(ErrorCode::not_a_container), "air is not usable");
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
