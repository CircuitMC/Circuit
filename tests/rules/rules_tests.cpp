// SPDX-License-Identifier: AGPL-3.0-only
// Reference-rule fixtures with expectations derived by hand from the rule
// definitions (not from running the runtime). They check the rules and the
// oracle; vanilla compatibility is not established by these values.
#include "../oracle/oracle.hpp"
#include "../support/check.hpp"
#include "../support/light_reference.hpp"
#include "../support/scenario.hpp"
#include <circuit/gameplay/level_random.hpp>
#include <circuit/gameplay/random_sequence.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/experience.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/potions.hpp>
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/light.hpp>
#include <circuit/gameplay/loot.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <circuit/gameplay/rules.hpp>
#include <circuit/gameplay/survival.hpp>
#include <bit>
#include <cmath>
#include <numbers>
#include <limits>
#include <format>
#include <map>
#include <random>
#include <set>

using namespace circuit::test;
using circuit::oracle::Oracle;
using circuit::test::require;
using D = Direction;

namespace {
const VersionBundle& bundle() { return **BundleRegistry::select_for_simulation("java-1.21.8"); }

struct Sim {
    explicit Sim(const VersionBundle& b = bundle(), std::uint64_t seed = 7, std::uint8_t radius = 2,
                 std::shared_ptr<const circuit::gameplay::GameData> data = {})
        : oracle(b, seed, std::move(data)) {
        // Random ticks are off unless a test turns them on, as in the
        // harness's scenarios: they draw from the level random.
        tick({ticket(1, {0, 0}, radius), SetGameRule{"randomTickSpeed", 0}});
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

// A stone floor at y = -1 under x in [x0, x1] and z in [z0, z1]: dust and
// diodes need support.
// A loot table of one pool.
circuit::gameplay::loot::Table one_pool(std::vector<circuit::gameplay::loot::Entry> entries) {
    circuit::gameplay::loot::Table t;
    t.pools.emplace_back().entries = std::move(entries);
    return t;
}

std::vector<CommandBody> floor(std::int32_t x0, std::int32_t x1, std::int32_t z0, std::int32_t z1) {
    std::vector<CommandBody> out;
    for (auto x = x0; x <= x1; ++x)
        for (auto z = z0; z <= z1; ++z) out.push_back(place({x, -1, z}, blocks::stone()));
    return out;
}
std::vector<CommandBody> with_floor(std::vector<CommandBody> floor_cmds, std::vector<CommandBody> rest) {
    floor_cmds.insert(floor_cmds.end(), rest.begin(), rest.end());
    return floor_cmds;
}

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
        require(BlockState::decode(blocks::water_falling().with_level(3).raw()).has_value(), "falling water keeps its stored level");
        require(!BlockState::decode(blocks::air().with_level(3).raw()).has_value(), "three kinds of air");
        require(!BlockState::decode(BlockState::registered(0).raw()).has_value(), "internal states start at 1");
        require(!BlockState::decode(BlockState::registered(circuit::gameplay::registry_state_count() + 1).raw()).has_value(),
                "internal states end at the registry size");
        require(!BlockState::decode(BlockState::registered(2).raw()).has_value(), "stone is modeled and never a registry state");
        require(BlockState::decode(BlockState::registered(3).raw()).has_value(), "granite is a registry state");
        for (const auto& bundle : circuit::contracts::BundleRegistry::candidates())
            if (bundle.rules)
                require(bundle.rules->block_registry == circuit::gameplay::registry::id(), "rule sets name the built registry");
        require(blocks::repeater(D::west, 3).delay() == 3 && blocks::wire(9).level() == 9, "accessors");
    });

    suite.run("blocks without rules of their own are inert", [] {
        const auto dirt = registry_block("dirt");
        // An opaque block conducts a repeater's power; a translucent one does not.
        for (const auto& [middle, conducts] : {std::pair{dirt, true}, std::pair{registry_block("white_stained_glass"), false}}) {
            Sim s;
            s.tick({place({0, -1, 0}, dirt), place({0, 0, 0}, blocks::repeater(D::east)), place({1, 0, 0}, middle),
                    place({2, 0, 0}, blocks::lamp()), place({-1, 0, 0}, blocks::redstone_block())});
            s.until(8);
            require(s.at({0, 0, 0}).flag() && s.at({2, 0, 0}).flag() == conducts, conducts ? "dirt conducts" : "stained glass does not");
        }
        // Dust stays on a covered top face or an opaque block.
        {
            Sim s;
            s.tick({place({0, -1, 0}, registry_block("oak_slab", {{"type", "bottom"}})), place({0, 0, 0}, blocks::wire()),
                    place({2, -1, 0}, registry_block("oak_slab", {{"type", "top"}})), place({2, 0, 0}, blocks::wire()),
                    place({4, -1, 0}, registry_block("soul_sand")), place({4, 0, 0}, blocks::wire())});
            s.tick({place({0, 0, 1}, blocks::stone()), place({2, 0, 1}, blocks::stone()), place({4, 0, 1}, blocks::stone())});
            require(s.at({0, 0, 0}).is_air() && s.at({2, 0, 0}).is(BlockType::redstone_wire) && s.at({4, 0, 0}).is(BlockType::redstone_wire),
                    "dust support");
        }
        // Pistons cannot move the immovable ones (see the push classes below).
        {
            Sim s;
            const auto table = registry_block("enchanting_table");
            s.tick({place({1, 0, 0}, table), place({0, 0, 0}, blocks::piston(D::east))});
            s.tick({place({0, 0, 1}, blocks::redstone_block())});
            require(!s.at({0, 0, 0}).flag() && s.at({1, 0, 0}) == table && of_kind(s.last, TraceKind::piston_blocked).size() == 1,
                    "immovable");
        }
        // Items land on blocks with collision and fall through those without.
        {
            Sim s;
            s.tick({place({0, 0, 0}, dirt), place({2, -1, 0}, blocks::stone()), place({2, 0, 0}, registry_block("short_grass")),
                    SpawnItem{at_block(0.5, 3.5, 0.5), {}, {items::redstone, 1}}, SpawnItem{at_block(2.5, 3.5, 0.5), {}, {items::stone, 1}}});
            s.until(60);
            const auto d = s.oracle.dump();
            require(d.overworld().entities.size() == 2, "both items rest");
            for (const auto& e : d.overworld().entities)
                require(e.position.y == (e.stack.item == items::redstone ? 1.0 : 0.0), std::format("item rests at {}", e.position.y));
        }
        // Breaking follows the block's hardness, tool and tier.
        using circuit::gameplay::destroy_progress;
        const auto near = [](float a, float b) { return std::abs(a - b) < 1e-6f; };
        require(near(destroy_progress(dirt, {}, true), 1.0f / 0.5f / 30.0f), "dirt by hand");
        require(near(destroy_progress(dirt, {items::iron_shovel, 1, 0}, true), 6.0f / 0.5f / 30.0f), "dirt with a shovel");
        require(near(destroy_progress(dirt, {items::iron_pickaxe, 1, 0}, true), 1.0f / 0.5f / 30.0f), "a pickaxe is no faster on dirt");
        require(near(destroy_progress(registry_block("iron_ore"), {items::wooden_pickaxe, 1, 0}, true), 2.0f / 3.0f / 100.0f),
                "iron ore needs a stone pickaxe");
        require(near(destroy_progress(registry_block("iron_ore"), {items::stone_pickaxe, 1, 0}, true), 4.0f / 3.0f / 30.0f), "stone pickaxe");
        require(destroy_progress(registry_block("bedrock"), {}, true) == 0.0f, "unbreakable");
        require(destroy_progress(registry_block("short_grass"), {}, true) >= 1.0f, "instant");
    });

    suite.run("vertical slice: line and repeater across a chunk edge", [] {
        Sim s;
        // Lever at x=13; dust x=14..17 crosses from chunk 0 into chunk 1 at x=16.
        s.tick(with_floor(floor(13, 20, 0, 0),
                          {place({13, 0, 0}, blocks::lever()), place({14, 0, 0}, blocks::wire()), place({15, 0, 0}, blocks::wire()),
                           place({16, 0, 0}, blocks::wire()), place({17, 0, 0}, blocks::wire()),
                           place({18, 0, 0}, blocks::repeater(D::east, 1)), place({19, 0, 0}, blocks::wire()),
                           place({20, 0, 0}, blocks::lamp())}));
        require(s.oracle.dump().overworld().ticks.empty(), "unpowered setup schedules nothing");
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
        s.tick(with_floor(floor(14, 16, 0, 2),
                          {place({14, 0, 0}, blocks::lever()), place({15, 0, 0}, blocks::wire()), place({16, 0, 0}, blocks::wire()),
                           place({16, 0, 1}, blocks::wire()), place({16, 0, 2}, blocks::wire()), place({15, 0, 2}, blocks::wire()),
                           place({14, 0, 2}, blocks::lamp())}));
        s.tick({use_lever({14, 0, 0})});
        require(wire_level(s, {15, 0, 2}) == 11 && s.at({14, 0, 2}).flag(), "lamp lit in the same tick after two crossings");
        std::set<std::int32_t> chunks_seen;
        for (const auto& r : of_kind(s.last, TraceKind::block_changed)) chunks_seen.insert(chunk_of(r.pos).x);
        require(chunks_seen == std::set<std::int32_t>{0, 1}, "changes in both chunks");
        require(of_kind(s.last, TraceKind::tick_scheduled).empty(), "no deferral to a later tick");
    });

    suite.run("observer pulse is extended by a repeater", [] {
        Sim s;
        s.tick(with_floor(floor(0, 0, 3, 6), {place({0, 0, 3}, blocks::stone()), place({0, 0, 4}, blocks::observer(D::north)),
                                              place({0, 0, 5}, blocks::repeater(D::south, 4)), place({0, 0, 6}, blocks::lamp())}));
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
        s.tick(with_floor(floor(-1, 0, -1, 1), {place({-1, 0, 0}, blocks::lamp()), place({0, 0, -1}, blocks::repeater(D::north, 2)),
                                                place({0, 0, 1}, blocks::repeater(D::south, 2)), place({0, 0, 0}, blocks::lever())}));
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
        s.tick(with_floor(floor(-2, 2, 0, 0), {place({-2, 0, 0}, blocks::lamp()), place({-1, 0, 0}, blocks::repeater(D::west, 1)),
                                               place({1, 0, 0}, blocks::repeater(D::east, 1)), place({2, 0, 0}, blocks::repeater(D::east, 1)),
                                               place({0, 0, 0}, blocks::lever())}));
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
        back.tick(with_floor(floor(-2, 2, 0, 0), {place({-2, 0, 0}, blocks::lamp()), place({-1, 0, 0}, blocks::repeater(D::west, 1)),
                                                  place({1, 0, 0}, blocks::repeater(D::east, 1)), place({2, 0, 0}, blocks::repeater(D::west, 1)),
                                                  place({0, 0, 0}, blocks::lever())}));
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
        std::vector<CommandBody> build = floor(0, 10, 0, 0);
        build.push_back(place({0, 0, 0}, blocks::lever()));
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
        require(s.oracle.dump().overworld().block_events.size() == 1, "extension queued for the next block-event phase");
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
        s.tick(with_floor(floor(0, 3, 0, 4), {place({1, 0, 0}, blocks::stone()), place({2, 0, 0}, blocks::wire()), place({3, 0, 0}, blocks::stone()),
                                              place({0, 0, 0}, blocks::piston(D::east)), place({1, 0, 4}, blocks::comparator(D::north)),
                                              place({0, 0, 4}, blocks::piston(D::east))}));
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
            for (int z = 0; z <= 6; ++z) build.push_back(place({x, -1, z}, x == 1 && z == 5 ? registry_block("farmland") : blocks::stone()));
        append(build, {place({1, 0, 0}, blocks::stone()), place({2, 0, 0}, blocks::wire()), place({0, 0, 0}, blocks::piston(D::east)),
                       place({1, 0, 3}, blocks::comparator(D::north)), place({0, 0, 3}, blocks::piston(D::east)),
                       place({1, 0, 5}, blocks::wheat(7)), place({0, 0, 5}, blocks::piston(D::east)),
                       place({1, 0, 6}, blocks::water_source()), place({0, 0, 6}, blocks::piston(D::east))});
        s.tick(build);
        // One tick, four pistons: events run in queue order, so drop ids follow it; water drops nothing.
        s.tick({place({0, 1, 0}, blocks::redstone_block()), place({0, 1, 3}, blocks::redstone_block()),
                place({0, 1, 5}, blocks::redstone_block()), place({0, 1, 6}, blocks::redstone_block())});
        const auto d = s.oracle.dump();
        require(d.overworld().entities.size() == 4, std::format("four drops, got {}", d.overworld().entities.size()));
        require(d.overworld().entities[0].stack == ItemStack{items::redstone, 1} && d.overworld().entities[1].stack == ItemStack{items::comparator, 1}
                    && d.overworld().entities[2].stack == ItemStack{items::wheat, 1} && d.overworld().entities[3].stack == ItemStack{items::wheat_seeds, 1},
                "drops in event order");
        // Each drop within 0.25 of its block's center (lowered by 0.125), moving 0.2 up, from the level random.
        const auto positions = of_kind(s.last, TraceKind::entity_spawn_position);
        const auto velocities = of_kind(s.last, TraceKind::entity_spawn_velocity);
        require(positions.size() == 4 && velocities.size() == 4, "spawn positions and velocities");
        const auto x = bits_double(positions[0].a), y = bits_double(positions[0].b), z = bits_double(positions[0].c);
        require(x >= 2.25 && x <= 2.75 && y >= 0.125 && y <= 0.625 && z >= 0.25 && z <= 0.75 && bits_double(velocities[0].b) == 0.2
                    && std::abs(bits_double(velocities[0].a)) <= 0.1 && std::abs(bits_double(velocities[0].c)) <= 0.1,
                "drop position");
        require(of_kind(s.last, TraceKind::level_random_draw).size() >= 4 * 5, "five draws per drop");
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
        // Each step pushes it out of the space the block sweeps through, to
        // 0.01 beyond it: from the block's face at 15 to 15.5, then to 16.
        const auto d = s.oracle.dump();
        require(d.overworld().entities.size() == 1 && std::abs(d.overworld().entities[0].position.x - 15.635) < 1e-9, "pushed out of the first step");
        s.tick(); // 4
        require(std::abs(entity_in(s.oracle.dump(), EntityId{1})->position.x - 16.135) < 1e-9, "and out of the second, into chunk 1");
        const auto crossed = of_kind(s.last, TraceKind::entity_changed_chunk);
        require(crossed.size() == 1 && crossed[0].key.phase == Phase::block_entities && crossed[0].c == pack_chunk({1, 0}),
                "pushed into chunk 1");
        s.until(6);
        require(std::abs(entity_in(s.oracle.dump(), EntityId{1})->position.x - 16.135) < 1e-9 && s.at({15, 0, 0}).is(BlockType::stone),
                "pushed twice; block arrived");
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

    suite.run("hoppers keep damage and components", [] {
        Sim s;
        s.tick({place({0, 1, 0}, blocks::chest()), place({0, 0, 0}, blocks::hopper(D::down)), place({0, -1, 0}, blocks::chest()),
                InsertItem{{0, 1, 0}, 0, ItemStack{items::iron_pickaxe, 1, 40}}, InsertItem{{0, 1, 0}, 1, named(items::stone, 2, "a")},
                InsertItem{{0, 1, 0}, 2, ItemStack{items::stone, 2}}});
        s.until(80);
        const auto d = s.oracle.dump();
        const auto* below = block_entity_at(d, {0, -1, 0});
        require(below && below->slots[0] == ItemStack{items::iron_pickaxe, 1, 40}, "the tool keeps its damage");
        require(below->slots[1] == named(items::stone, 2, "a") && below->slots[2] == ItemStack{items::stone, 2},
                "named and plain stone stay apart");
    });

    suite.run("hopper picks up a falling item", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::hopper(D::down)),
                SpawnItem{{0.5, 3.5, 0.5}, {}, {items::redstone, 5}}}); // tick 2
        // Independent expectation of the documented motion rule: gravity
        // 0.04, then the move, then drag 0.98; picked up in the cell above.
        double y = 3.5, vy = 0.0;
        Tick pickup = 0;
        for (Tick t = 2; t < 100 && !pickup; ++t) {
            vy -= 0.04;
            y += vy;
            vy *= 0.98;
            if (std::floor(y) == 1.0) pickup = t;
        }
        require(pickup > 2, "expectation computed");
        s.until(pickup - 1);
        require(s.oracle.dump().overworld().entities.size() == 1, "still falling");
        s.tick();
        const auto d = s.oracle.dump();
        require(d.overworld().entities.empty() && block_entity_at(d, {0, 0, 0})->slots[0] == ItemStack{items::redstone, 5}, "picked up");
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

    suite.run("random ticks share the level's random and position generator", [] {
        const auto run = [](std::uint64_t seed, bool neighbor_activity) {
            Sim s(bundle(), seed, 1);
            std::vector<CommandBody> build;
            // Wheat stays on farmland only.
            const auto crop = [&](BlockPos p) {
                build.push_back(place(p.relative(D::down), registry_block("farmland")));
                build.push_back(place(p, blocks::wheat()));
            };
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z) crop({x, 0, z});
            if (neighbor_activity)
                for (int x = 16; x < 32; ++x) crop({x, 0, 3});
            build.push_back(SetGameRule{"randomTickSpeed", 3});
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
        require(run(11, false) == base, "the same world grows the same");
        require(run(11, true) != base, "another chunk's random ticks shift the level's generators");
        require(run(12, false) != base, "seed changes the outcome");
    });

    suite.run("player actions resolve against the state earlier commands left", [] {
        Sim s;
        const circuit::contracts::PlayerKey alex{1, 2};
        std::vector<CommandBody> commands;
        std::vector<std::uint64_t> expected; // 0 applied, else the error code, in admission order
        const auto code = [](ErrorCode e) { return static_cast<std::uint64_t>(e); };
        // Holds `item` (0: nothing) in the first hotbar slot, then clicks.
        const auto use = [&](BlockPos pos, D face, std::uint16_t item, ErrorCode outcome, std::uint8_t reach = 3, bool sneaking = false,
                             D horizontal = D::south) {
            commands.push_back(circuit::contracts::SetPlayerSlot{alex, 36, {item, static_cast<std::uint8_t>(item ? 1 : 0), 0}});
            expected.push_back(0);
            commands.push_back(circuit::contracts::PlayerUse{alex, 36, pos, face, horizontal, horizontal, sneaking, false, reach, 3});
            expected.push_back(outcome == ErrorCode{} ? 0 : code(outcome));
        };
        const ErrorCode ok{};
        commands.push_back(SetBlock{{1, 0, 1}, blocks::stone().raw(), {}});
        expected.push_back(0);
        use({1, 0, 1}, D::up, items::stone, ok);                 // beside: (1,1,1)
        use({1, 1, 1}, D::up, items::redstone, ok);              // (1,2,1): dust on the stone placed just before
        use({1, 0, 1}, D::east, items::lever, ok);               // (2,0,1), a wall lever on the stone
        use({2, 0, 1}, D::up, items::stone, ok);                 // uses the new lever
        use({2, 0, 1}, D::up, items::stone, ok, 3, true);        // sneaking with an item: places on top of it instead
        use({1, 0, 1}, D::up, items::stone, ErrorCode::occupied);  // (1,1,1) is taken
        use({1, 1, 1}, D::north, items::stone, ErrorCode::out_of_reach, 1); // the neighbor is not reached
        use({5, 0, 5}, D::up, items::redstone, ErrorCode::unsupported); // replaces air at (5,0,5): nothing below
        use({6, 0, 6}, D::up, items::lever, ErrorCode::unsupported);    // a lever cannot replace air
        use({1, 0, 1}, D::up, 0, ErrorCode::not_usable);                // empty hand on stone
        use({1, 0, 1}, D::up, items::diamond, ErrorCode::not_usable);   // an item that places nothing
        commands.push_back(circuit::contracts::PlayerBreak{alex, {7, 0, 7}, false}); // air
        expected.push_back(code(ErrorCode::not_breakable));
        commands.push_back(circuit::contracts::PlayerBreak{alex, {1, 0, 1}, false});
        expected.push_back(0);
        s.tick(commands);
        std::vector<std::uint64_t> outcomes;
        for (const auto& r : s.last) {
            if (r.kind == TraceKind::command_applied) outcomes.push_back(0);
            if (r.kind == TraceKind::command_rejected) outcomes.push_back(r.b);
        }
        require(outcomes == expected, "outcomes in admission order");
        // Breaking the stone takes the lever hanging on it, which was used
        // earlier in the tick; its drop spawns. The stone above stays.
        require(s.at({1, 0, 1}).is_air() && s.at({2, 0, 1}).is_air(), "support removed");
        require(s.at({1, 1, 1}).is(BlockType::stone) && s.at({1, 2, 1}).is(BlockType::redstone_wire), "blocks that need no support stay");
        std::size_t lever_uses = 0;
        for (const auto& r : s.last)
            if (r.kind == TraceKind::block_changed && r.pos == BlockPos{2, 0, 1} && BlockState::decode(static_cast<std::uint32_t>(r.b))->flag())
                ++lever_uses;
        require(lever_uses == 1, "the lever placed in the same tick was used");
        require(s.oracle.dump().overworld().entities.size() == 1, "the lever dropped");
        require(s.at({2, 1, 1}).is(BlockType::stone), "sneaking placed instead of using");

        // Survival placement uses the item up; a water bucket, used along the
        // view, leaves a bucket (a click on a block with it does nothing).
        const circuit::contracts::PlayerKey sam{3, 4};
        s.tick({SetBlock{{8, 0, 8}, blocks::stone().raw(), {}}, SetBlock{{9, 0, 9}, blocks::stone().raw(), {}},
                circuit::contracts::SetPlayerSlot{sam, 36, {items::stone, 2, 0}},
                circuit::contracts::SetPlayerSlot{sam, 37, {items::water_bucket, 1, 0}},
                circuit::contracts::PlayerUse{sam, 36, {8, 0, 8}, D::up, D::south, D::south, false, true, 3, 3},
                circuit::contracts::PlayerUse{sam, 37, {9, 0, 9}, D::up, D::south, D::south, false, true, 3, 3},
                circuit::contracts::PlayerUseItem{sam, 37, {9.5, 2.62, 10.5}, 0.0f, 60.0f, 4.5, true,
                                                  circuit::contracts::BlockHit{{9, 0, 9}, D::up, 0.5f, 1.0f, 0.5f}},
                circuit::contracts::PlayerUse{sam, 38, {9, 0, 9}, D::up, D::south, D::south, false, true, 3, 3}});
        const auto d = s.oracle.dump();
        const auto inventory = std::ranges::find(d.players, sam, &PlayerInventory::player);
        require(inventory != d.players.end(), "the survival player has an inventory");
        require(inventory->slots[36] == ItemStack{items::stone, 1, 0} && inventory->slots[37] == ItemStack{items::bucket, 1, 0},
                "one stone used, the bucket emptied");
        require(s.at({8, 1, 8}).is(BlockType::stone) && s.at({9, 1, 9}).is(BlockType::water), "both placed");
        require(of_kind(s.last, TraceKind::player_slot_changed).size() == 4, "every slot change is recorded");
    });

    suite.run("dig progress follows tools, ground and water", [] {
        using circuit::gameplay::destroy_progress;
        using circuit::gameplay::fluid_height;
        const auto near = [](float a, float b) { return std::abs(a - b) < 1e-6f; };
        // Stone (hardness 1.5): by hand 1/150 per tick, with a wooden pickaxe 2/45.
        require(near(destroy_progress(blocks::stone(), {}, true), 1.0f / 150.0f), "by hand");
        require(near(destroy_progress(blocks::stone(), {items::wooden_pickaxe, 1, 0}, true), 2.0f / 45.0f), "wooden pickaxe");
        // Five times slower off the ground, and again with the eyes in water.
        require(near(destroy_progress(blocks::stone(), {items::wooden_pickaxe, 1, 0}, false), 2.0f / 225.0f), "off the ground");
        require(near(destroy_progress(blocks::stone(), {items::wooden_pickaxe, 1, 0}, false, true), 2.0f / 1125.0f), "under water");
        // Obsidian needs a diamond pickaxe; dust breaks at once.
        require(near(destroy_progress(blocks::obsidian(), {items::iron_pickaxe, 1, 0}, true), 6.0f / 50.0f / 100.0f), "iron on obsidian");
        require(destroy_progress(blocks::wire(), {}, false) >= 1.0f, "instant");
        // A source is 8/9 high, a flowing cell at distance 3 5/9, water under water 1.
        require(near(fluid_height(blocks::water_source(), blocks::air()), 8.0f / 9.0f), "source height");
        require(near(fluid_height(blocks::water_flowing(3), blocks::air()), 5.0f / 9.0f), "flowing height");
        require(near(fluid_height(blocks::water_flowing(3), blocks::water_source()), 1.0f), "covered");
    });

    suite.run("broken containers drop their contents, and survival breaking harvests", [] {
        Sim s;
        const circuit::contracts::PlayerKey alex{5, 6};
        s.tick({place({0, 0, 0}, blocks::chest()), place({2, 0, 0}, blocks::stone()), place({4, 0, 0}, blocks::obsidian()),
                insert({0, 0, 0}, 0, items::iron_ingot, 5), insert({0, 0, 0}, 3, items::redstone, 64),
                circuit::contracts::SetPlayerSlot{alex, 36, {items::stone_pickaxe, 1, 130}}});
        // Creative breaking drops the chest's contents, not the chest: each
        // slot in order, in parts of 10 to 30 items that can be picked up at
        // once, drawing from the level random for all 27 slots.
        const auto before = s.oracle.dump().overworld().random;
        s.tick({circuit::contracts::PlayerBreak{alex, {0, 0, 0}, false, 36}});
        auto d = s.oracle.dump();
        const auto& drops = d.overworld().entities;
        require(drops.size() >= 4 && drops[0].stack == ItemStack{items::iron_ingot, 5, 0}, "the first slot's stack first, in one part");
        unsigned redstone = 0;
        for (std::size_t i = 1; i < drops.size(); ++i) {
            require(drops[i].stack.item == items::redstone && drops[i].stack.count <= 30 && (drops[i].stack.count >= 10 || i + 1 == drops.size()),
                    "then the redstone, in parts of 10 to 30");
            redstone += drops[i].stack.count;
        }
        require(redstone == 64, "nothing lost");
        require(std::ranges::all_of(drops, [](const auto& e) { return e.pickup_delay == 0 && e.velocity.y > 0.0; }), "launched, at once collectable");
        auto replay = circuit::gameplay::LevelRandom::from_state(before);
        for (int slot = 0; slot < 27 * 3; ++slot) (void)replay.next_double();          // a point in the cell for each slot
        for (std::size_t part = 0; part < drops.size(); ++part) {                   // per part: its size, the default velocity,
            (void)replay.next_int(21);                                              // the velocity around (0, 0.2, 0)
            for (int i = 0; i < 8; ++i) (void)replay.next_double();
        }
        require(replay.state() == d.overworld().random, "all draws accounted for");
        const auto scattered = drops.size();
        // Survival with a stone pickaxe: stone gives cobblestone, obsidian nothing; the
        // pickaxe (131 uses) breaks on its last use.
        s.tick({circuit::contracts::PlayerBreak{alex, {2, 0, 0}, true, 36}});
        d = s.oracle.dump();
        require(d.overworld().entities.size() == scattered + 1 && d.overworld().entities.back().stack.item == items::cobblestone, "cobblestone");
        require(d.players.empty(), "the pickaxe broke");
        s.tick({circuit::contracts::PlayerBreak{alex, {4, 0, 0}, true, 36}});
        require(s.oracle.dump().overworld().entities.size() == scattered + 1 && s.at({4, 0, 0}).is_air(), "obsidian by hand drops nothing");
    });

    suite.run("container clicks follow the target version's menus", [] {
        using circuit::contracts::ContainerClick;
        using circuit::contracts::PlayerKey;
        Sim s;
        const PlayerKey alex{7, 8};
        const BlockPos chest{0, 0, 0};
        s.tick({place(chest, blocks::chest()), insert(chest, 0, items::redstone, 5), insert(chest, 1, items::stone, 64),
                insert(chest, 2, items::redstone, 60)});
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode, bool in_chest = true) {
            return ContainerClick{alex, in_chest ? std::optional(chest) : std::nullopt, slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            const auto it = std::ranges::find(d.players, alex, &PlayerInventory::player);
            if (it != d.players.end()) return *it;
            PlayerInventory none;
            none.player = alex;
            return none;
        };
        const auto chest_slot = [&](std::size_t i) { return block_entity_at(s.oracle.dump(), chest)->slots[i]; };
        // Chest menu: slots 0-26 the chest, 27-53 the main inventory (window 9-35), 54-62 the hotbar (36-44).
        s.tick({click(0, 1, 0)}); // right click: half of 5, rounded up
        require(player().cursor == ItemStack{items::redstone, 3, 0} && chest_slot(0).count == 2, "right click takes half");
        s.tick({click(54, 1, 0)}); // right click on an empty hotbar slot: one
        require(player().slots[36] == ItemStack{items::redstone, 1, 0} && player().cursor.count == 2, "right click puts one");
        s.tick({click(54, 0, 0)}); // left click on the same item: all of the cursor
        require(player().slots[36].count == 3 && player().cursor.empty(), "left click puts all");
        s.tick({click(1, 0, 1)}); // shift-click from the chest: into the inventory from the hotbar end
        require(chest_slot(1).empty() && player().slots[44] == ItemStack{items::stone, 64, 0}, "shift-click fills the last hotbar slot");
        s.tick({click(62, 0, 1)}); // and back: into the first empty chest slot
        require(chest_slot(1) == ItemStack{items::stone, 64, 0} && player().slots[44].empty(), "shift-click back");
        s.tick({click(2, 4, 2)}); // swap chest slot 2 with hotbar slot 4
        require(player().slots[40] == ItemStack{items::redstone, 60, 0} && chest_slot(2).empty(), "number-key swap");
        s.tick({click(58, 0, 4)}); // throw one from hotbar slot 4
        require(player().slots[40].count == 59 && s.oracle.dump().overworld().entities.size() == 1, "throw one");
        // Drag 59 evenly over three empty chest slots: 19 each, 2 stay on the cursor.
        s.tick({click(58, 0, 0), click(-999, 0, 5), click(3, 1, 5), click(4, 1, 5), click(5, 1, 5), click(-999, 2, 5)});
        require(chest_slot(3).count == 19 && chest_slot(4).count == 19 && chest_slot(5).count == 19 && player().cursor.count == 2,
                "even drag");
        // Double click (on the slot the first click emptied) collects matching
        // items onto the cursor, full stacks last.
        s.tick({click(6, 0, 6)});
        require(player().cursor.count == 64, "collect to a full stack");
        s.tick({click(-999, 1, 0)}); // right click outside: throws one
        require(player().cursor.count == 63 && s.oracle.dump().overworld().entities.size() == 2, "one thrown outside");
        s.tick({circuit::contracts::CloseContainer{alex, at_block(0.5, 2, 0.5), {}}});
        require(player().cursor.empty(), "closing returns the cursor to the inventory");
        // The own window: armor slots take armor only, the crafting result nothing.
        s.tick({circuit::contracts::SetPlayerSlot{alex, 9, {items::diamond_helmet, 1, 0}}, click(9, 0, 1, false)});
        require(player().slots[5].item == items::diamond_helmet, "shift-click wears a helmet");
        s.tick({click(37, 0, 0, false), click(6, 0, 0, false), click(0, 0, 0, false)});
        require(player().slots[6].empty() && player().slots[0].empty(), "stone is no chestplate and the result takes nothing");
    });

    suite.run("random container clicks conserve items", [] {
        using circuit::contracts::ContainerClick;
        Sim s;
        const circuit::contracts::PlayerKey alex{9, 9};
        const BlockPos left{0, 0, 0}, right{1, 0, 0};
        // Thrown items land on the stone at (3, 0, 3) instead of falling out of the world.
        std::vector<CommandBody> setup{place(left, blocks::chest(D::north, blocks::ChestType::left)),
                                       place(right, blocks::chest(D::north, blocks::ChestType::right)), place({3, 0, 3}, blocks::stone())};
        std::mt19937 rng(23);
        const std::uint16_t kinds[]{items::redstone, items::stone, items::water_bucket, items::bucket, items::diamond_helmet};
        for (std::uint8_t i = 0; i < 27; i += 2) {
            const auto item = kinds[i % 5];
            setup.push_back(insert(i % 2 ? left : right, i, item, std::min<std::uint8_t>(items::max_stack(item), 1 + i)));
        }
        for (std::uint8_t slot = 9; slot < 45; slot += 3) {
            const auto item = kinds[slot % 5];
            setup.push_back(circuit::contracts::SetPlayerSlot{alex, slot, {item, items::max_stack(item), 0}});
        }
        s.tick(setup);
        const auto total = [&] {
            std::map<std::uint16_t, std::size_t> n;
            const auto d = s.oracle.dump();
            for (const auto& p : d.players) {
                n[p.cursor.item] += p.cursor.count;
                for (const auto& st : p.slots) n[st.item] += st.count;
            }
            for (const auto& [pos, be] : d.overworld().block_entities)
                for (const auto& st : be.slots) n[st.item] += st.count;
            for (const auto& e : d.overworld().entities) n[e.stack.item] += e.stack.count;
            n.erase(0);
            return n;
        };
        const auto before = total();
        for (int round = 0; round < 3000; ++round) {
            const bool own = std::uniform_int_distribution<int>(0, 4)(rng) == 0;
            const auto size = own ? 46 : 54 + 36;
            const auto mode = static_cast<std::uint8_t>(std::uniform_int_distribution<int>(0, 6)(rng));
            const auto slot = std::uniform_int_distribution<int>(0, 15)(rng) == 0 ? std::int16_t{-999}
                                                                                   : static_cast<std::int16_t>(std::uniform_int_distribution<int>(0, size - 1)(rng));
            const auto button = static_cast<std::uint8_t>(mode == 2 ? std::uniform_int_distribution<int>(0, 9)(rng)
                                                        : mode == 5 ? std::uniform_int_distribution<int>(0, 10)(rng)
                                                                    : std::uniform_int_distribution<int>(0, 1)(rng));
            // Survival clicks only: a creative clone creates items by design.
            std::vector<CommandBody> c{ContainerClick{alex, own ? std::nullopt : std::optional(left), slot, button == 9 ? std::uint8_t{40} : button, mode,
                                                      false, at_block(3.5, 1, 3.5), {}}};
            if (round % 97 == 0) c.push_back(circuit::contracts::CloseContainer{alex, at_block(3.5, 1, 3.5), {}});
            s.tick(c);
            if (total() != before) {
                std::string diff;
                const auto now = total();
                for (const auto& [item, n] : before)
                    if (!now.contains(item) || now.at(item) != n) diff += std::format(" item {} {}->{}", item, n, now.contains(item) ? now.at(item) : 0);
                for (const auto& [item, n] : now)
                    if (!before.contains(item)) diff += std::format(" new item {} {}", item, n);
                require(false, std::format("conserved after click {} (mode {} button {} slot {}){}", round, mode, button, slot, diff));
            }
            const auto d = s.oracle.dump();
            for (const auto& p : d.players)
                for (const auto& st : p.slots) require(st.empty() || st.count <= items::max_stack(st.item), "slot limits hold");
            for (const auto& [pos, be] : d.overworld().block_entities)
                for (const auto& st : be.slots) require(st.empty() || st.count <= items::max_stack(st.item), "container limits hold");
        }
    });

    suite.run("a dead player's items drop around them, or are lost below the world", [] {
        using circuit::contracts::ContainerClick;
        using circuit::contracts::PlayerDeath;
        using circuit::contracts::SetPlayerSlot;
        Sim s;
        const circuit::contracts::PlayerKey alex{3, 4};
        auto setup = floor(0, 3, 0, 3);
        for (const auto& [slot, stack] : std::vector<std::pair<std::uint8_t, ItemStack>>{
                 {40, {items::redstone, 64, 0}}, {12, {items::stone, 5, 0}}, {5, {items::diamond_helmet, 1, 30}},
                 {8, {items::iron_boots, 1, 0}}, {45, {items::shield, 1, 0}}, {2, {items::wheat, 3, 0}}, {37, {items::lever, 7, 0}}})
            setup.push_back(SetPlayerSlot{alex, slot, stack});
        s.tick(setup);
        // Picking up the hotbar's levers leaves them carried.
        s.tick({ContainerClick{alex, std::nullopt, 37, 0, 0, false, {}, {}}});
        const auto position = at_block(1.5, 1.32, 1.5);
        s.tick({PlayerDeath{alex, position}});
        const auto d = s.oracle.dump();
        require(d.players.empty(), "the inventory is empty");
        // Hotbar, main inventory, armor from the feet up, off hand, crafting grid, carried stack.
        const std::vector<ItemStack> expected{{items::redstone, 64, 0}, {items::stone, 5, 0}, {items::iron_boots, 1, 0},
                                              {items::diamond_helmet, 1, 30}, {items::shield, 1, 0}, {items::wheat, 3, 0},
                                              {items::lever, 7, 0}};
        require(d.overworld().entities.size() == expected.size(), std::format("{} entities", d.overworld().entities.size()));
        for (std::size_t i = 0; i < expected.size(); ++i) {
            require(d.overworld().entities[i].stack == expected[i], std::format("drop {} in order", i));
            require(d.overworld().entities[i].pickup_delay > 30, "with the thrown pickup delay");
        }
        // They spread in different directions.
        std::set<std::pair<double, double>> directions;
        for (const auto& e : d.overworld().entities) directions.insert({e.velocity.x, e.velocity.z});
        require(directions.size() == expected.size(), "spread");
        // Below the world nothing can drop: the items are lost, as in the void.
        s.tick({SetPlayerSlot{alex, 36, {items::stone, 2, 0}}});
        s.tick({PlayerDeath{alex, at_block(1.5, -200, 1.5)}});
        require(s.oracle.dump().players.empty() && s.oracle.dump().overworld().entities.size() == expected.size(), "lost below the world");
    });

    suite.run("ticket bounds are checked without overflow", [] {
        const auto& limits = bundle().limits;
        const std::int32_t limit = world_border / 16;
        const auto accepted = [&](ChunkPos center, std::uint8_t radius, std::uint8_t level = 31) {
            TicketSet tickets;
            return tickets.add({1, center, radius, TicketType::forced, level}, DimensionId::overworld, 0, limits).has_value();
        };
        for (const auto extreme : {INT32_MIN, INT32_MAX})
            for (const std::uint8_t radius : {std::uint8_t{0}, limits.max_ticket_radius})
                require(!accepted({extreme, 0}, radius) && !accepted({0, extreme}, radius), "extreme centers");
        // A level-31 ticket of radius r loads r + 2 chunks (levels 32 and 33
        // beyond its square); every loaded chunk stays inside the border.
        for (const std::uint8_t radius : {std::uint8_t{0}, limits.max_ticket_radius}) {
            const std::int32_t reach = radius + 2;
            const std::int32_t low = -limit + reach, high = limit - 1 - reach;
            require(accepted({low, 0}, radius) && accepted({high, 0}, radius) && accepted({0, low}, radius)
                        && accepted({0, high}, radius),
                    std::format("radius {} endpoints accepted", radius));
            require(!accepted({low - 1, 0}, radius) && !accepted({high + 1, 0}, radius) && !accepted({0, low - 1}, radius)
                        && !accepted({0, high + 1}, radius),
                    std::format("radius {} beyond the endpoints rejected", radius));
        }
        // A lower level loads further, and is bounded the same way: radius 1
        // at level 0 loads 34 chunks out.
        const std::int32_t far = limits.max_load_radius;
        require(accepted({-limit + far, 0}, 1, 0) && !accepted({-limit + far - 1, 0}, 1, 0)
                    && accepted({0, limit - 1 - far}, 1, 0) && !accepted({0, limit - far}, 1, 0),
                "low level endpoints");
        TicketSet tickets;
        const auto add = [&](AddTicket t, Tick now = 0) { return tickets.add(t, DimensionId::overworld, now, limits); };
        require(add({1, {0, 0}, static_cast<std::uint8_t>(limits.max_ticket_radius + 1)}).error() == ErrorCode::out_of_bounds,
                "radius limit");
        require(add({1, {0, 0}, 2, TicketType::forced, 0}).error() == ErrorCode::out_of_bounds, "load radius limit");
        require(add({1, {0, 0}, 0, TicketType::forced, 45}).error() == ErrorCode::invalid_argument, "level limit");
        require(add({1, {0, 0}, 0, static_cast<TicketType>(ticket_type_count)}).error() == ErrorCode::invalid_argument,
                "unknown type");
        require(add({1, {0, 0}, 0, TicketType::portal, 30, 2}, UINT64_MAX - 1).error() == ErrorCode::invalid_argument,
                "lifetime overflow");
        require(tickets.list().empty(), "nothing added");
        require(add({2, {0, 0}, 2}).has_value(), "forced ticket");
        auto l = tickets.levels();
        const auto at = [&](ChunkPos c) { return l.at({DimensionId::overworld, c}); };
        require(l.size() == 81 && at({2, 0}) == ChunkLevels{31, 31} && at({3, 0}) == ChunkLevels{32, 32}
                    && at({4, 4}) == ChunkLevels{33, 33} && !l.contains({DimensionId::overworld, {5, 0}}),
                "level 31 in the square, one more per chunk beyond, loaded to 33");
        require(status_of(31) == ChunkLevel::entity_ticking && status_of(0) == ChunkLevel::entity_ticking
                    && status_of(32) == ChunkLevel::block_ticking && status_of(33) == ChunkLevel::border
                    && status_of(34) == ChunkLevel::unloaded,
                "statuses");
        // A player's loading ticket loads without simulating; their simulation
        // ticket simulates the chunks it loads, without loading more.
        require(add({3, {20, 0}, 2, TicketType::player_loading, 31}).has_value()
                    && add({4, {20, 0}, 0, TicketType::player_simulation, 30}).has_value(),
                "player tickets");
        l = tickets.levels();
        require(at({20, 0}) == ChunkLevels{31, 31} && at({21, 0}) == ChunkLevels{31, 31} && at({22, 0}) == ChunkLevels{31, 32}
                    && at({23, 0}) == ChunkLevels{32, 33} && at({24, 0}) == ChunkLevels{33, 34}
                    && !l.contains({DimensionId::overworld, {25, 0}}),
                "loading and simulation levels");
        require(add({5, {-20, 0}, 0, TicketType::player_simulation, 27}).has_value()
                    && !tickets.levels().contains({DimensionId::overworld, {-20, 0}}),
                "a simulation ticket alone loads nothing");
        // Lifetimes end at the command phase of the tick they reach.
        require(add({6, {40, 0}, 0, TicketType::post_teleport, 32, 5}, 10).has_value(), "post-teleport ticket");
        require(tickets.expire(14, DimensionId::overworld).empty() && tickets.expire(15, DimensionId::the_nether).empty()
                    && tickets.expire(15, DimensionId::overworld) == std::vector<std::uint32_t>{6},
                "expires after its lifetime, in its level");
        require(tickets.keeps_active(DimensionId::overworld) && !tickets.keeps_active(DimensionId::the_nether),
                "forced tickets keep their level active");
    });

    suite.run("light properties of states", [] {
        // The registry's light table (tools/compile_block_registry.py), as
        // modeled states and registry states see it.
        const auto info = [](BlockState s) { return light_info(s); };
        require(info(blocks::lamp().with_flag(true)) == LightInfo{15, 15, 0} && info(blocks::lamp()) == LightInfo{0, 15, 0},
                "a lit lamp emits 15");
        require(info(blocks::glass()) == LightInfo{0, 0, 0} && info(blocks::stone()).dampening == 15
                    && info(blocks::water_source()).dampening == 1 && info(blocks::air()) == LightInfo{},
                "glass, stone, water, air");
        require(info(registry_block("torch")).emission == 14 && info(registry_block("glowstone")) == LightInfo{15, 15, 0},
                "torch and glowstone");
        require(info(registry_block("sea_pickle", {{"pickles", "3"}, {"waterlogged", "true"}})) == LightInfo{12, 1, 0}
                    && info(registry_block("sea_pickle", {{"pickles", "3"}, {"waterlogged", "false"}})) == LightInfo{0, 0, 0},
                "sea pickles shine in water");
        require(info(registry_block("respawn_anchor", {{"charges", "2"}})).emission == 7
                    && info(registry_block("candle", {{"candles", "4"}, {"lit", "true"}, {"waterlogged", "false"}})).emission == 12,
                "anchors and candles");
        const auto bottom = info(registry_block("oak_slab", {{"type", "bottom"}, {"waterlogged", "false"}}));
        const auto top = info(registry_block("oak_slab", {{"type", "top"}, {"waterlogged", "false"}}));
        require(bottom.shape != 0 && top.shape != 0 && bottom.dampening == 0
                    && info(registry_block("oak_slab", {{"type", "double"}, {"waterlogged", "false"}})).dampening == 15,
                "slabs occlude by shape");
        // Light from above into a bottom slab, and between a bottom and a top slab side by side.
        require(light_passed(LightKind::block, 10, {}, bottom, D::down) == 9 && light_passed(LightKind::block, 10, {}, bottom, D::up) == 0,
                "a bottom slab lets light in from above, not from below");
        require(light_passed(LightKind::block, 10, bottom, top, D::east) == 0 && light_passed(LightKind::block, 10, bottom, bottom, D::east) == 9,
                "a bottom and a top slab close the gap between them");
        require(light_passed(LightKind::sky, 15, {}, info(blocks::glass()), D::down) == 15
                    && light_passed(LightKind::sky, 15, {}, info(blocks::water_source()), D::down) == 14
                    && light_passed(LightKind::sky, 15, {}, {}, D::east) == 14,
                "sky light passes straight down undiminished only through cells that dampen nothing");
    });

    suite.run("light fields follow changes to the values computed from scratch", [] {
        // A small world of 3x3 columns, 48 high, with random blocks.
        struct World final : LightWorld {
            std::map<BlockPos, BlockState> cells;
            std::set<ChunkPos> columns;
            std::map<BlockPos, BlockState> before; // changes not followed yet
            BlockState block(BlockPos p) const override {
                if (!columns.contains(chunk_of(p))) return BlockState::unloaded_sentinel();
                if (const auto it = before.find(p); it != before.end()) return it->second;
                const auto it = cells.find(p);
                return it == cells.end() ? blocks::air() : it->second;
            }
            bool loaded(ChunkPos c) const override { return columns.contains(c); }
        } world;
        const std::vector<BlockState> palette{
            blocks::stone(), blocks::glass(), blocks::water_source(), blocks::lamp().with_flag(true), registry_block("glowstone"),
            registry_block("torch"), registry_block("oak_leaves", {{"distance", "7"}, {"persistent", "true"}, {"waterlogged", "false"}}),
            registry_block("oak_slab", {{"type", "bottom"}, {"waterlogged", "false"}}),
            registry_block("oak_slab", {{"type", "top"}, {"waterlogged", "false"}}),
            registry_block("oak_stairs", {{"facing", "east"}, {"half", "bottom"}, {"shape", "straight"}, {"waterlogged", "false"}}),
            registry_block("snow", {{"layers", "3"}}), registry_block("sea_pickle", {{"pickles", "2"}, {"waterlogged", "true"}}),
            registry_block("ice"), registry_block("farmland", {{"moisture", "0"}})};
        std::mt19937 rng(17);
        const auto random_pos = [&] {
            return BlockPos{static_cast<std::int32_t>(rng() % 48) - 16, static_cast<std::int32_t>(rng() % 40),
                            static_cast<std::int32_t>(rng() % 48) - 16};
        };
        const auto random_state = [&] { return rng() % 3 == 0 ? blocks::air() : palette[rng() % palette.size()]; };
        for (int i = 0; i < 2500; ++i) world.cells[random_pos()] = random_state();
        // A roof with holes, so that sky light spreads underneath.
        for (int x = -16; x < 32; ++x)
            for (int z = -16; z < 32; ++z)
                if ((x + 2 * z) % 7 != 0) world.cells[{x, 42, z}] = blocks::stone();
        LightField field(0, 48);
        const auto check = [&](std::string_view label) {
            const ReferenceLight reference(world, world.columns, 0, 47);
            for (const auto c : world.columns)
                for (std::int32_t y = 0; y < 48; ++y)
                    for (int i = 0; i < 256; ++i) {
                        const BlockPos p{c.x * 16 + (i & 15), y, c.z * 16 + (i >> 4)};
                        for (const auto kind : {LightKind::block, LightKind::sky})
                            if (field.get(kind, p) != reference.get(kind, p))
                                throw std::runtime_error(std::format("{}: {} light at ({},{},{}): {} instead of {}", label,
                                                                     kind == LightKind::block ? "block" : "sky", p.x, p.y, p.z,
                                                                     field.get(kind, p), reference.get(kind, p)));
                    }
        };
        std::vector<ChunkPos> order;
        for (int x = -1; x <= 1; ++x)
            for (int z = -1; z <= 1; ++z) order.push_back({x, z});
        std::ranges::shuffle(order, rng);
        for (const auto c : order) {
            world.columns.insert(c);
            field.load(c, world);
            check("columns loaded one by one");
        }
        for (int step = 0; step < 300; ++step) {
            const auto p = random_pos();
            const auto old = world.block(p);
            world.cells[p] = random_state();
            field.change(p, old, world);
            if (step % 60 == 59) check(std::format("after {} changes", step + 1));
        }
        // Several changes made before the field follows them one by one, as
        // the engine's light phase does.
        for (int batch = 0; batch < 4; ++batch) {
            std::map<BlockPos, BlockState> before;
            for (int k = 0; k < 25; ++k) {
                const auto p = random_pos();
                before.emplace(p, world.block(p));
                world.cells[p] = random_state();
            }
            world.before = before;
            while (!world.before.empty()) {
                const auto node = world.before.extract(world.before.begin());
                field.change(node.key(), node.mapped(), world);
            }
            check(std::format("batch {}", batch));
        }
        for (const auto c : {ChunkPos{0, 0}, ChunkPos{1, -1}}) {
            world.columns.erase(c);
            field.unload(c, world);
            check("a column unloaded");
        }
        world.columns.insert({0, 0});
        field.load({0, 0}, world);
        check("loaded again");
        require(true, "every check passed");
    });

    suite.run("game rules are world state", [] {
        Sim s;
        s.tick({SetGameRule{"randomTickSpeed", -5}, SetGameRule{"doDaylightCycle", 0}, SetGameRule{"spawnChunkRadius", 32},
                SetGameRule{"spawnChunkRadius", 33}, SetGameRule{"doDaylightCycle", 2}, SetGameRule{"noSuchRule", 1},
                SetGameRule{"minecartMaxSpeed", 8}});
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == 4 && rejected[0].b == static_cast<std::uint64_t>(ErrorCode::out_of_bounds)
                    && rejected[1].b == static_cast<std::uint64_t>(ErrorCode::invalid_argument)
                    && rejected[2].b == static_cast<std::uint64_t>(ErrorCode::invalid_argument)
                    && rejected[3].b == static_cast<std::uint64_t>(ErrorCode::invalid_argument),
                "bounds, booleans, unknown and experimental rules");
        const auto d = s.oracle.dump();
        require(d.game_rules == std::map<std::string, std::int32_t>{{"doDaylightCycle", 0}, {"randomTickSpeed", -5}, {"spawnChunkRadius", 32}},
                "the rules set");
        // No random ticks at speed 0 or less.
        std::vector<CommandBody> crops;
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) crops.push_back(place({x, 0, z}, blocks::wheat()));
        s.tick(crops);
        s.until(200);
        bool grown = false;
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) grown |= s.at({x, 0, z}).level() > 0;
        require(!grown, "randomTickSpeed -5 ticks nothing");
    });

    suite.run("the day time advances after the commands while doDaylightCycle is on", [] {
        Sim s;
        s.tick({});
        require(s.oracle.dump().day_time == static_cast<std::int64_t>(s.oracle.time()), "a new world's day starts with it");
        s.tick({SetDayTime{1000}});
        require(s.oracle.dump().day_time == 1001, "set, then advanced in the same tick");
        s.tick({SetGameRule{"doDaylightCycle", 0}, SetDayTime{-1}});
        require(of_kind(s.last, TraceKind::command_rejected).size() == 1 && s.oracle.dump().day_time == 1001,
                "stopped in the tick the rule changes; a negative time is refused");
        s.tick({SetDayTime{4294967318}});
        s.until(s.oracle.dump().time + 20);
        require(s.oracle.dump().day_time == 4294967318, "frozen");
        s.tick({SetGameRule{"doDaylightCycle", 1}});
        require(s.oracle.dump().day_time == 4294967319, "advancing again");
    });

    suite.run("the difficulty is the world's, set by commands", [] {
        Sim s;
        require(s.oracle.dump().difficulty == 0, "a new world is peaceful");
        s.tick({SetDifficulty{2}});
        require(s.oracle.dump().difficulty == 2 && of_kind(s.last, TraceKind::command_rejected).empty(), "normal");
        s.tick({SetDifficulty{4}});
        require(s.oracle.dump().difficulty == 2 && of_kind(s.last, TraceKind::command_rejected).size() == 1, "nothing above hard");
    });

    suite.run("fire burns near players on 1.21.8, or away from them with the game rule, and anywhere on 1.21.4", [] {
        const auto fire = registry_block("fire");
        const auto planks = registry_block("oak_planks");
        // Fire on stone beside planks; what is there after 300 ticks.
        const auto burn = [&](const VersionBundle& b, std::vector<CommandBody> setup) {
            Sim s(b);
            auto build = floor(-2, 2, -2, 2);
            build.push_back(place({1, 0, 0}, planks));
            build.insert(build.end(), setup.begin(), setup.end());
            s.tick(build);
            s.tick({place({0, 0, 0}, fire)});
            require(s.at({0, 0, 0}) == fire, "set");
            s.until(s.oracle.time() + 300);
            return std::pair{s.at({0, 0, 0}), s.at({1, 0, 0})};
        };
        const auto& newer = bundle();
        const auto& older = **BundleRegistry::select_for_simulation("java-1.21.4");
        require(burn(newer, {}) == std::pair{fire, planks}, "1.21.8 without a player: only its ticks rescheduled");
        for (const auto& [b, setup, label] : {std::tuple{&newer, std::vector<CommandBody>{PlayerPresence{{1, 1}, at_block(40.5, 0.0, 0.5)}}, "a player near"},
                                             std::tuple{&newer, std::vector<CommandBody>{SetGameRule{"allowFireTicksAwayFromPlayer", 1}}, "the game rule"},
                                             std::tuple{&older, std::vector<CommandBody>{}, "1.21.4"}})
            require(burn(*b, setup) != std::pair{fire, planks}, std::format("{}: it ages or burns", label));
        // A spectator does not count; doFireTick off stops it everywhere.
        require(burn(newer, {PlayerPresence{{1, 1}, at_block(0.5, 0.0, 0.5), true}}) == std::pair{fire, planks}, "a spectator");
        require(burn(older, {SetGameRule{"doFireTick", 0}}) == std::pair{fire, planks}, "doFireTick off");
    });

    suite.run("snow lies on its supports, dirt turns snowy, broken ice becomes water and frosted ice melts", [] {
        const circuit::contracts::PlayerKey alex{9, 10};
        const auto snow = registry_block("snow", {{"layers", "1"}});
        const auto frosted = registry_block("frosted_ice", {{"age", "0"}});
        Sim s;
        auto build = floor(-3, 3, -3, 3);
        for (const BlockPos p : {BlockPos{0, 0, 0}, BlockPos{2, 1, 0}, BlockPos{-2, 0, 0}, BlockPos{-2, 0, -2}}) build.push_back(place(p, registry_block("ice")));
        build.push_back(place({2, 0, -2}, registry_block("grass_block", {{"snowy", "false"}})));
        for (int x = -1; x <= 1; ++x) build.push_back(place({x, 0, 2}, frosted));
        s.tick(build);
        // Snow on ice and on grass; dirt under snow is snowy.
        s.tick({place({-2, 1, -2}, snow), place({2, 1, -2}, snow), place({0, 0, -3}, snow)});
        require(s.at({2, 0, -2}) == registry_block("grass_block", {{"snowy", "true"}}), "snowy under snow");
        // An update beside them: the layer on ice goes, the one on stone stays.
        s.tick({place({-1, 1, -2}, blocks::stone()), place({1, 0, -3}, blocks::stone())});
        require(s.at({-2, 1, -2}).is_air() && s.at({0, 0, -3}) == snow, "snow on ice breaks, on stone it stays");
        // Ice broken in survival: water over stone, air over air; in creative, air.
        s.tick({circuit::contracts::PlayerBreak{alex, {0, 0, 0}, true, 36}, circuit::contracts::PlayerBreak{alex, {2, 1, 0}, true, 36},
                circuit::contracts::PlayerBreak{alex, {-2, 0, 0}, false, 36}});
        require(s.at({0, 0, 0}) == blocks::water_source() && s.at({2, 1, 0}).is_air() && s.at({-2, 0, 0}).is_air(), "ice to water");
        // Frosted ice replaced: its neighbors, left without frosted neighbors, melt.
        s.tick({place({0, 0, 2}, blocks::stone())});
        require(s.at({-1, 0, 2}) == blocks::water_source() && s.at({1, 0, 2}) == blocks::water_source(), "frosted ice melts");
    });

    suite.run("random ticks visit chunks in the official servers' order", [] {
        // The order the official 1.21.8 server ticked sixteen forced chunks in
        // (every pick of farmland layers in them).
        std::vector<ChunkPos> forced;
        for (int x = 0; x < 4; ++x)
            for (int z = 0; z < 4; ++z) forced.push_back({x, z});
        const std::vector<ChunkPos> official{{0, 0}, {0, 2}, {1, 3}, {1, 2}, {3, 0}, {2, 0}, {2, 2}, {0, 3},
                                             {0, 1}, {3, 1}, {1, 0}, {1, 1}, {3, 3}, {3, 2}, {2, 3}, {2, 1}};
        require(circuit::gameplay::random_tick_order(forced) == official, "sixteen forced chunks");
        auto reversed = forced;
        std::ranges::reverse(reversed);
        require(circuit::gameplay::random_tick_order(reversed) == official, "whatever order they come in");
        require(circuit::gameplay::random_tick_order({{5, -3}, {0, 0}, {-2, 7}}).front() == ChunkPos{0, 0}, "the chunk at 0, 0 first");
    });

    suite.run("an arrow or a snowball tilts a big dripleaf at once, and it goes back 100 ticks later", [] {
        const auto leaf = registry_block("big_dripleaf", {{"facing", "north"}, {"tilt", "none"}});
        const auto tilted = registry_block("big_dripleaf", {{"facing", "north"}, {"tilt", "full"}});
        Sim s;
        auto build = floor(-4, 4, -4, 4);
        for (const int x : {0, 2}) {
            build.push_back(place({x, 0, 0}, registry_block("clay")));
            build.push_back(place({x, 1, 0}, registry_block("big_dripleaf_stem", {{"facing", "north"}})));
            build.push_back(place({x, 2, 0}, leaf));
        }
        s.tick(build);
        s.tick({SummonEntity{"minecraft:arrow", at_block(0.5, 5.0, 0.5), {0.0, -0.5, 0.0}, 0, 0},
                SummonEntity{"minecraft:snowball", at_block(2.5, 5.0, 0.5), {0.0, -0.5, 0.0}, 0, 0}});
        Tick hit = 0;
        for (int i = 0; i < 10 && !hit; ++i) {
            s.tick();
            if (s.at({0, 2, 0}) == tilted) hit = s.oracle.time();
        }
        require(hit != 0 && s.at({2, 2, 0}) == tilted, "both tilt fully when hit");
        s.until(hit + 99);
        require(s.at({0, 2, 0}) == tilted, "still tilted");
        s.tick();
        require(s.at({0, 2, 0}) == leaf && s.at({2, 2, 0}) == leaf, "back after 100 ticks");
    });

    suite.run("players stack snow layers and replace single ones", [] {
        const circuit::contracts::PlayerKey alex{11, 12};
        const auto snow_item = *circuit::gameplay::items::by_name("minecraft:snow");
        const auto layers = [](int n) { return registry_block("snow", {{"layers", std::to_string(n)}}); };
        Sim s;
        s.tick(floor(-4, 4, -4, 4));
        const auto use = [&](std::uint16_t item, BlockPos pos, D face) {
            s.tick({circuit::contracts::SetPlayerSlot{alex, 36, {item, 1, 0}},
                    circuit::contracts::PlayerUse{alex, 36, pos, face, D::north, D::north, false, false, 3, 3}});
        };
        // Snow on the floor, then on its top: a layer more up to eight, then a layer above.
        use(snow_item, {0, -1, 0}, D::up);
        for (int n = 2; n <= 8; ++n) {
            use(snow_item, {0, 0, 0}, D::up);
            require(s.at({0, 0, 0}) == layers(n), std::format("{} layers", n));
        }
        use(snow_item, {0, 0, 0}, D::up);
        require(s.at({0, 0, 0}) == layers(8) && s.at({0, 1, 0}) == layers(1), "a ninth goes above");
        // Clicked on a layer's side: a layer beside it.
        use(snow_item, {2, -1, 0}, D::up);
        use(snow_item, {2, 0, 0}, D::east);
        require(s.at({2, 0, 0}) == layers(1) && s.at({3, 0, 0}) == layers(1), "beside");
        // Stone and a snow block replace a single layer, not two.
        use(snow_item, {-2, -1, 0}, D::up);
        use(items::stone, {-2, 0, 0}, D::up);
        use(snow_item, {-2, -1, 2}, D::up);
        use(snow_item, {-2, 0, 2}, D::up);
        use(items::stone, {-2, 0, 2}, D::up);
        require(s.at({-2, 0, 0}) == blocks::stone() && s.at({-2, 1, 0}).is_air(), "stone replaces one layer");
        require(s.at({-2, 0, 2}) == layers(2) && s.at({-2, 1, 2}) == blocks::stone(), "but not two");
    });

    suite.run("flint and steel and fire charges set fire, light campfires, prime TNT and wear", [] {
        using circuit::contracts::PlayerUse;
        using circuit::contracts::SetPlayerSlot;
        using circuit::gameplay::block_notation;
        using D = Direction;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        const auto flint = item("minecraft:flint_and_steel"), charge = item("minecraft:fire_charge");
        const PlayerKey alex{4, 2};
        const auto campfire = [](std::string lit) {
            return registry_block("campfire", {{"facing", "north"}, {"lit", std::move(lit)}, {"signal_fire", "false"}, {"waterlogged", "false"}});
        };
        Sim s;
        s.tick({place({0, 0, 0}, blocks::stone()), place({2, 0, 0}, blocks::stone()), place({2, 1, 0}, blocks::stone()), place({4, 0, 0}, campfire("false")),
                place({6, 0, 0}, registry_block("tnt", {{"unstable", "false"}})), place({8, 0, 0}, registry_block("soul_soil"))});
        const auto held = [&] {
            const auto d = s.oracle.dump();
            return std::ranges::find(d.players, alex, &PlayerInventory::player)->slots[36];
        };
        const auto use = [&](BlockPos pos, bool survival = true) { return PlayerUse{alex, 36, pos, D::up, D::south, D::south, false, survival, 3, 3}; };
        s.tick({SetPlayerSlot{alex, 36, {flint, 1, 62}}});
        s.tick({use({0, 0, 0})});
        require(block_notation(s.at({0, 1, 0})).starts_with("minecraft:fire[age=0") && held().damage == 63, "fire on stone, worn by one");
        s.tick({use({2, 0, 0})});
        require(s.at({2, 1, 0}) == blocks::stone() && held().damage == 63, "the cell taken: nothing, and no wear");
        s.tick({use({4, 0, 0})});
        require(s.at({4, 0, 0}) == campfire("true") && held().empty(), "a campfire lit, and the last use breaks it");
        s.tick({SetPlayerSlot{alex, 36, {charge, 2}}});
        s.tick({use({8, 0, 0})});
        require(block_notation(s.at({8, 1, 0})) == "minecraft:soul_fire" && held().count == 1, "soul fire on soul soil, a charge used up");
        s.tick({use({6, 0, 0}, false)});
        const auto d = s.oracle.dump();
        require(s.at({6, 0, 0}).is_air() && held().count == 1
                    && std::ranges::any_of(d.overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::primed_tnt; }),
                "TNT primed; in creative mode nothing is used up");
    });

    suite.run("shears carve pumpkins, empty full hives, disarm tripwire, cut fast and wear on every block", [] {
        using circuit::contracts::PlayerBreak;
        using circuit::contracts::PlayerUse;
        using circuit::contracts::SetPlayerSlot;
        using circuit::gameplay::block_notation;
        using D = Direction;
        const auto shears = *circuit::gameplay::items::by_name("minecraft:shears");
        // Speeds by the item's tool component: cobweb 15 (harvested), leaves 15, wool 5, vines 2, others 1.
        const ItemStack tool{shears, 1};
        const auto progress = [&](std::string_view block) {
            return circuit::gameplay::destroy_progress(registry_block(block), tool, true, false, nullptr);
        };
        require(progress("oak_leaves") >= 1.0f && std::abs(progress("cobweb") - 15.0f / 4.0f / 30.0f) < 1e-6f
                    && std::abs(progress("white_wool") - 5.0f / 0.8f / 30.0f) < 1e-6f && std::abs(progress("dirt") - 1.0f / 0.5f / 30.0f) < 1e-6f,
                "dig speeds");
        const PlayerKey alex{4, 2};
        Sim s;
        s.tick({place({0, 0, 0}, registry_block("pumpkin")), place({2, 0, 0}, registry_block("beehive", {{"facing", "north"}, {"honey_level", "5"}})),
                place({4, 0, 0}, registry_block("tripwire", {{"attached", "false"}, {"disarmed", "false"}, {"east", "false"}, {"north", "false"},
                                                             {"powered", "false"}, {"south", "false"}, {"west", "false"}})),
                SetPlayerSlot{alex, 36, {shears, 1}}});
        const auto damage = [&] {
            const auto d = s.oracle.dump();
            return std::ranges::find(d.players, alex, &PlayerInventory::player)->slots[36].damage;
        };
        const auto items_of = [&](std::string_view name) {
            int n = 0;
            const auto d = s.oracle.dump();
            for (const auto& e : d.overworld().entities)
                if (e.kind == circuit::gameplay::EntityKind::item && e.stack.item == *circuit::gameplay::items::by_name(name)) n += e.stack.count;
            return n;
        };
        // Used from above by a player facing east: the carved face looks west.
        s.tick({PlayerUse{alex, 36, {0, 0, 0}, D::up, D::east, D::east, false, true, 3, 3}});
        require(block_notation(s.at({0, 0, 0})) == "minecraft:carved_pumpkin[facing=west]" && items_of("minecraft:pumpkin_seeds") == 4 && damage() == 1,
                "carved, four seeds, worn");
        s.tick({PlayerUse{alex, 36, {2, 0, 0}, D::up, D::east, D::east, false, true, 3, 3}});
        require(block_notation(s.at({2, 0, 0})) == "minecraft:beehive[facing=north,honey_level=0]" && items_of("minecraft:honeycomb") == 3 && damage() == 2,
                "a full hive emptied");
        s.tick({PlayerUse{alex, 36, {2, 0, 0}, D::up, D::east, D::east, false, true, 3, 3}});
        require(damage() == 2 && items_of("minecraft:honeycomb") == 3, "an empty hive: nothing");
        s.tick({PlayerBreak{alex, {4, 0, 0}, true, 36}});
        bool disarmed = false;
        for (const auto& r : of_kind(s.last, TraceKind::block_changed))
            if (const auto st = BlockState::decode(static_cast<std::uint32_t>(r.b))) disarmed = disarmed || block_notation(*st).find("disarmed=true") != std::string::npos;
        require(s.at({4, 0, 0}).is_air() && disarmed && damage() == 3, "tripwire disarmed first; an instant block wears shears");
    });

    suite.run("the swap key swaps the hands; books are edited only with the server's components", [] {
        using circuit::contracts::EditBook;
        using circuit::contracts::SetPlayerSlot;
        using circuit::contracts::SwapHands;
        const PlayerKey alex{4, 2};
        Sim s;
        const auto slot = [&](std::size_t i) {
            const auto d = s.oracle.dump();
            return std::ranges::find(d.players, alex, &PlayerInventory::player)->slots[i];
        };
        s.tick({SetPlayerSlot{alex, 38, {items::stone, 3}}, SetPlayerSlot{alex, 45, {items::oak_log, 1}}});
        s.tick({SwapHands{alex, 38}});
        require(slot(38) == ItemStack{items::oak_log, 1} && slot(45) == ItemStack{items::stone, 3}, "swapped");
        const auto writable = *circuit::gameplay::items::by_name("minecraft:writable_book");
        s.tick({SetPlayerSlot{alex, 36, {writable, 1}}});
        s.tick({EditBook{alex, 36, {"page"}, std::string("Title"), "alex"}});
        require(slot(36) == ItemStack{writable, 1}, "without the components' encoding the book stays");
    });

    suite.run("items burn in fire and lava, netherite does not, and lava reaches them by the version", [] {
        const auto stick = *circuit::gameplay::items::by_name("minecraft:stick");
        const auto netherite = *circuit::gameplay::items::by_name("minecraft:netherite_ingot");
        const auto lava = registry_block("lava", {{"level", "0"}});
        // Ticks until each item (in fire, in lava, netherite in fire) is gone, or 0.
        const auto gone = [&](const VersionBundle& b) {
            Sim s(b);
            auto build = floor(-4, 4, -4, 4);
            build.push_back(place({0, 0, 0}, registry_block("fire")));
            for (const BlockPos p : {BlockPos{3, 0, 1}, BlockPos{3, 0, -1}, BlockPos{2, 0, 0}, BlockPos{4, 0, 0}}) build.push_back(place(p, blocks::stone()));
            build.push_back(place({3, 0, 0}, lava));
            build.push_back(place({-3, 0, 0}, registry_block("fire")));
            s.tick(build);
            s.tick({SpawnItem{at_block(0.5, 0.0, 0.5), {}, {stick, 1}, 32767}, SpawnItem{at_block(3.5, 0.0, 0.5), {}, {stick, 1}, 32767},
                    SpawnItem{at_block(-2.5, 0.0, 0.5), {}, {netherite, 1}, 32767}});
            const auto start = s.oracle.time();
            std::array<Tick, 3> when{};
            for (Tick t = 1; t <= 40; ++t) {
                s.tick();
                for (const auto& r : of_kind(s.last, TraceKind::entity_removed)) {
                    require(r.b == static_cast<std::uint64_t>(circuit::gameplay::RemovalReason::hurt), "burnt");
                    when[r.pos.x == 0 ? 0 : r.pos.x == 3 ? 1 : 2] = s.oracle.time() - start;
                }
            }
            return when;
        };
        const auto newer = gone(bundle());
        const auto older = gone(**BundleRegistry::select_for_simulation("java-1.21.4"));
        require(newer[0] > 0 && newer[0] == older[0], "fire burns an item the same way on both versions");
        require(newer[1] > 0 && older[1] == newer[1] + 1, "lava reaches it a tick later on 1.21.4");
        require(newer[2] == 0 && older[2] == 0, "netherite survives fire");
    });

    suite.run("items and orbs inside blocks move out of them, orbs by the version", [] {
        using circuit::contracts::SummonEntity;
        using circuit::gameplay::EntityKind;
        const auto stick = *circuit::gameplay::items::by_name("minecraft:stick");
        struct Seen {
            circuit::gameplay::Entity item, orb, covered;
        };
        const auto find = [](Sim& s, double x0, double x1) {
            const auto dump = s.oracle.dump();
            for (const auto& e : dump.overworld().entities)
                if (e.position.x >= x0 && e.position.x < x1) return e;
            require(false, "an entity there");
            return circuit::gameplay::Entity{};
        };
        // An item and an orb in stones near their west faces after their
        // first tick, and an item at rest a tick after a stone was set on it.
        const auto run = [&](const VersionBundle& b) {
            Sim s(b);
            auto build = floor(-4, 12, -4, 4);
            build.push_back(place({0, 0, 0}, blocks::stone()));
            build.push_back(place({4, 0, 0}, blocks::stone()));
            s.tick(build);
            s.tick({SpawnItem{at_block(0.2, 0.3, 0.5), {}, {stick, 1}, 0}, SpawnItem{at_block(8.5, 0.0, 0.5), {}, {stick, 1}, 0},
                    SummonEntity{"minecraft:experience_orb", {4.2, 0.3, 0.5}, {}, 80, 0, 3, 1, 0}});
            Seen seen{find(s, -1.0, 2.0), find(s, 3.0, 6.0), {}};
            s.until(s.oracle.time() + 5);
            s.tick({place({8, 0, 0}, blocks::stone())});
            seen.covered = find(s, 7.0, 10.0);
            return seen;
        };
        const auto newer = run(bundle());
        const auto older = run(**BundleRegistry::select_for_simulation("java-1.21.4"));
        for (const auto* seen : {&newer, &older}) {
            const double west = 0.2 - seen->item.position.x;
            require(west >= 0.1 && west <= 0.3 && seen->item.position.y == 0.3 - 0.03 && seen->item.position.z == 0.5,
                    "the item goes west at 0.1 to 0.3 and falls by 0.04 times 0.75");
            require(seen->orb.kind == EntityKind::experience_orb && 4.2 - seen->orb.position.x >= 0.1 && 4.2 - seen->orb.position.x <= 0.3,
                    "the orb goes west too");
            require(seen->covered.position.z < 0.5 && seen->covered.position.y < 0.0 && seen->covered.position.x == 8.5,
                    "the covered item goes north and through the floor below");
        }
        require(newer.item.position == older.item.position, "items move out the same way on both versions");
        require(newer.orb.position.y == 0.3 && older.orb.position.y == 0.3 - 0.0225, "on 1.21.8 an orb in a block does not fall");
    });

    suite.run("the weather follows its timers and its levels follow the flags", [] {
        Sim s;
        // What both official servers did after /weather rain 100.
        s.tick({SetWeather{WeatherKind::rain, 100}});
        auto w = s.oracle.dump().weather;
        require(w.raining && !w.thundering && w.rain_time == 99 && w.thunder_time == 99 && w.rain_level == 0.01f, "rain for 100 ticks");
        for (int i = 0; i < 18; ++i) s.tick();
        require(!s.oracle.dump().weather.rain_visible(), "not seen after 19 ticks");
        s.tick();
        w = s.oracle.dump().weather;
        require(w.rain_visible() && w.rain_level == 0.20000001788139343f, "seen after 20: a single-precision sum just above 0.2");
        for (int i = 0; i < 79; ++i) s.tick();
        w = s.oracle.dump().weather;
        require(w.raining && w.rain_level == 0.9899993538856506f && w.thunder_level == 0.0f, "99 ticks of rising rain");
        s.tick();
        w = s.oracle.dump().weather;
        require(!w.raining && w.thundering && w.rain_level < 0.99f && w.thunder_level == 0.01f, "the timers end: rain stops, thunder starts");
        s.tick();
        w = s.oracle.dump().weather;
        require(w.rain_time >= circuit::gameplay::clear_min && w.rain_time <= circuit::gameplay::clear_max && w.thunder_time >= circuit::gameplay::thunder_min
                    && w.thunder_time <= circuit::gameplay::thunder_max,
                "then the flags' next durations are drawn");
        // A clear spell ends in a thunderstorm; the levels fall while it lasts.
        s.tick({SetWeather{WeatherKind::clear, 2}});
        s.tick();
        w = s.oracle.dump().weather;
        require(!w.raining && !w.thundering && w.clear_time == 0 && w.rain_time == 1 && w.thunder_time == 1, "clear for two ticks");
        s.tick();
        w = s.oracle.dump().weather;
        require(w.raining && w.thundering, "then a thunderstorm");
        // Default durations, and timers that wait while doWeatherCycle is off.
        s.tick({SetWeather{WeatherKind::thunder, 0}});
        w = s.oracle.dump().weather;
        require(w.rain_time == w.thunder_time && w.rain_time + 1 >= circuit::gameplay::thunder_min && w.rain_time + 1 <= circuit::gameplay::thunder_max,
                "a default thunderstorm");
        s.tick({SetGameRule{"doWeatherCycle", 0}, SetWeather{WeatherKind::clear, 5}});
        for (int i = 0; i < 10; ++i) s.tick();
        w = s.oracle.dump().weather;
        require(w.clear_time == 5 && !w.raining && w.rain_level < 0.9f, "timers wait; levels still follow the flags");
        // A start sets the levels to their flags.
        s.tick({SetWeather{WeatherKind::rain, 50}});
        s.tick({LevelsStarted{}});
        w = s.oracle.dump().weather;
        require(w.rain_level == 1.0f && w.thunder_level == 0.0f, "levels after a start");
        s.tick({SetWeather{static_cast<WeatherKind>(3), 1}, SetWeather{WeatherKind::rain, -1}});
        require(of_kind(s.last, TraceKind::command_rejected).size() == 2, "invalid weather is refused");
    });

    suite.run("crops grow only in light 9 or more", [] {
        // 16x16 wheat on farmland sealed in stone (floor, walls and roof);
        // with a glowstone in the middle, light 15 - distance reaches the
        // crops.
        for (const bool lit : {false, true}) {
            Sim s(bundle(), 5, 1);
            s.tick({SetGameRule{"randomTickSpeed", 200}});
            std::vector<CommandBody> box, crops;
            for (int x = -1; x <= 16; ++x)
                for (int z = -1; z <= 16; ++z) {
                    const bool wall = x == -1 || x == 16 || z == -1 || z == 16;
                    box.push_back(place({x, -1, z}, wall ? blocks::stone() : registry_block("farmland")));
                    box.push_back(place({x, 1, z}, blocks::stone()));
                    if (wall) box.push_back(place({x, 0, z}, blocks::stone()));
                    else if (lit && x == 8 && z == 8) box.push_back(place({x, 0, z}, registry_block("glowstone")));
                    else crops.push_back(place({x, 0, z}, blocks::wheat()));
                }
            // Light follows the previous tick's blocks: the crops come a tick
            // after the box closes.
            s.tick(box);
            s.tick(crops);
            s.until(80);
            unsigned grown = 0, far = 0;
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z) {
                    const auto w = s.at({x, 0, z});
                    if (!w.is(BlockType::wheat) || w.level() == 0) continue;
                    ++grown;
                    if (std::abs(x - 8) + std::abs(z - 8) > 6) ++far;
                }
            // Dry farmland and neighbors on all sides: one in 13 a random tick.
            require(lit ? grown > 10 : grown == 0, std::format("{}: {} crops grew", lit ? "lit" : "dark", grown));
            require(far == 0, std::format("{} crops grew with light below 9", far));
        }
    });

    suite.run("stems set their fruit, and grass spreads and dies, in random ticks", [] {
        Sim s(bundle(), 9, 1);
        const auto name = [&](BlockPos p) { return circuit::gameplay::block_notation(s.at(p)); };
        std::vector<CommandBody> build;
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) build.push_back(place({x, -1, z}, blocks::stone()));
        // A ripe stem on moist farmland, free only to the east; water keeps
        // the farmland moist. A torchflower crop about to flower.
        for (int x = 3; x <= 5; ++x)
            for (int z = 3; z <= 5; ++z) build.push_back(place({x, -1, z}, registry_block("farmland", {{"moisture", "7"}})));
        build.push_back(place({7, -1, 4}, blocks::water_source()));
        build.push_back(place({8, -1, 8}, registry_block("farmland", {{"moisture", "7"}})));
        for (const BlockPos p : {BlockPos{3, 0, 4}, BlockPos{4, 0, 3}, BlockPos{4, 0, 5}}) build.push_back(place(p, blocks::stone()));
        // A grass block among dirt, and one under stone.
        for (int x = 9; x <= 13; ++x)
            for (int z = 9; z <= 13; ++z) build.push_back(place({x, -1, z}, registry_block("dirt")));
        build.push_back(place({11, -1, 11}, registry_block("grass_block")));
        build.push_back(place({2, -1, 12}, registry_block("grass_block")));
        build.push_back(place({2, 0, 12}, blocks::stone()));
        s.tick(build);
        s.tick({place({4, 0, 4}, registry_block("melon_stem", {{"age", "7"}})), place({8, 0, 8}, registry_block("torchflower_crop", {{"age", "1"}})),
                SetGameRule{"randomTickSpeed", 1000}});
        s.until(s.oracle.time() + 200);
        require(name({5, 0, 4}) == "minecraft:melon" && name({4, 0, 4}) == "minecraft:attached_melon_stem[facing=east]",
                std::format("the fruit: {}, {}", name({5, 0, 4}), name({4, 0, 4})));
        require(name({8, 0, 8}) == "minecraft:torchflower", "the crop flowers: " + name({8, 0, 8}));
        require(name({2, -1, 12}) == "minecraft:dirt", "grass under stone dies: " + name({2, -1, 12}));
        int grass = 0;
        for (int x = 9; x <= 13; ++x)
            for (int z = 9; z <= 13; ++z) grass += name({x, -1, z}) == "minecraft:grass_block[snowy=false]" ? 1 : 0;
        require(grass > 1, std::format("grass spreads: {} blocks", grass));
        // The fruit taken, the stem stands ripe again.
        s.tick({SetGameRule{"randomTickSpeed", 0}});
        s.tick({place({5, 0, 4}, blocks::air())});
        require(name({4, 0, 4}) == "minecraft:melon_stem[age=7]", "the stem without its fruit: " + name({4, 0, 4}));
    });

    suite.run("sugar cane, cacti, nether wart and kelp grow in random ticks", [] {
        Sim s(bundle(), 11, 1);
        const auto name = [&](BlockPos p) { return circuit::gameplay::block_notation(s.at(p)); };
        std::vector<CommandBody> build;
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) build.push_back(place({x, -1, z}, blocks::stone()));
        // Sugar cane on sand beside water, a cactus on sand, nether wart on
        // soul sand, and kelp in a glass tank of water four deep.
        build.push_back(place({2, 0, 2}, registry_block("sand")));
        build.push_back(place({3, 0, 2}, blocks::water_source()));
        build.push_back(place({6, 0, 2}, registry_block("sand")));
        build.push_back(place({9, 0, 2}, registry_block("soul_sand")));
        for (int x = 11; x <= 15; ++x)
            for (int z = 9; z <= 13; ++z)
                for (int y = 0; y <= 4; ++y) {
                    const bool wall = x == 11 || x == 15 || z == 9 || z == 13;
                    build.push_back(place({x, y, z}, wall ? registry_block("glass") : blocks::water_source()));
                }
        s.tick(build);
        s.tick({place({2, 1, 2}, registry_block("sugar_cane")), place({6, 1, 2}, registry_block("cactus")),
                place({9, 1, 2}, registry_block("nether_wart")), place({13, 0, 11}, registry_block("kelp", {{"age", "20"}})),
                SetGameRule{"randomTickSpeed", 2000}});
        s.until(s.oracle.time() + 300);
        require(name({2, 3, 2}).starts_with("minecraft:sugar_cane") && name({2, 4, 2}) == "minecraft:air",
                std::format("sugar cane three tall: {}, {}", name({2, 3, 2}), name({2, 4, 2})));
        require(name({6, 2, 2}).starts_with("minecraft:cactus"), "a cactus grows: " + name({6, 2, 2}));
        require(name({9, 1, 2}) == "minecraft:nether_wart[age=3]", "nether wart ripens: " + name({9, 1, 2}));
        require(name({13, 0, 11}) == "minecraft:kelp_plant" && name({13, 4, 11}).starts_with("minecraft:kelp"),
                std::format("kelp grows to the surface: {}, {}", name({13, 0, 11}), name({13, 4, 11})));
    });

    suite.run("leaves take their distance from logs and decay without them", [] {
        Sim s(bundle(), 13, 1);
        const auto name = [&](BlockPos p) { return circuit::gameplay::block_notation(s.at(p)); };
        const auto leaves = [](int d) {
            return std::format("minecraft:oak_leaves[distance={},persistent=false,waterlogged=false]", d);
        };
        std::vector<CommandBody> line;
        for (int x = 1; x <= 8; ++x) line.push_back(place({x, 0, 0}, registry_block("oak_leaves")));
        s.tick(line);
        s.tick({place({0, 0, 0}, registry_block("oak_log", {{"axis", "y"}}))});
        s.until(s.oracle.time() + 10);
        for (int x = 1; x <= 8; ++x)
            require(name({x, 0, 0}) == leaves(std::min(x, 7)), std::format("distance at {}: {}", x, name({x, 0, 0})));
        // Without the log they count up again a step a tick, then decay.
        s.tick({place({0, 0, 0}, blocks::air())});
        s.until(s.oracle.time() + 10);
        require(name({1, 0, 0}) == leaves(7), "away from logs: " + name({1, 0, 0}));
        s.tick({SetGameRule{"randomTickSpeed", 4096}});
        s.until(s.oracle.time() + 20);
        for (int x = 1; x <= 8; ++x) require(s.at({x, 0, 0}).is_air(), std::format("decayed at {}: {}", x, name({x, 0, 0})));
    });

    suite.run("bamboo grows from its shoot and thickens", [] {
        Sim s(bundle(), 17, 1);
        const auto name = [&](BlockPos p) { return circuit::gameplay::block_notation(s.at(p)); };
        s.tick({place({4, -1, 4}, registry_block("grass_block"))});
        s.tick({place({4, 0, 4}, registry_block("bamboo_sapling")), SetGameRule{"randomTickSpeed", 4096}});
        s.until(s.oracle.time() + 60);
        int height = 0;
        while (name({4, height, 4}).starts_with("minecraft:bamboo[")) ++height;
        require(height >= 5, std::format("bamboo {} tall: {}", height, name({4, 0, 4})));
        require(name({4, 0, 4}).starts_with("minecraft:bamboo[age=1"), "a thick stalk: " + name({4, 0, 4}));
        require(name({4, height - 1, 4}).find("leaves=large") != std::string::npos, "large leaves on top: " + name({4, height - 1, 4}));
    });

    suite.run("ice melts by light, copper weathers, ore goes out, amethyst buds and lava sets fire", [] {
        Sim s(bundle(), 19, 1);
        const auto name = [&](BlockPos p) { return circuit::gameplay::block_notation(s.at(p)); };
        std::vector<CommandBody> build;
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) build.push_back(place({x, -1, z}, blocks::stone()));
        // Ice beside glowstone and in a box of stone; copper alone; lit ore;
        // budding amethyst; lava in stone beside planks above it.
        build.push_back(place({1, 0, 1}, registry_block("glowstone")));
        build.push_back(place({2, 0, 1}, registry_block("ice")));
        build.push_back(place({6, 0, 6}, registry_block("copper_block")));
        build.push_back(place({9, 0, 1}, registry_block("redstone_ore", {{"lit", "true"}})));
        build.push_back(place({12, 2, 12}, registry_block("budding_amethyst")));
        build.push_back(place({4, 0, 12}, circuit::gameplay::lava_source()));
        for (const BlockPos p : {BlockPos{3, 0, 12}, BlockPos{5, 0, 12}, BlockPos{4, 0, 11}, BlockPos{4, 0, 13}}) build.push_back(place(p, blocks::stone()));
        build.push_back(place({5, 1, 12}, registry_block("oak_planks")));
        build.push_back(SetGameRule{"allowFireTicksAwayFromPlayer", 1});
        s.tick(build);
        s.tick({SetGameRule{"randomTickSpeed", 4096}});
        s.until(s.oracle.time() + 40);
        require(name({2, 0, 1}).starts_with("minecraft:water"), "ice by glowstone melts: " + name({2, 0, 1}));
        require(!name({6, 0, 6}).starts_with("minecraft:copper_block"), "copper weathers: " + name({6, 0, 6}));
        require(name({9, 0, 1}) == "minecraft:redstone_ore[lit=false]", "the ore goes out: " + name({9, 0, 1}));
        int buds = 0;
        for (const auto d : circuit::contracts::all_directions) buds += name(BlockPos{12, 2, 12}.relative(d)).find("amethyst") != std::string::npos ? 1 : 0;
        require(buds > 0, "budding amethyst grows buds");
        bool fire = false;
        for (int x = 3; x <= 7; ++x)
            for (int z = 10; z <= 14; ++z)
                for (int y = 1; y <= 3; ++y) fire = fire || name({x, y, z}).starts_with("minecraft:fire") || s.at({5, 1, 12}).is_air();
        require(fire, "lava sets fire by the planks");
    });

    suite.run("eyeblossoms turn by night, copper doors weather whole, pitcher crops grow tall and nylium dies", [] {
        Sim s(bundle(), 23, 1);
        const auto name = [&](BlockPos p) { return circuit::gameplay::block_notation(s.at(p)); };
        std::vector<CommandBody> build;
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) build.push_back(place({x, -1, z}, registry_block("grass_block")));
        for (int x = 2; x <= 6; ++x) build.push_back(place({x, 0, 2}, registry_block("closed_eyeblossom")));
        build.push_back(place({9, -1, 9}, registry_block("crimson_nylium")));
        build.push_back(place({9, 0, 9}, blocks::stone()));
        for (int x = 12; x <= 14; ++x) build.push_back(place({x, -1, 12}, registry_block("farmland", {{"moisture", "7"}})));
        build.push_back(place({11, -1, 12}, blocks::water_source()));
        s.tick(build);
        s.tick({place({12, 0, 12}, registry_block("pitcher_crop", {{"age", "2"}, {"half", "lower"}})),
                place({4, 0, 8}, registry_block("copper_door", {{"half", "lower"}})),
                place({4, 1, 8}, registry_block("copper_door", {{"half", "upper"}})),
                SetDayTime{13000}, SetGameRule{"randomTickSpeed", 4096}});
        s.until(s.oracle.time() + 100);
        for (int x = 2; x <= 6; ++x) require(name({x, 0, 2}) == "minecraft:open_eyeblossom", std::format("open by night at {}: {}", x, name({x, 0, 2})));
        require(name({9, -1, 9}) == "minecraft:netherrack", "nylium under stone: " + name({9, -1, 9}));
        require(name({12, 1, 12}).starts_with("minecraft:pitcher_crop[age=") && name({12, 1, 12}).ends_with("half=upper]"),
                "the pitcher's upper half: " + name({12, 1, 12}));
        const auto lower = name({4, 0, 8}), upper = name({4, 1, 8});
        require(!lower.starts_with("minecraft:copper_door") && lower.substr(0, lower.find('[')) == upper.substr(0, upper.find('[')),
                std::format("both halves weather: {}, {}", lower, upper));
    });

    suite.run("pointed dripstone takes its thickness, falls without support and drips into cauldrons", [] {
        Sim s(bundle(), 29, 1);
        const auto name = [&](BlockPos p) { return circuit::gameplay::block_notation(s.at(p)); };
        const auto piece = [](std::string_view d) {
            return registry_block("pointed_dripstone", {{"thickness", "tip"}, {"vertical_direction", std::string(d)}, {"waterlogged", "false"}});
        };
        const auto thick = [](std::string_view t, std::string_view d) {
            return std::format("minecraft:pointed_dripstone[thickness={},vertical_direction={},waterlogged=false]", t, d);
        };
        std::vector<CommandBody> build;
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) build.push_back(place({x, -1, z}, blocks::stone()));
        build.push_back(place({2, 8, 2}, registry_block("dripstone_block")));
        build.push_back(place({2, 9, 2}, blocks::water_source()));
        for (const auto& [dx, dz] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
            build.push_back(place({2 + dx, 9, 2 + dz}, registry_block("glass")));
        build.push_back(place({2, 0, 2}, registry_block("cauldron")));
        build.push_back(place({6, 8, 6}, registry_block("dripstone_block")));
        s.tick(build);
        for (int y = 7; y >= 5; --y) s.tick({place({2, y, 2}, piece("down")), place({6, y, 6}, piece("down"))});
        require(name({2, 7, 2}) == thick("base", "down") && name({2, 6, 2}) == thick("frustum", "down") && name({2, 5, 2}) == thick("tip", "down"),
                std::format("a stalactite of three: {}, {}, {}", name({2, 7, 2}), name({2, 6, 2}), name({2, 5, 2})));
        // Without the block it hangs from it falls, its pieces breaking on the floor.
        s.tick({place({6, 8, 6}, blocks::air())});
        s.until(s.oracle.time() + 30);
        require(s.at({6, 7, 6}).is_air() && s.at({6, 5, 6}).is_air() && s.at({6, 0, 6}).is_air(), "the stalactite fell and broke");
        // Water drips into the cauldron below the other.
        s.tick({SetGameRule{"randomTickSpeed", 4096}});
        s.until(s.oracle.time() + 200);
        require(name({2, 0, 2}).starts_with("minecraft:water_cauldron"), "water drips into the cauldron: " + name({2, 0, 2}));
    });

    suite.run("dispensers use bone meal on plants and keep it for other blocks", [] {
        Sim s;
        const auto name = [&](circuit::contracts::BlockPos p) { return circuit::gameplay::block_notation(s.at(p)); };
        const auto bone_meal = *circuit::gameplay::items::by_name("minecraft:bone_meal");
        s.tick(floor(-3, 8, -1, 6));
        s.tick({place({1, -1, 0}, registry_block("farmland", {{"moisture", "7"}})), place({0, 0, 0}, blocks::dispenser(D::east)),
                place({0, 0, 3}, blocks::dispenser(D::east)), place({1, 0, 3}, blocks::stone()),
                insert({0, 0, 0}, 0, bone_meal, 1), insert({0, 0, 3}, 0, bone_meal, 1)});
        s.tick({place({1, 0, 0}, blocks::wheat())});
        s.tick({place({-1, 0, 0}, blocks::redstone_block()), place({-1, 0, 3}, blocks::redstone_block())});
        s.until(s.oracle.time() + 6);
        require(s.at({1, 0, 0}).level() >= 2, "the wheat grows 2 to 5 stages: " + name({1, 0, 0}));
        const auto d = s.oracle.dump();
        const auto kept = [&](circuit::contracts::BlockPos p) {
            for (const auto& [pos, be] : d.overworld().block_entities)
                if (pos == p) return be.slots.empty() ? 0 : static_cast<int>(be.slots[0].count);
            return -1;
        };
        require(kept({0, 0, 0}) == 0 && kept({0, 0, 3}) == 1, std::format("used once, kept once: {}, {}", kept({0, 0, 0}), kept({0, 0, 3})));
    });

    suite.run("furnaces cook with fuel from hoppers and smokers burn it twice as fast", [] {
        using circuit::gameplay::blocks::FurnaceKind;
        namespace recipes = circuit::gameplay::recipes;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->cooking.push_back({recipes::Cooking::Kind::smelting, "minecraft:iron_ingot", {{item("minecraft:raw_iron")}},
                                 {item("minecraft:iron_ingot"), 1}, 0.7f, 200});
        data->cooking.push_back({recipes::Cooking::Kind::smoking, "minecraft:cooked_beef_from_smoking", {{item("minecraft:beef")}},
                                 {item("minecraft:cooked_beef"), 1}, 0.35f, 100});
        Sim s(bundle(), 7, 2, data);
        s.tick(floor(-3, 8, -1, 6));
        s.tick({place({0, 0, 0}, blocks::furnace(FurnaceKind::furnace, Direction::south)), place({0, 1, 0}, blocks::hopper(Direction::down)),
                place({-1, 0, 0}, blocks::hopper(Direction::east)), place({4, 0, 0}, blocks::furnace(FurnaceKind::smoker, Direction::south)),
                insert({0, 1, 0}, 0, item("minecraft:raw_iron"), 2), insert({-1, 0, 0}, 0, item("minecraft:stick"), 1),
                insert({4, 0, 0}, 0, item("minecraft:beef"), 2), insert({4, 0, 0}, 1, item("minecraft:stick"), 2)});
        const auto slots = [&](circuit::contracts::BlockPos p) {
            const auto d = s.oracle.dump();
            for (const auto& [pos, be] : d.overworld().block_entities)
                if (pos == p) return be.slots;
            return std::vector<ItemStack>{};
        };
        s.until(s.oracle.time() + 3);
        require(s.at({0, 0, 0}).flag() && s.at({4, 0, 0}).flag(), "both light");
        // A stick burns 100 ticks in a furnace, 50 in a smoker: half an iron
        // ingot's time; the smoker cooks one beef per stick.
        s.until(s.oracle.time() + 210);
        require(slots({0, 0, 0})[2].empty() && !s.at({0, 0, 0}).flag(), "the furnace went out before the ingot");
        require(slots({4, 0, 0})[2].count == 1, std::format("the smoker cooked one beef: {}", slots({4, 0, 0})[2].count));
    });

    suite.run("furnace menus take fuel in the fuel slot and shift-click by recipe and fuel", [] {
        using circuit::contracts::ContainerClick;
        using circuit::contracts::PlayerKey;
        using circuit::gameplay::blocks::FurnaceKind;
        namespace recipes = circuit::gameplay::recipes;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->cooking.push_back({recipes::Cooking::Kind::smelting, "minecraft:iron_ingot", {{item("minecraft:raw_iron")}},
                                 {item("minecraft:iron_ingot"), 1}, 0.7f, 200});
        Sim s(bundle(), 7, 2, data);
        const PlayerKey alex{7, 8};
        const BlockPos furnace{0, 0, 0};
        s.tick({place(furnace, blocks::furnace(FurnaceKind::furnace, Direction::south)),
                circuit::contracts::SetPlayerSlot{alex, 36, {item("minecraft:raw_iron"), 3, 0}},
                circuit::contracts::SetPlayerSlot{alex, 37, {item("minecraft:coal"), 2, 0}},
                circuit::contracts::SetPlayerSlot{alex, 38, {items::stone, 4, 0}},
                circuit::contracts::SetPlayerSlot{alex, 39, {items::bucket, 1, 0}}});
        // Furnace menu: 0 input, 1 fuel, 2 result, 3-29 the main inventory, 30-38 the hotbar.
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::optional(furnace), slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, alex, &PlayerInventory::player);
        };
        const auto furnace_slot = [&](std::size_t i) { return block_entity_at(s.oracle.dump(), furnace)->slots[i]; };
        s.tick({click(32, 0, 0), click(1, 0, 0), click(2, 0, 0)});
        require(player().cursor.count == 4 && furnace_slot(1).empty() && furnace_slot(2).empty(), "stone fits neither fuel nor result");
        s.tick({click(32, 0, 0), click(32, 0, 1)});
        require(player().slots[38].empty() && player().slots[9] == ItemStack{items::stone, 4, 0}, "no recipe, no fuel: into the inventory");
        s.tick({click(33, 0, 1)});
        require(furnace_slot(1).empty() && player().slots[10].item == items::bucket, "a bucket is no fuel to shift-click");
        s.tick({click(4, 0, 0), click(1, 0, 0), click(31, 0, 1), click(30, 0, 1)});
        require(furnace_slot(1).item == items::bucket && player().slots[37].count == 2, "but goes in the fuel slot; coal waits");
        require(furnace_slot(0) == ItemStack{item("minecraft:raw_iron"), 3, 0}, "raw iron goes to the input");
        s.tick({click(1, 0, 1), click(31, 0, 1)});
        require(furnace_slot(1) == ItemStack{item("minecraft:coal"), 1, 0} && s.at(furnace).flag() && player().slots[10].item == items::bucket,
                "the bucket back to the first empty inventory slot, then coal lights it");
        s.until(s.oracle.time() + 205);
        require(furnace_slot(2).item == item("minecraft:iron_ingot"), "an ingot");
        s.tick({click(2, 0, 1)});
        require(furnace_slot(2).empty() && player().slots[44].item == item("minecraft:iron_ingot"),
                "the result goes to the hotbar from its end");
    });

    suite.run("experience orbs fall, rest, merge 40 ids apart and go at age 6000", [] {
        using circuit::contracts::SummonEntity;
        using circuit::gameplay::EntityKind;
        Sim s;
        s.tick(floor(-3, 45, -1, 6));
        const auto orb = [](double x, double y, double z, std::int32_t value, std::int32_t age = 0) {
            return SummonEntity{"minecraft:experience_orb", {x, y, z}, {}, 80, 0, value, 1, age};
        };
        const auto orbs = [&] {
            const auto d = s.oracle.dump();
            std::vector<circuit::gameplay::Entity> out;
            for (const auto& e : d.overworld().entities)
                if (e.kind == EntityKind::experience_orb) out.push_back(e);
            return out;
        };
        // The first orb, then 38 others and an old one, then one of its value
        // beside it 40 ids on, and one 41 ids on.
        std::vector<CommandBody> first{orb(0.5, 3.0, 0.5, 3)};
        for (int k = 0; k < 38; ++k) first.push_back(orb(2.5 + k, 0.0, 4.5, 1));
        first.push_back(orb(0.5, 0.0, 2.5, 7, 5997));
        s.tick(first);
        const auto ids = orbs();
        require(ids.size() == 40, "forty orbs");
        const auto first_id = ids.front().id.value;
        s.until(s.oracle.time() + 30);
        const auto rested = orbs();
        const auto it = std::ranges::find(rested, first_id, [](const auto& e) { return e.id.value; });
        require(it != rested.end() && it->position.y == 0.0 && it->velocity.y == 0.0 && it->on_ground, "the first one fell and rests");
        require(rested.size() == 39, "the old one went at age 6000");
        s.tick({orb(0.8, 0.0, 0.5, 3), orb(0.5, 0.0, 1.0, 3)});
        const auto now = orbs();
        const auto merged = std::ranges::find(now, first_id + 40, [](const auto& e) { return e.id.value; });
        require(merged != now.end() && merged->orbs == 2 && now.size() == 40
                    && std::ranges::find(now, first_id, [](const auto& e) { return e.id.value; }) == now.end(),
                "the orb 40 ids on took in the first; the next one did not");
    });

    suite.run("players draw experience orbs and take one every 2 ticks for the official levels", [] {
        using circuit::contracts::PickUp;
        using circuit::contracts::PlayerKey;
        using circuit::contracts::PlayerPresence;
        using circuit::contracts::SummonEntity;
        using circuit::gameplay::EntityKind;
        // Points to level and progress as both official servers give them.
        circuit::gameplay::PlayerInventory p;
        std::vector<std::tuple<float, std::int32_t, std::int32_t>> got;
        for (const auto points : {7, 17, 1, 2, 3, 4, 5, 37, 37, 37, 2477}) {
            circuit::gameplay::experience::give_points(p, points);
            got.emplace_back(p.xp_progress, p.xp_level, p.xp_total);
        }
        require(got.front() == std::tuple{0.0f, 1, 7} && got[1] == std::tuple{0.7272726893424988f, 2, 24}
                    && got[3] == std::tuple{0.9999999403953552f, 2, 27} && got[9] == std::tuple{0.6000000834465027f, 9, 150}
                    && got.back() == std::tuple{0.4565214216709137f, 38, 2627},
                "the official levels and progress");
        Sim s;
        const PlayerKey alex{3, 9};
        s.tick(floor(-3, 20, -3, 6));
        s.tick({PlayerPresence{alex, {0.5, 0.0, 0.5}}, SummonEntity{"minecraft:experience_orb", {4.5, 0.0, 0.5}, {}, 80, 0, 7, 1, 0},
                SummonEntity{"minecraft:experience_orb", {12.5, 0.0, 0.5}, {}, 80, 0, 3, 1, 0},
                SummonEntity{"minecraft:experience_orb", {0.5, 0.0, 2.5}, {}, 80, 0, 5, 2, 0}});
        const auto orbs = [&] {
            const auto d = s.oracle.dump();
            std::vector<circuit::gameplay::Entity> out;
            for (const auto& e : d.overworld().entities)
                if (e.kind == EntityKind::experience_orb) out.push_back(e);
            return out;
        };
        const auto first = orbs();
        s.until(s.oracle.time() + 10);
        const auto later = orbs();
        require(later[0].position.x < first[0].position.x - 0.1 && later[1].position.x == first[1].position.x,
                "the orb within 8 blocks comes, the one 12 away stays");
        const auto pair = first[2].id;
        const auto player = [&] {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, alex, &circuit::gameplay::PlayerInventory::player);
        };
        s.tick({PickUp{alex, pair, 36, false}});
        require(player().xp_total == 5 && orbs().size() == 3, "one of the pair's two orbs");
        s.tick({PickUp{alex, pair, 36, false}});
        require(player().xp_total == 5, "not again in the next tick");
        s.tick({PickUp{alex, pair, 36, false}});
        require(player().xp_total == 10 && player().xp_level == 1 && orbs().size() == 2, "the second two ticks on, and the orb is gone");
    });

    suite.run("mined ores give experience as orbs at their center, but not to Silk Touch", [] {
        using circuit::contracts::ComponentPatch;
        using circuit::contracts::PlayerBreak;
        using circuit::contracts::PlayerKey;
        using circuit::contracts::SetPlayerSlot;
        using circuit::gameplay::EntityKind;
        auto data = std::make_shared<circuit::gameplay::GameData>();
        // A stand-in Silk Touch (id 5): a stack with any component has it.
        data->block_experience_set = {{5, 0.0f}};
        data->enchantment_level = [](const ItemStack& stack, std::uint32_t enchantment) -> long {
            return enchantment == 5 && !stack.components.empty() ? 1 : 0;
        };
        const PlayerKey alex{2, 2};
        Sim s(bundle(), 7, 2, data);
        s.tick(floor(-3, 12, -1, 6));
        const auto silk = ComponentPatch::make(772, {{1, {std::byte{1}}}}, {}, {});
        s.tick({place({2, 0, 2}, registry_block("diamond_ore")), place({6, 0, 2}, registry_block("diamond_ore")),
                place({10, 0, 2}, registry_block("iron_ore")), SetPlayerSlot{alex, 36, {items::diamond_pickaxe, 1, 0}},
                SetPlayerSlot{alex, 37, {items::diamond_pickaxe, 1, 0, silk}}});
        s.tick({PlayerBreak{alex, {2, 0, 2}, true, 36}, PlayerBreak{alex, {6, 0, 2}, true, 37}, PlayerBreak{alex, {10, 0, 2}, true, 36}});
        const auto d = s.oracle.dump();
        std::int32_t total = 0;
        bool centered = true;
        for (const auto& e : d.overworld().entities)
            if (e.kind == EntityKind::experience_orb) {
                total += e.experience * e.orbs;
                centered = centered && e.position.x > 2.0 && e.position.x < 3.0 && e.seeded;
            }
        require(total >= 3 && total <= 7 && centered, std::format("3 to 7 experience from the plain pickaxe's diamond ore: {}", total));
    });

    suite.run("furnaces keep what they cooked and give its experience when taken or removed", [] {
        using circuit::contracts::ContainerClick;
        using circuit::contracts::PlayerBreak;
        using circuit::contracts::PlayerKey;
        using circuit::contracts::PlayerPresence;
        using circuit::contracts::SetPlayerSlot;
        using circuit::gameplay::EntityKind;
        using circuit::gameplay::blocks::FurnaceKind;
        namespace recipes = circuit::gameplay::recipes;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->cooking.push_back({recipes::Cooking::Kind::blasting, "minecraft:gold_ingot_from_blasting_raw_gold", {{item("minecraft:raw_gold")}},
                                 {item("minecraft:gold_ingot"), 1}, 1.0f, 100});
        Sim s(bundle(), 7, 2, data);
        const PlayerKey alex{5, 5};
        s.tick(floor(-3, 12, -1, 6));
        s.tick({place({0, 0, 0}, blocks::furnace(FurnaceKind::blast_furnace, Direction::south)), place({6, 0, 0}, blocks::furnace(FurnaceKind::blast_furnace, Direction::south)),
                insert({0, 0, 0}, 0, item("minecraft:raw_gold"), 3), insert({0, 0, 0}, 1, items::coal, 1),
                insert({6, 0, 0}, 0, item("minecraft:raw_gold"), 2), insert({6, 0, 0}, 1, items::coal, 1),
                PlayerPresence{alex, {3.5, 0.0, 3.5}}, SetPlayerSlot{alex, 36, {items::diamond_pickaxe, 1, 0}}});
        s.until(s.oracle.time() + 310);
        const auto orbs = [&] {
            const auto d = s.oracle.dump();
            std::vector<circuit::gameplay::Entity> out;
            for (const auto& e : d.overworld().entities)
                if (e.kind == EntityKind::experience_orb) out.push_back(e);
            return out;
        };
        const auto cooked = s.oracle.dump();
        const auto* first = block_entity_at(cooked, {0, 0, 0});
        require(first && first->recipes_used == std::vector<std::pair<std::string, std::int32_t>>{{"minecraft:gold_ingot_from_blasting_raw_gold", 3}},
                "three cooked by the recipe");
        // Taking the result gives 3 at the player's feet; the other furnace,
        // broken, gives 2 at its center after its contents.
        s.tick({ContainerClick{alex, std::optional(BlockPos{0, 0, 0}), 2, 0, 1, false, at_block(3.5, 1.6, 3.5), {}}});
        auto now = orbs();
        std::int32_t taken = 0;
        for (const auto& o : now) taken += std::abs(o.position.x - 3.5) < 0.5 ? o.experience * o.orbs : 0;
        const auto after = s.oracle.dump();
        require(taken == 3 && block_entity_at(after, {0, 0, 0})->recipes_used.empty(), "3 at the player, the recipes cleared");
        s.tick({PlayerBreak{alex, {6, 0, 0}, true, 36}});
        now = orbs();
        std::int32_t broken = 0;
        for (const auto& o : now) broken += std::abs(o.position.x - 6.5) < 0.5 ? o.experience * o.orbs : 0;
        require(broken == 2, std::format("2 from the broken furnace: {}", broken));
    });

    suite.run("brewing stands brew the bottles that mix with their ingredient", [] {
        using circuit::contracts::ComponentPatch;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->brewing_fuel = {items::blaze_powder};
        // A stand-in for the server's encoding: the potion's name as the bytes of one component.
        data->potion_of = [](const ComponentPatch& patch) -> std::optional<std::string_view> {
            for (const auto& a : patch.added()) {
                const std::string name(reinterpret_cast<const char*>(a.value.data()), a.value.size());
                const auto names = circuit::gameplay::potions::names();
                if (const auto it = std::ranges::find(names, name); it != names.end()) return *it;
            }
            return std::nullopt;
        };
        data->with_potion = [](const ComponentPatch&, std::string_view potion) {
            std::vector<std::byte> bytes;
            for (const char c : potion) bytes.push_back(static_cast<std::byte>(c));
            return std::optional(ComponentPatch::make(772, {{1, bytes}}, {}, {}));
        };
        const auto bottle = [&](std::uint16_t kind, std::string_view potion) {
            return ItemStack{kind, 1, 0, *data->with_potion({}, potion)};
        };
        const auto put = [](BlockPos p, std::uint8_t slot, ItemStack stack) { return circuit::contracts::InsertItem{p, slot, std::move(stack)}; };
        Sim s(bundle(), 7, 2, data);
        s.tick(floor(-3, 10, -1, 6));
        const auto slots = [&](BlockPos p) { return block_entity_at(s.oracle.dump(), p)->slots; };
        const auto potion = [&](const ItemStack& stack) { return data->potion_of(stack.components).value_or(""); };
        // Water bottles with nether wart; splash potions with two dragon's
        // breath; a hopper below another stand takes its bottle, not its fuel.
        s.tick({place({0, 0, 0}, blocks::brewing_stand()), place({4, 0, 0}, blocks::brewing_stand()), place({8, 1, 0}, blocks::brewing_stand()),
                place({8, 0, 0}, blocks::hopper(Direction::east))});
        s.tick({put({0, 0, 0}, 0, bottle(items::potion, "minecraft:water")), put({0, 0, 0}, 2, bottle(items::potion, "minecraft:water")),
                put({0, 0, 0}, 3, {item("minecraft:nether_wart"), 2}), put({0, 0, 0}, 4, {items::blaze_powder, 1}),
                put({4, 0, 0}, 1, bottle(items::splash_potion, "minecraft:swiftness")), put({4, 0, 0}, 3, {items::dragon_breath, 2}),
                put({4, 0, 0}, 4, {items::blaze_powder, 2}), put({8, 1, 0}, 0, bottle(items::potion, "minecraft:awkward")),
                put({8, 1, 0}, 4, {items::blaze_powder, 3})});
        require(slots({0, 0, 0})[4].empty() && slots({4, 0, 0})[4].count == 1 && s.at({0, 0, 0}).level() == 0b101,
                "fuel is used up as it comes; the stand shows its bottles");
        require(slots({8, 1, 0})[0].empty() && slots({8, 1, 0})[4].count == 2 && slots({8, 0, 0})[0].item == items::potion,
                "the hopper below takes the bottle, not the fuel");
        const auto entities = s.oracle.dump().overworld().entities.size();
        s.until(s.oracle.time() + 399);
        require(potion(slots({0, 0, 0})[0]).empty() || potion(slots({0, 0, 0})[0]) == "minecraft:water", "not before 400 ticks");
        s.until(s.oracle.time() + 1);
        const auto first = slots({0, 0, 0});
        require(potion(first[0]) == "minecraft:awkward" && potion(first[2]) == "minecraft:awkward" && first[3].count == 1,
                "both bottles became awkward potions with one nether wart");
        const auto second = slots({4, 0, 0});
        require(second[1].item == items::lingering_potion && potion(second[1]) == "minecraft:swiftness" && second[3].count == 1,
                "dragon's breath made a lingering potion of the same potion");
        const auto d = s.oracle.dump();
        require(d.overworld().entities.size() == entities + 1 && d.overworld().entities.back().stack.item == items::glass_bottle,
                "and threw out its glass bottle");
        // Awkward potions take no nether wart: the stand does not brew again.
        s.until(s.oracle.time() + 401);
        require(potion(slots({0, 0, 0})[0]) == "minecraft:awkward" && slots({0, 0, 0})[3].count == 1, "nothing more to brew");
    });

    suite.run("brewing stand windows put fuel, ingredients and bottles where they go", [] {
        using circuit::contracts::ComponentPatch;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::PlayerKey;
        using circuit::contracts::SetPlayerSlot;
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->brewing_fuel = {items::blaze_powder};
        data->potion_of = [](const ComponentPatch& patch) -> std::optional<std::string_view> {
            if (patch.empty()) return std::nullopt;
            return circuit::gameplay::potions::names()[0];
        };
        data->with_potion = [](const ComponentPatch&, std::string_view) {
            return std::optional(ComponentPatch::make(772, {{1, {std::byte{0}}}}, {}, {}));
        };
        const PlayerKey alex{7, 8};
        const BlockPos stand{0, 0, 0};
        Sim s(bundle(), 7, 2, data);
        const ItemStack water{items::potion, 1, 0, *data->with_potion({}, "minecraft:water")};
        s.tick({place(stand, blocks::brewing_stand()), SetPlayerSlot{alex, 36, {items::blaze_powder, 3, 0}}, SetPlayerSlot{alex, 37, {items::stone, 2, 0}},
                SetPlayerSlot{alex, 38, {items::glass_bottle, 3, 0}}, SetPlayerSlot{alex, 39, water}});
        // Brewing stand window: 0-2 bottles, 3 ingredient, 4 fuel, 5-31 the main inventory, 32-40 the hotbar.
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::optional(stand), slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, alex, &PlayerInventory::player);
        };
        const auto slot = [&](std::size_t i) { return block_entity_at(s.oracle.dump(), stand)->slots[i]; };
        s.tick({click(32, 0, 1), click(33, 0, 1), click(34, 0, 1), click(35, 0, 1)});
        require(slot(4) == ItemStack{items::blaze_powder, 2, 0} && slot(3) == ItemStack{items::stone, 2, 0},
                "blaze powder into the fuel slot (one burned), stone into the ingredient slot");
        require(slot(0).item == items::glass_bottle && slot(1).item == items::glass_bottle && slot(2).item == items::glass_bottle
                    && slot(2).count == 1 && player().slots[38].empty(),
                "one glass bottle into each bottle slot");
        require(player().slots[39] == water, "a bottle with every bottle slot full stays where it is");
        s.tick({click(3, 0, 0), click(4, 0, 0), click(1, 0, 0)});
        require(player().cursor == ItemStack{items::stone, 2, 0} && slot(4).count == 2 && slot(1).item == items::glass_bottle,
                "stone goes into neither the fuel slot nor a bottle slot");
        s.tick({click(3, 0, 0), click(0, 0, 1)});
        require(slot(3).item == items::stone && slot(0).empty() && player().slots[44].item == items::glass_bottle,
                "a bottle goes out to the hotbar's end");
    });

    suite.run("crafters craft on power and fill evenly from hoppers", [] {
        namespace recipes = circuit::gameplay::recipes;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->crafting = {
            {recipes::Recipe::Kind::shapeless, "test:planks", 0, 0, {recipes::Ingredient{{items::oak_log}}}, {items::oak_planks, 4}},
            {recipes::Recipe::Kind::shapeless, "test:sugar", 0, 0, {recipes::Ingredient{{items::milk_bucket}}}, {item("minecraft:sugar"), 1}},
        };
        Sim s(bundle(), 7, 2, data);
        s.tick(floor(-3, 12, -1, 6));
        const auto slots = [&](circuit::contracts::BlockPos p) {
            const auto d = s.oracle.dump();
            for (const auto& [pos, be] : d.overworld().block_entities)
                if (pos == p) return be.slots;
            return std::vector<ItemStack>{};
        };
        // A crafter facing a chest, powered from the side; another powered
        // only through the block above it (no quasi-connectivity).
        s.tick({place({0, 0, 0}, blocks::crafter(Direction::east)), place({1, 0, 0}, blocks::chest(Direction::west)),
                place({4, 0, 0}, blocks::crafter(Direction::east)), place({4, 1, 0}, blocks::stone()),
                insert({0, 0, 0}, 4, items::oak_log, 2), insert({4, 0, 0}, 4, items::oak_log, 2)});
        s.tick({place({0, 0, 1}, blocks::redstone_block()), place({4, 2, 0}, blocks::redstone_block())});
        require(s.at({0, 0, 0}).flag() && !s.at({4, 0, 0}).flag(), "triggered by power at the block only");
        s.until(s.oracle.time() + 3);
        require(slots({0, 0, 0})[4].count == 1 && slots({1, 0, 0})[0] == ItemStack{items::oak_planks, 4} && s.at({0, 0, 0}).extra() == 1,
                "crafted into the chest, and shows it");
        s.until(s.oracle.time() + 6);
        require(s.at({0, 0, 0}).extra() == 0 && slots({4, 0, 0})[4].count == 2, "for six ticks; the other never crafted");
        // A milk bucket's remainder is thrown out after the result.
        s.tick({place({8, 0, 0}, blocks::crafter(Direction::east)), insert({8, 0, 0}, 0, items::milk_bucket, 1)});
        const auto before = s.oracle.dump().overworld().entities.size();
        s.tick({place({8, 0, 1}, blocks::redstone_block())});
        s.until(s.oracle.time() + 4);
        const auto d = s.oracle.dump();
        require(d.overworld().entities.size() == before + 2 && d.overworld().entities[before].stack.item == item("minecraft:sugar") &&
                    d.overworld().entities[before + 1].stack.item == items::bucket && slots({8, 0, 0})[0].empty(),
                "sugar, then the bucket");
        // A hopper fills the grid evenly: empty slots first (around a stick),
        // then the smallest stacks of its kind.
        s.tick({place({12, 0, 0}, blocks::crafter(Direction::east)), place({12, 1, 0}, blocks::hopper(Direction::down)),
                insert({12, 0, 0}, 4, item("minecraft:stick"), 1), insert({12, 1, 0}, 0, items::oak_planks, 10)});
        s.until(s.oracle.time() + 80);
        const auto grid = slots({12, 0, 0});
        require(grid[0].count == 2 && grid[1].count == 2 && grid[2].count == 1 && grid[8].count == 1 && grid[4].item == item("minecraft:stick") &&
                    grid[4].count == 1,
                "eight planks around the stick, then a second in slots 0 and 1");
    });

    suite.run("crafter windows disable empty slots and keep items out of them", [] {
        using circuit::contracts::ContainerClick;
        using circuit::contracts::SetPlayerSlot;
        using circuit::contracts::SetSlotEnabled;
        const PlayerKey alex{4, 2};
        const BlockPos crafter{0, 0, 0};
        Sim s;
        s.tick({place(crafter, blocks::crafter(Direction::east)), insert(crafter, 0, items::oak_log, 3),
                SetPlayerSlot{alex, 36, {items::stone, 5}}, SetPlayerSlot{alex, 37, {items::oak_planks, 4}}});
        const auto be = [&] { return *block_entity_at(s.oracle.dump(), crafter); };
        // Crafter window: 0-8 the grid, 9-35 the main inventory, 36-44 the hotbar (45, the result shown, is no slot).
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::optional(crafter), slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        s.tick({SetSlotEnabled{alex, crafter, 1, false}, SetSlotEnabled{alex, crafter, 0, false}});
        require(be().disabled == 0b10, "slot 1 disabled; slot 0 holds logs and stays enabled");
        s.tick({click(36, 0, 1)});
        require(be().slots[2] == ItemStack{items::stone, 5} && be().slots[1].empty(), "a shift-click skips the disabled slot");
        s.tick({click(37, 0, 0), click(1, 1, 0), click(45, 0, 0)});
        require(be().slots[1].empty() && s.oracle.dump().players[0].cursor.count == 4, "nothing goes into a disabled slot or the result");
        s.tick({click(37, 0, 0), SetSlotEnabled{alex, crafter, 1, true}});
        require(be().disabled == 0, "enabled again");
        s.tick({click(2, 0, 1)});
        require(be().slots[2].empty() && s.oracle.dump().players[0].slots[44] == ItemStack{items::stone, 5}, "out to the hotbar's end");
    });

    suite.run("idle levels and random tick ranges", [] {
        // A level is awake while active, and for 300 ticks after it was last
        // active or started.
        require(level_awake(true, 1000, 0, 0) && level_awake(false, 300, 0, 0) && !level_awake(false, 301, 0, 0)
                    && level_awake(false, 400, 100, 0) && !level_awake(false, 401, 100, 0) && level_awake(false, 900, 0, 600),
                "idle after 300 ticks");
        // 1.21.4: within 128 blocks (horizontally, exclusive) of a
        // non-spectator player's position, from the chunk's center.
        std::vector<Presence> presences{{PlayerKey{1, 1}, Vec3{8.0, 0.0, 8.0}, false}};
        const auto near = [&](ChunkPos c) { return player_near(presences, c); };
        require(near({0, 0}) && near({7, 0}) && !near({8, 0}) && near({-7, 0}) && !near({-8, 0}) && near({5, 5}) && !near({6, 6}),
                "128 blocks from the chunk center");
        presences[0].position.x = 8.25;
        require(near({8, 0}) && !near({-8, 0}), "measured exactly");
        presences[0].spectator = true;
        require(!near({0, 0}), "spectators do not count");
        presences[0].spectator = false;
        using enum RandomTickRange;
        require(random_ticks_run(entity_ticking, 31, {100, 0}, presences) && !random_ticks_run(entity_ticking, 32, {0, 0}, presences),
                "1.21.8: entity-ticking simulation");
        require(random_ticks_run(block_ticking_near_player, 32, {0, 0}, presences)
                    && !random_ticks_run(block_ticking_near_player, 31, {100, 0}, presences)
                    && !random_ticks_run(block_ticking_near_player, 33, {0, 0}, presences),
                "1.21.4: block-ticking simulation near a player");
    });

    suite.run("spawn positions are checked before narrowing", [] {
        Sim s;
        const auto before = s.oracle.dump();
        // 2^32 blocks: narrowing first would alias block (0, 1, 0); also NaN and infinite positions.
        s.tick({SpawnItem{{4294967296.0, 1.0, 0.0}, {}, {items::stone, 1}}, SpawnItem{{0.0, -1e300, 0.0}, {}, {items::stone, 1}},
                SpawnItem{{std::nan(""), 1.0, 0.0}, {}, {items::stone, 1}},
                SpawnItem{{0.0, std::numeric_limits<double>::infinity(), 0.0}, {}, {items::stone, 1}}});
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == 4 && std::ranges::all_of(rejected, [](const auto& r) { return r.b == static_cast<std::uint64_t>(ErrorCode::out_of_bounds); }),
                "rejected as out of bounds");
        auto after = s.oracle.dump();
        after.time = before.time;
        after.day_time = before.day_time;
        after.weather = before.weather;
        for (auto& l : after.levels) l.last_active = before.level(l.dimension).last_active;
        require(after == before, "no world mutation");
        // Valid endpoints: the lowest and highest positions inside the world height.
        const auto& world = bundle().world;
        s.tick({SpawnItem{{0.5, static_cast<double>(world.min_y), 0.5}, {}, {items::stone, 1}},
                SpawnItem{{0.5, std::nextafter(static_cast<double>(world.min_y + world.height), 0.0), 0.5}, {}, {items::stone, 1}},
                SpawnItem{{std::nextafter(static_cast<double>(world_border), 0.0), 0.0, 0.0}, {}, {items::stone, 1}}});
        require(of_kind(s.last, TraceKind::entity_spawned).size() == 2, "height endpoints accepted");
        const auto last = of_kind(s.last, TraceKind::command_rejected);
        require(last.size() == 1 && last[0].b == static_cast<std::uint64_t>(ErrorCode::unloaded_chunk),
                "inside the border but unloaded");
    });

    suite.run("spawned items take a pickup delay and an age", [] {
        Sim s;
        // Out of range: a pickup delay above 32767, ages outside -32768..32767.
        s.tick({SpawnItem{at_block(15.5, 1.0, 0.5), {}, {items::stone, 1}, 32768, 0}, SpawnItem{at_block(15.5, 1.0, 0.5), {}, {items::stone, 1}, 0, -32769},
                SpawnItem{at_block(15.5, 1.0, 0.5), {}, {items::stone, 1}, 0, 32768}});
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == 3 && std::ranges::all_of(rejected, [](const auto& r) { return r.b == static_cast<std::uint64_t>(ErrorCode::invalid_argument); }),
                "rejected");
        // 32767 never counts down and -32768 never ages; age 5995 despawns in its fifth tick.
        s.tick({SpawnItem{at_block(15.5, 1.0, 0.5), {}, {items::stone, 1}, 32767, -32768}, SpawnItem{at_block(15.5, 1.0, 4.5), {}, {items::stone, 1}, 7, 5995}});
        for (int t = 2; t <= 5; ++t) {
            require(of_kind(s.last, TraceKind::entity_removed).empty(), std::format("not yet at tick {}", t - 1));
            s.tick({});
        }
        const auto removed = of_kind(s.last, TraceKind::entity_removed);
        require(removed.size() == 1 && removed[0].b == static_cast<std::uint64_t>(circuit::gameplay::RemovalReason::despawned), "despawned");
        const auto d = s.oracle.dump();
        require(d.overworld().entities.size() == 1 && d.overworld().entities[0].age == -32768 && d.overworld().entities[0].pickup_delay == 32767
                    && d.overworld().entities[0].tick_count == 5,
                "never ages, never picked up; the tick count still runs");
    });

    suite.run("falling blocks land, break on what they cannot replace, and leave the world", [] {
        Sim s;
        const auto sand = registry_block("sand");
        // Sand above two snow layers (a block that can be replaced, but only
        // with a single layer) breaks into its item; above air it lands.
        s.tick({place({0, -1, 0}, blocks::stone()), place({2, -1, 0}, blocks::stone()),
                place({0, 0, 0}, registry_block("snow", {{"layers", "2"}})), place({0, 4, 0}, sand), place({2, 4, 0}, sand)});
        s.until(20);
        auto d = s.oracle.dump();
        require(s.at({2, 0, 0}) == sand && s.at({2, 4, 0}).is_air(), "landed on stone");
        require(s.at({0, 0, 0}) == registry_block("snow", {{"layers", "2"}}), "the snow stays");
        require(std::ranges::count_if(d.overworld().entities, [](const auto& e) {
                    return e.kind == circuit::gameplay::EntityKind::item && e.stack.item == items::by_name("minecraft:sand");
                }) == 1,
                "broke into one sand item");
        // Sand at the bottom of the world falls out of it: after 100 ticks
        // below its height it breaks too, and its item falls further.
        const auto bottom = bundle().world.min_y;
        s.tick({place({4, bottom, 0}, sand)});
        s.until(s.oracle.time() + 2);
        const auto falling = [&] {
            return std::ranges::count_if(s.oracle.dump().overworld().entities,
                                         [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::falling_block; });
        };
        require(falling() == 1, "falling");
        for (int i = 0; i < 101 && falling() == 1; ++i) s.tick();
        require(falling() == 0, "gone after 100 ticks");
        const auto removed = of_kind(s.last, TraceKind::entity_removed);
        const auto spawned = of_kind(s.last, TraceKind::entity_spawned);
        require(removed.size() == 1 && spawned.size() == 1 && spawned[0].c == static_cast<std::uint64_t>(circuit::gameplay::EntityKind::item),
                "broke into its item");
    });

    suite.run("redstone primes TNT, whose fuse runs out; tntExplodes off leaves it", [] {
        const auto tnt = registry_block("tnt");
        const auto primed = [](const Sim& s) {
            std::vector<circuit::gameplay::Entity> out;
            const auto d = s.oracle.dump();
            for (const auto& e : d.overworld().entities)
                if (e.kind == circuit::gameplay::EntityKind::primed_tnt) out.push_back(e);
            return out;
        };
        Sim s;
        // Placed beside a redstone block: the entity first, then the block goes.
        s.tick(with_floor(floor(0, 4, 0, 0), {place({1, 0, 0}, blocks::redstone_block()), place({0, 0, 0}, tnt), place({4, 0, 0}, tnt)}));
        const auto spawned = of_kind(s.last, TraceKind::entity_spawned);
        require(spawned.size() == 1 && spawned[0].b == 80 && spawned[0].c == static_cast<std::uint64_t>(circuit::gameplay::EntityKind::primed_tnt),
                "primed with a fuse of 80");
        require(s.at({0, 0, 0}).is_air() && s.at({4, 0, 0}) == tnt, "the powered block went, the other stays");
        auto p = primed(s);
        require(p.size() == 1 && p[0].fuse == 79 && p[0].position.y > 0.0, "launched upwards and counting");
        // An unpowered update leaves TNT alone; a lever beside it primes it.
        s.tick({place({4, 0, 1}, blocks::stone())});
        require(s.at({4, 0, 0}) == tnt, "no signal");
        s.tick({place({3, 0, 0}, blocks::lever(true, D::north, blocks::LeverFace::floor))});
        require(s.at({4, 0, 0}).is_air() && primed(s).size() == 2, "the lever primes it");
        // The fuse runs out: the entity is removed as exploded.
        while (primed(s).size() == 2) s.tick();
        const auto removed = of_kind(s.last, TraceKind::entity_removed);
        require(removed.size() == 1 && removed[0].b == static_cast<std::uint64_t>(circuit::gameplay::RemovalReason::exploded), "exploded");
        // With tntExplodes off (1.21.8) redstone does not prime TNT.
        s.tick({SetGameRule{"tntExplodes", 0}, place({8, -1, 0}, blocks::stone()), place({8, 0, 0}, tnt), place({9, 0, 0}, blocks::redstone_block())});
        require(s.at({8, 0, 0}) == tnt && primed(s).size() == 1, "not primed with the rule off");
        // 1.21.4 has no such rule: TNT is always primed.
        Sim old(**BundleRegistry::select_for_simulation("java-1.21.4"));
        old.tick({place({0, -1, 0}, blocks::stone()), place({0, 0, 0}, tnt), place({1, 0, 0}, blocks::redstone_block())});
        require(old.at({0, 0, 0}).is_air() && primed(old).size() == 1, "1.21.4 primes it");
    });

    suite.run("explosions break blocks by resistance, merge their drops, prime TNT and hurt items", [] {
        const auto tnt = registry_block("tnt");
        // A stone box around TNT, with obsidian on one side, more TNT and an
        // item inside.
        Sim s;
        std::vector<CommandBody> build;
        for (int x = -3; x <= 3; ++x)
            for (int y = -1; y <= 2; ++y)
                for (int z = -3; z <= 3; ++z)
                    if (std::abs(x) == 3 || std::abs(z) == 3 || y == -1 || y == 2) build.push_back(place({x, y, z}, blocks::stone()));
        build.push_back(place({0, 0, 3}, blocks::obsidian()));
        build.push_back(place({-2, 0, 0}, tnt));
        build.push_back(place({0, 0, 0}, tnt));
        build.push_back(place({1, 0, 0}, blocks::redstone_block()));
        s.tick(build);
        s.tick({SpawnItem{at_block(1.5, 0.0, 2.5), {}, {items::stone, 1}}});
        const auto tick = [&] {
            s.tick();
            return !of_kind(s.last, TraceKind::explosion).empty();
        };
        bool exploded = false;
        for (int i = 0; i < 100 && !exploded; ++i) exploded = tick();
        require(exploded, "the fuse ran out");
        require(s.at({0, -1, 0}).is_air() && s.at({1, 0, 0}).is_air(), "stone and the redstone block next to it are gone");
        require(s.at({0, 0, 3}) == blocks::obsidian(), "obsidian resists");
        const auto d = s.oracle.dump();
        int cobblestone = 0, stacks = 0, primed = 0;
        for (const auto& e : d.overworld().entities) {
            if (e.kind == circuit::gameplay::EntityKind::item && e.stack.item == items::cobblestone) {
                cobblestone += e.stack.count;
                ++stacks;
                require(e.stack.count <= 16, "drops merge into stacks of at most 16");
            }
            if (e.kind == circuit::gameplay::EntityKind::primed_tnt) {
                ++primed;
                require(e.fuse >= 9 && e.fuse <= 28, std::format("an explosion's fuse: {}", e.fuse));
            }
            require(!(e.kind == circuit::gameplay::EntityKind::item && e.stack.item == items::stone), "the item in the open is destroyed");
        }
        require(cobblestone > 16 && stacks == (cobblestone + 15) / 16, std::format("{} cobblestone in {} stacks", cobblestone, stacks));
        require(primed == 1 && s.at({-2, 0, 0}).is_air(), "the other TNT is primed");
        // Waterlogged blocks resist as water does.
        require(circuit::gameplay::blast_resistance(blocks::water_source()) == 100.0f
                    && circuit::gameplay::blast_resistance(registry_block("oak_slab", {{"type", "bottom"}, {"waterlogged", "true"}})) == 100.0f
                    && circuit::gameplay::blast_resistance(registry_block("oak_slab", {{"type", "bottom"}, {"waterlogged", "false"}})) == 3.0f
                    && circuit::gameplay::blast_resistance(blocks::obsidian()) == 1200.0f,
                "resistances");
    });

    suite.run("arrows stick, fall when their block goes and prime TNT when burning; snowballs break", [] {
        const auto tnt = registry_block("tnt");
        Sim s;
        std::vector<CommandBody> build = floor(-1, 12, -2, 2);
        for (int y = 0; y <= 3; ++y) build.push_back(place({10, y, 0}, blocks::obsidian()));
        build.push_back(place({10, 0, 2}, tnt));
        build.push_back(place({10, 0, -2}, tnt));
        s.tick(build);
        s.tick({SummonEntity{"minecraft:arrow", {0.5, 1.5, 0.5}, {1.0, 0.1, 0.0}, 80, 0},
                SummonEntity{"minecraft:arrow", {4.5, 0.5, 2.5}, {1.0, 0.05, 0.0}, 80, 200},
                SummonEntity{"minecraft:arrow", {4.5, 0.5, -1.5}, {1.0, 0.05, 0.0}, 80, 0},
                SummonEntity{"minecraft:snowball", {0.5, 3.5, 0.5}, {1.0, 0.0, 0.0}, 80, 0}});
        const auto of = [&](circuit::gameplay::EntityKind kind) {
            std::vector<circuit::gameplay::Entity> out;
            const auto d = s.oracle.dump();
            for (const auto& e : d.overworld().entities)
                if (e.kind == kind) out.push_back(e);
            return out;
        };
        for (int i = 0; i < 15; ++i) s.tick();
        auto arrows = of(circuit::gameplay::EntityKind::arrow);
        require(arrows.size() == 3, "three arrows");
        const auto wall = std::ranges::find_if(arrows, [](const auto& e) { return std::abs(e.position.z - 0.5) < 0.1; });
        require(wall != arrows.end() && wall->in_ground && wall->position.x == 10.0 - static_cast<double>(0.05f) && wall->velocity == Vec3{},
                "stuck 0.05 short of the wall");
        require(s.at({10, 0, 2}).is_air() && s.at({10, 0, -2}) == tnt, "the burning arrow primed its TNT, the cold one did not");
        require(of(circuit::gameplay::EntityKind::primed_tnt).size() == 1, "one primed TNT");
        require(of(circuit::gameplay::EntityKind::snowball).empty(), "the snowball broke on the wall");
        // The wall goes: the arrow stuck in it falls to the floor.
        std::vector<CommandBody> clear;
        for (int y = 0; y <= 3; ++y) clear.push_back(place({10, y, 0}, blocks::air()));
        s.tick(clear);
        for (int i = 0; i < 30; ++i) s.tick();
        arrows = of(circuit::gameplay::EntityKind::arrow);
        const auto fell = std::ranges::find_if(arrows, [](const auto& e) { return std::abs(e.position.z - 0.5) < 0.1; });
        require(fell != arrows.end() && fell->in_ground && fell->position.y == static_cast<double>(0.05f) && fell->life > 0, "fell to the floor");
    });

    suite.run("minecarts slide off rails, boats float and push each other", [] {
        Sim s;
        std::vector<CommandBody> build = floor(-1, 14, -3, 6);
        for (int x = -1; x <= 9; ++x)
            for (int z = 1; z <= 6; ++z)
                build.push_back(place({x, 0, z}, x < 0 || x > 8 || z < 2 || z > 5 ? blocks::stone() : blocks::water_source()));
        s.tick(build);
        s.tick({SummonEntity{"minecraft:minecart", {0.5, 0.0, -1.5}, {1.5, 0.0, 0.0}, 80, 0},
                SummonEntity{"minecraft:oak_boat", {2.5, 0.5, 3.5}, {0.0, 0.0, 0.0}, 80, 0},
                SummonEntity{"minecraft:spruce_boat", {3.6, 0.5, 3.5}, {0.0, 0.0, 0.0}, 80, 0}});
        const auto of = [&](circuit::gameplay::EntityKind kind) {
            std::vector<circuit::gameplay::Entity> out;
            const auto d = s.oracle.dump();
            for (const auto& e : d.overworld().entities)
                if (e.kind == kind) out.push_back(e);
            return out;
        };
        auto cart = of(circuit::gameplay::EntityKind::minecart).at(0);
        require(cart.position.x == 0.9 && cart.on_ground, "the first move is held to 0.4");
        s.tick();
        cart = of(circuit::gameplay::EntityKind::minecart).at(0);
        require(cart.position.x == 1.1 && cart.velocity.x == 0.2, "on the ground it keeps half");
        for (int i = 0; i < 40; ++i) s.tick();
        const auto boats = of(circuit::gameplay::EntityKind::boat);
        require(boats.size() == 2 && boats[0].variant != boats[1].variant, "two kinds of boats");
        for (const auto& b : boats) require(b.position.y > 0.5 && b.position.y < 1.0 && b.status == 1, std::format("afloat at {}", b.position.y));
        require(std::abs(boats[0].position.x - boats[1].position.x) > 1.2, "pushed apart");
    });

    suite.run("redstone torches invert their block, burn out and relight", [] {
        using circuit::contracts::UseBlock;
        Sim s;
        std::vector<CommandBody> build = floor(-1, 8, -1, 4);
        build.push_back(place({1, 0, 0}, blocks::lamp()));
        build.push_back(place({0, 1, 0}, blocks::stone()));
        build.push_back(place({0, 2, 0}, blocks::lamp()));
        build.push_back(place({4, 0, 2}, blocks::stone()));
        build.push_back(place({4, 1, 2}, blocks::lever(false)));
        build.push_back(place({6, 0, 2}, blocks::lamp()));
        build.push_back(place({7, 0, 0}, blocks::redstone_block()));
        s.tick(build);
        s.tick({place({0, 0, 0}, blocks::torch()), place({5, 0, 2}, blocks::wall_torch(D::east)), place({7, 1, 0}, blocks::torch())});
        require(s.at({1, 0, 0}) == blocks::lamp(true) && s.at({0, 2, 0}) == blocks::lamp(true) && s.at({6, 0, 2}) == blocks::lamp(true),
                "torches light the lamps beside them, and through the block above them");
        s.tick();
        require(s.at({7, 1, 0}) == blocks::torch(false), "a torch on a redstone block goes out");
        // The lever powers the wall torch's block: off after 2 ticks.
        s.tick({UseBlock{{4, 1, 2}}});
        require(s.at({5, 0, 2}) == blocks::wall_torch(D::east, true), "still lit");
        s.tick();
        require(s.at({5, 0, 2}) == blocks::wall_torch(D::east, false), "turned off");
        s.tick({UseBlock{{4, 1, 2}}});
        s.tick();
        require(s.at({5, 0, 2}) == blocks::wall_torch(D::east, true), "on again");
        // Seven more turn-offs within 60 ticks burn it out on the eighth.
        Tick eighth = 0;
        for (int i = 0; i < 7; ++i) {
            s.tick({UseBlock{{4, 1, 2}}});
            s.tick();
            eighth = s.oracle.time();
            s.tick({UseBlock{{4, 1, 2}}});
            s.tick();
        }
        const auto d = s.oracle.dump();
        require(std::ranges::count(d.overworld().turn_offs, BlockPos{5, 0, 2}, &circuit::gameplay::TurnOffs::value_type::first) == 8,
                "eight turn-offs kept");
        require(s.at({5, 0, 2}) == blocks::wall_torch(D::east, false), "burnt out: off though unpowered");
        s.until(eighth + 159);
        require(s.at({5, 0, 2}) == blocks::wall_torch(D::east, false), "still out");
        s.tick();
        require(s.at({5, 0, 2}) == blocks::wall_torch(D::east, true), "relit 160 ticks after the eighth turn-off");
        require(s.oracle.dump().overworld().turn_offs.empty(), "old turn-offs forgotten");
        // Without its support a torch drops.
        s.tick({place({0, -1, 0}, blocks::air())});
        require(s.at({0, 0, 0}).is_air() && s.oracle.dump().overworld().entities.size() == 1, "the torch dropped");
    });

    suite.run("buttons stay pressed 20 or 30 ticks; arrows hold wooden ones", [] {
        using circuit::contracts::RemoveEntity;
        using circuit::contracts::UseBlock;
        using circuit::gameplay::blocks::ButtonKind;
        using circuit::gameplay::blocks::LeverFace;
        Sim s;
        std::vector<CommandBody> build = floor(-1, 8, -1, 6);
        build.push_back(place({0, 0, 0}, blocks::stone()));
        build.push_back(place({1, 0, 0}, blocks::button(ButtonKind::stone, false, D::east)));
        build.push_back(place({2, 0, 0}, blocks::lamp()));
        build.push_back(place({-1, 0, 0}, blocks::lamp()));
        build.push_back(place({4, 0, 0}, blocks::button(ButtonKind::oak, false, D::north, LeverFace::floor)));
        build.push_back(place({5, 0, 0}, blocks::lamp()));
        build.push_back(place({0, 1, 4}, blocks::stone()));
        build.push_back(place({1, 1, 4}, blocks::button(ButtonKind::oak, false, D::east)));
        build.push_back(place({0, 2, 4}, blocks::lamp()));
        s.tick(build);
        s.tick({UseBlock{{1, 0, 0}}, UseBlock{{4, 0, 0}}});
        const auto pressed = s.oracle.time();
        require(s.at({1, 0, 0}).flag() && s.at({4, 0, 0}).flag(), "pressed");
        require(s.at({2, 0, 0}).flag() && s.at({-1, 0, 0}).flag() && s.at({5, 0, 0}).flag(),
                "they power their neighbors, and their block strongly");
        s.tick({UseBlock{{1, 0, 0}}});
        require(of_kind(s.last, TraceKind::command_applied).size() == 1 && of_kind(s.last, TraceKind::block_changed).empty(),
                "using a pressed button does nothing");
        s.until(pressed + 18);
        require(s.at({1, 0, 0}).flag(), "a stone button holds for 20 ticks");
        s.tick();
        require(!s.at({1, 0, 0}).flag() && s.at({4, 0, 0}).flag(), "the stone button is out, the oak one still in");
        s.until(pressed + 29);
        require(!s.at({4, 0, 0}).flag(), "the oak button holds for 30 ticks");
        // An arrow sticks in the oak button's cell and holds it pressed.
        s.tick({SummonEntity{"minecraft:arrow", {5.5, 1.5, 4.5}, {-1.0, 0.04, 0.0}, 80, 0}});
        for (int i = 0; i < 6; ++i) s.tick();
        require(s.at({1, 1, 4}).flag() && s.at({0, 2, 4}).flag(), "pressed by the arrow");
        for (int i = 0; i < 60; ++i) s.tick();
        require(s.at({1, 1, 4}).flag(), "held while the arrow is in it");
        const auto arrow = s.oracle.dump().overworld().entities.at(0).id;
        s.tick({RemoveEntity{arrow}});
        for (int i = 0; i < 30; ++i) s.tick();
        require(!s.at({1, 1, 4}).flag(), "out once the arrow is gone");
        // Players place buttons and torches on the clicked face; buttons
        // drop themselves.
        const circuit::contracts::PlayerKey alex{1, 2};
        s.tick({circuit::contracts::SetPlayerSlot{alex, 36, {items::stone_button, 2, 0}},
                circuit::contracts::SetPlayerSlot{alex, 37, {items::redstone_torch, 2, 0}},
                circuit::contracts::PlayerUse{alex, 36, {7, -1, 5}, D::up, D::west, D::down, false, false, 3, 3},
                circuit::contracts::PlayerUse{alex, 36, {0, 1, 4}, D::down, D::west, D::up, false, false, 3, 3},
                circuit::contracts::PlayerUse{alex, 37, {0, 1, 4}, D::south, D::north, D::north, false, false, 3, 3},
                circuit::contracts::PlayerUse{alex, 37, {8, -1, 6}, D::up, D::north, D::down, false, false, 3, 3}});
        require(s.at({7, 0, 5}) == blocks::button(ButtonKind::stone, false, D::west, LeverFace::floor), "a floor button");
        require(s.at({0, 0, 4}) == blocks::button(ButtonKind::stone, false, D::west, LeverFace::ceiling), "a ceiling button");
        require(s.at({0, 1, 5}) == blocks::wall_torch(D::south), "a wall torch");
        require(s.at({8, 0, 6}) == blocks::torch(), "a floor torch");
        s.tick({circuit::contracts::PlayerBreak{alex, {7, 0, 5}, true, 38}});
        const auto d = s.oracle.dump();
        require(std::ranges::any_of(d.overworld().entities, [](const auto& e) { return e.stack.item == items::stone_button; }),
                "a stone button drops by hand");
    });

    suite.run("pressure plates sense entities; target blocks grade hits", [] {
        using circuit::gameplay::blocks::ButtonKind;
        Sim s;
        std::vector<CommandBody> build = floor(-2, 8, -1, 8);
        build.push_back(place({0, 0, 0}, blocks::pressure_plate(ButtonKind::oak)));
        build.push_back(place({0, 0, 1}, blocks::lamp()));
        build.push_back(place({0, 0, 3}, blocks::pressure_plate(ButtonKind::stone)));
        build.push_back(place({0, 0, 4}, blocks::lamp()));
        build.push_back(place({0, 0, 6}, blocks::weighted_plate(false)));
        build.push_back(place({4, 0, 6}, blocks::weighted_plate(true)));
        s.tick(build);
        std::vector<CommandBody> drops{SpawnItem{at_block(0.5, 0.5, 0.5), {}, {items::stone, 1}},
                                       SpawnItem{at_block(0.5, 0.5, 3.5), {}, {items::stone, 1}}};
        for (int i = 0; i < 5; ++i) {
            drops.push_back(SpawnItem{at_block(0.2 + 0.15 * i, 0.1, 6.5), {}, {items::stone, 1}});
            drops.push_back(SpawnItem{at_block(4.2 + 0.15 * i, 0.1, 6.5), {}, {items::stone, 1}});
        }
        s.tick(drops);
        for (int i = 0; i < 25; ++i) s.tick();
        require(s.at({0, 0, 0}) == blocks::pressure_plate(ButtonKind::oak, true) && s.at({0, 0, 1}).flag(), "an item presses a wooden plate");
        require(s.at({0, 0, 3}) == blocks::pressure_plate(ButtonKind::stone) && !s.at({0, 0, 4}).flag(), "but not a stone one");
        require(s.at({0, 0, 6}) == blocks::weighted_plate(false, 5), "a light plate counts five items");
        require(s.at({4, 0, 6}) == blocks::weighted_plate(true, 1), "a heavy one a level per ten");
        // Without its support a plate drops; the lamp goes out 4 ticks later.
        s.tick({place({0, -1, 0}, blocks::air())});
        require(s.at({0, 0, 0}).is_air(), "the plate dropped");
        for (int i = 0; i < 4; ++i) s.tick();
        require(!s.at({0, 0, 1}).flag(), "the lamp is out");
        // An arrow at the center of a target's face gives 15 for 20 ticks; a
        // snowball off center gives less, for 8.
        s.tick({place({4, 0, 8}, blocks::target()), place({4, 0, 4}, blocks::target()),
                SummonEntity{"minecraft:arrow", {9.5, 0.73, 8.5}, {-1.0, 0.04, 0.0}, 80, 0},
                SummonEntity{"minecraft:snowball", {8.5, 0.6, 4.8}, {-1.0, 0.1, 0.0}, 80, 0}});
        for (int i = 0; i < 5; ++i) s.tick();
        const auto centered = s.at({4, 0, 8}).level(), off = s.at({4, 0, 4}).level();
        require(centered == 15 && off > 1 && off < 15, std::format("centered {}, off center {}", centered, off));
        for (int i = 0; i < 8; ++i) s.tick();
        require(s.at({4, 0, 4}).level() == 0 && s.at({4, 0, 8}).level() == 15, "the snowball's 8 ticks are over, not the arrow's 20");
        for (int i = 0; i < 12; ++i) s.tick();
        require(s.at({4, 0, 8}).level() == 0, "the arrow's too");
    });

    suite.run("doors, trapdoors and fence gates open by power and by hand", [] {
        using circuit::contracts::UseBlock;
        using circuit::gameplay::blocks::DoorKind;
        Sim s;
        std::vector<CommandBody> build = floor(-2, 8, -1, 8);
        build.push_back(place({0, 0, 0}, blocks::door(DoorKind::oak, D::east, false)));
        build.push_back(place({0, 1, 0}, blocks::door(DoorKind::oak, D::east, true)));
        build.push_back(place({0, 0, 1}, blocks::lever(false)));
        build.push_back(place({3, 0, 0}, blocks::door(DoorKind::iron, D::east, false)));
        build.push_back(place({3, 1, 0}, blocks::door(DoorKind::iron, D::east, true)));
        build.push_back(place({0, 0, 4}, blocks::trapdoor(DoorKind::oak, D::north)));
        build.push_back(place({3, 0, 4}, blocks::fence_gate(DoorKind::oak, D::north)));
        s.tick(build);
        const auto draws = [&] {
            std::uint64_t n = 0;
            for (const auto& r : of_kind(s.last, TraceKind::level_random_draw)) n += r.a;
            return n;
        };
        s.tick({UseBlock{{0, 0, 1}}});
        require(s.at({0, 0, 0}) == blocks::door(DoorKind::oak, D::east, false, false, true, true)
                    && s.at({0, 1, 0}) == blocks::door(DoorKind::oak, D::east, true, false, true, true),
                "both halves open with the power");
        require(draws() == 1, "one draw for the sound");
        s.tick({UseBlock{{0, 1, 0}}});
        require(!s.at({0, 0, 0}).flag() && s.at({0, 0, 0}).extra() == 1, "closed by hand, still powered");
        s.tick({UseBlock{{0, 0, 1}}});
        require(!s.at({0, 0, 0}).flag() && s.at({0, 0, 0}).extra() == 0, "unpowered, it stays closed");
        require(draws() == 0, "no sound without a move");
        s.tick({UseBlock{{3, 0, 0}}, UseBlock{{0, 0, 4}}, UseBlock{{3, 0, 4}}});
        require(!s.at({3, 0, 0}).flag(), "hands do not open iron doors");
        require(s.at({0, 0, 4}).flag() && s.at({3, 0, 4}) == blocks::fence_gate(DoorKind::oak, D::south, true),
                "the trapdoor opens; the gate opens away from the player facing south");
        // A half without its other half breaks; the lower one drops the door.
        s.tick({place({0, 1, 0}, blocks::air())});
        require(s.at({0, 0, 0}).is_air(), "the lower half broke");
        const auto d = s.oracle.dump();
        require(std::ranges::count_if(d.overworld().entities, [](const auto& e) { return e.stack.item == items::oak_door; }) == 1,
                "one door dropped");
        // Players place a door with its upper half.
        const circuit::contracts::PlayerKey alex{1, 2};
        s.tick({circuit::contracts::SetPlayerSlot{alex, 36, {items::iron_door, 1, 0}},
                circuit::contracts::PlayerUse{alex, 36, {6, -1, 6}, D::up, D::north, D::down, false, false, 3, 3}});
        require(s.at({6, 0, 6}) == blocks::door(DoorKind::iron, D::north, false) && s.at({6, 1, 6}) == blocks::door(DoorKind::iron, D::north, true),
                "a placed door");
    });

    suite.run("honey sticks in piston structures, slows and lets things slide", [] {
        using circuit::contracts::UseBlock;
        Sim s;
        std::vector<CommandBody> build = floor(-2, 10, -1, 10);
        // A pushed slime block takes the stone beside it but not the honey.
        build.push_back(place({0, 0, 0}, blocks::piston(D::east)));
        build.push_back(place({1, 0, 0}, blocks::slime()));
        build.push_back(place({1, 0, 1}, blocks::honey()));
        build.push_back(place({1, 0, -1}, blocks::stone()));
        build.push_back(place({-1, 0, 0}, blocks::lever(false)));
        // A honey wall, and honey to slide on.
        for (int y = 0; y <= 3; ++y) build.push_back(place({6, y, 4}, blocks::honey()));
        build.push_back(place({4, 0, 8}, blocks::honey()));
        s.tick(build);
        s.tick({UseBlock{{-1, 0, 0}},
                SpawnItem{at_block(5.93, 5.5, 4.5), {}, {items::stone, 1}},
                SpawnItem{at_block(4.5, 0.9375, 8.5), {0.0, 0.0, 0.2}, {items::dirt, 1}}});
        const auto item = [&](std::uint16_t kind) {
            const auto d = s.oracle.dump();
            return *std::ranges::find_if(d.overworld().entities, [&](const auto& e) { return e.stack.item == kind; });
        };
        require(std::abs(item(items::dirt).velocity.z - 0.2 * 0.4 * 0.6 * 0.98) < 1e-6, "honey slows a sliding item to 40% a tick");
        for (int i = 0; i < 3; ++i) s.tick();
        require(s.at({2, 0, 0}).is(BlockType::slime_block) && s.at({2, 0, -1}).is(BlockType::stone) && s.at({1, 0, 1}).is(BlockType::honey_block),
                "slime and honey do not stick to each other");
        for (int i = 0; i < 12; ++i) s.tick();
        const auto slider = item(items::stone);
        require(slider.velocity.y == (-0.05 - 0.08) * static_cast<double>(0.98f) * 0.98 && slider.position.y < 4.0,
                std::format("sliding down the honey at {}", slider.velocity.y));
    });

    suite.run("daylight detectors follow the sky every 20 ticks", [] {
        using circuit::contracts::SetDayTime;
        using circuit::contracts::SetWeather;
        using circuit::contracts::UseBlock;
        using circuit::contracts::WeatherKind;
        Sim s;
        std::vector<CommandBody> build = floor(-2, 6, -1, 3);
        build.push_back(place({0, 0, 0}, blocks::daylight_detector()));
        build.push_back(place({0, 0, 1}, blocks::lamp()));
        build.push_back(place({2, 0, 0}, blocks::daylight_detector(true)));
        // Under a roof the sky light is 14.
        build.push_back(place({4, 0, 0}, blocks::daylight_detector()));
        build.push_back(place({4, 1, 0}, blocks::stone()));
        build.push_back(SetGameRule{"doDaylightCycle", 0});
        build.push_back(SetDayTime{6000});
        s.tick(build);
        s.until(19);
        require(s.at({0, 0, 0}) == blocks::daylight_detector() && !s.at({0, 0, 1}).flag(), "nothing before a multiple of 20");
        s.tick();
        require(s.at({0, 0, 0}) == blocks::daylight_detector(false, 15) && s.at({0, 0, 1}).flag(), "15 at noon, the lamp lit");
        require(s.at({2, 0, 0}) == blocks::daylight_detector(true, 0) && s.at({4, 0, 0}) == blocks::daylight_detector(false, 14),
                "inverted 0, roofed 14");
        // At midnight the sky darkens by 11: the sun is down, an inverted
        // detector gives 15 - (15 - 11).
        s.tick({SetDayTime{18000}});
        s.until(40);
        require(s.at({0, 0, 0}) == blocks::daylight_detector() && s.at({2, 0, 0}) == blocks::daylight_detector(true, 11)
                    && s.at({4, 0, 0}) == blocks::daylight_detector(),
                "midnight");
        // Using a detector inverts it and sets its output at once.
        s.tick({UseBlock{{4, 0, 0}}});
        require(s.at({4, 0, 0}) == blocks::daylight_detector(true, 12), "inverted under the roof: 15 - (14 - 11)");
        // Rain darkens the noon sky by 3 once its level is full.
        s.tick({SetDayTime{6000}, SetWeather{WeatherKind::rain, 100000}});
        s.until(160);
        require(s.at({0, 0, 0}) == blocks::daylight_detector(false, 12) && s.at({2, 0, 0}) == blocks::daylight_detector(true, 3),
                std::format("noon in the rain: {} and {}", s.at({0, 0, 0}).level(), s.at({2, 0, 0}).level()));
    });

    suite.run("note blocks take their instrument and play by power and use", [] {
        using circuit::contracts::UseBlock;
        using circuit::gameplay::registry::Instrument;
        const auto note = [](Instrument i, std::uint8_t n = 0, bool powered = false) {
            return blocks::note_block(static_cast<std::uint8_t>(i), n, powered);
        };
        Sim s;
        std::vector<CommandBody> build = floor(-2, 8, -1, 4);
        build.push_back(place({0, 0, 0}, blocks::note_block()));
        build.push_back(place({1, 0, 0}, blocks::lever(false)));
        build.push_back(place({4, 0, 0}, blocks::note_block()));
        build.push_back(place({4, 1, 0}, blocks::stone()));
        s.tick(build);
        const auto draws = [&] {
            std::uint64_t n = 0;
            for (const auto& r : of_kind(s.last, TraceKind::level_random_draw)) n += r.a;
            return n;
        };
        // Set by commands as given; a shape update from below gives the
        // instrument of the block there.
        s.tick({place({0, -1, 0}, registry_block("oak_planks")), place({4, -1, 0}, registry_block("gold_block"))});
        require(s.at({0, 0, 0}) == note(Instrument::bass) && s.at({4, 0, 0}) == note(Instrument::bell), "bass on planks, bell on gold");
        s.tick({UseBlock{{1, 0, 0}}});
        require(s.at({0, 0, 0}) == note(Instrument::bass, 0, true) && draws() == 2, "a rising signal plays it: one long");
        s.tick({UseBlock{{1, 0, 0}}});
        require(s.at({0, 0, 0}) == note(Instrument::bass) && draws() == 0, "a falling one does not");
        // Used: the next note, played; under stone it stays silent.
        s.tick({UseBlock{{0, 0, 0}}, UseBlock{{4, 0, 0}}});
        require(s.at({0, 0, 0}) == note(Instrument::bass, 1) && s.at({4, 0, 0}) == note(Instrument::bell, 1) && draws() == 2,
                "one sound, two notes");
        for (int i = 0; i < 24; ++i) s.tick({UseBlock{{0, 0, 0}}});
        require(s.at({0, 0, 0}) == note(Instrument::bass), "the note wraps after 24");
        // A head above plays its sound, also from under a block.
        s.tick({place({0, 1, 0}, registry_block("creeper_head", {{"powered", "false"}, {"rotation", "0"}}))});
        require(s.at({0, 0, 0}) == note(Instrument::creeper), "a creeper head above");
        s.tick({UseBlock{{0, 0, 0}}});
        require(draws() == 2, "it plays under the head");
        // Pushed onto other blocks it takes their instrument.
        s.tick({place({6, 0, 2}, blocks::piston(D::east)), place({7, 0, 2}, blocks::note_block()), place({8, -1, 2}, registry_block("clay")),
                place({5, 0, 2}, blocks::lever(false))});
        s.tick({UseBlock{{5, 0, 2}}});
        for (int i = 0; i < 3; ++i) s.tick();
        require(s.at({8, 0, 2}) == note(Instrument::flute), std::format("moved onto clay: {}", circuit::gameplay::block_notation(s.at({8, 0, 2}))));
    });

    suite.run("rails take their shapes from their neighbors and pass power", [] {
        using circuit::contracts::UseBlock;
        using circuit::gameplay::blocks::RailKind;
        using circuit::gameplay::blocks::RailShape;
        const auto rail = [](RailShape r, RailKind k = RailKind::rail, bool on = false) { return blocks::rail(k, r, on); };
        Sim s;
        s.tick(floor(-2, 14, -1, 8));
        // A line laid east: each rail joins the one before.
        s.tick({place({0, 0, 0}, rail(RailShape::north_south))});
        s.tick({place({1, 0, 0}, rail(RailShape::north_south))});
        require(s.at({0, 0, 0}) == rail(RailShape::east_west) && s.at({1, 0, 0}) == rail(RailShape::east_west), "a line");
        // A rail south of the end makes a curve.
        s.tick({place({1, 0, 1}, rail(RailShape::north_south))});
        require(s.at({1, 0, 0}) == rail(RailShape::south_west), "the end curves south");
        // A junction prefers south-east, and north-west while powered.
        s.tick({place({4, 0, 3}, rail(RailShape::east_west)), place({6, 0, 3}, rail(RailShape::east_west)), place({5, 0, 4}, rail(RailShape::north_south)),
                place({5, 0, 2}, rail(RailShape::north_south))});
        s.tick({place({5, 0, 3}, rail(RailShape::north_south))});
        require(s.at({5, 0, 3}) == rail(RailShape::south_east), std::format("junction: {}", circuit::gameplay::block_notation(s.at({5, 0, 3}))));
        // A rail rises toward a rail a block higher.
        s.tick({place({0, 0, 6}, blocks::stone()), place({0, 1, 6}, rail(RailShape::north_south))});
        s.tick({place({1, 0, 6}, rail(RailShape::north_south))});
        require(s.at({1, 0, 6}) == rail(RailShape::ascending_west), "ascending toward the higher rail");
        // Powered rails: a lever at the end of ten powers nine of them.
        std::vector<CommandBody> line;
        for (int x = 3; x <= 12; ++x) line.push_back(place({x, 0, 8}, rail(RailShape::east_west, RailKind::powered)));
        line.push_back(place({2, 0, 8}, blocks::lever(false)));
        s.tick(line);
        s.tick({UseBlock{{2, 0, 8}}});
        require(s.at({11, 0, 8}).flag() && !s.at({12, 0, 8}).flag(), "eight rails beyond the one with the signal");
        // Without a sturdy top below, a rail breaks with its drop.
        s.tick({place({0, -1, 0}, registry_block("oak_slab", {{"type", "bottom"}, {"waterlogged", "false"}}))});
        require(s.at({0, 0, 0}).is_air(), "the rail on a bottom slab broke");
    });

    suite.run("minecarts ride rails: drag, slopes, braking and boosting", [] {
        using circuit::gameplay::blocks::RailKind;
        using circuit::gameplay::blocks::RailShape;
        Sim s;
        std::vector<CommandBody> build = floor(-2, 30, -1, 6);
        for (int x = 0; x <= 28; ++x) build.push_back(place({x, 0, 0}, blocks::rail(RailKind::rail, RailShape::east_west)));
        // Powered rails at x=4..7 powered from below on a second line.
        for (int x = 0; x <= 28; ++x) {
            const bool powered = x >= 4 && x <= 7;
            if (powered) build.push_back(place({x, -1, 4}, blocks::redstone_block()));
            build.push_back(place({x, 0, 4}, blocks::rail(powered ? RailKind::powered : RailKind::rail, RailShape::east_west)));
        }
        s.tick(build);
        s.tick({SummonEntity{"minecraft:minecart", {2.5, 0.0, 0.5}, {0.1, 0.0, 0.0}, 0, 0},
                SummonEntity{"minecraft:minecart", {2.5, 0.0, 4.5}, {0.1, 0.0, 0.0}, 0, 0}});
        const auto cart = [&](double z) {
            const auto d = s.oracle.dump();
            return *std::ranges::find_if(d.overworld().entities, [&](const auto& e) { return e.position.z == z; });
        };
        // On a rail it sits a sixteenth above it, moves by its velocity and keeps 96%.
        const auto first = cart(0.5);
        require(first.position.y == 0.0625 && first.position.x == 2.6 && first.velocity.x == 0.1 * 0.96 && first.velocity.y == 0.0,
                std::format("on the rail: {} {} {}", first.position.x, first.position.y, first.velocity.x));
        for (int i = 0; i < 30; ++i) s.tick();
        // The boosted cart passed the other.
        require(cart(4.5).position.x > cart(0.5).position.x + 1.0 && cart(4.5).velocity.x > cart(0.5).velocity.x,
                std::format("boosted: {} against {}", cart(4.5).position.x, cart(0.5).position.x));
    });

    suite.run("minecarts collide with and push each other by their yaw", [] {
        using circuit::gameplay::blocks::RailKind;
        using circuit::gameplay::blocks::RailShape;
        Sim s;
        std::vector<CommandBody> build = floor(-2, 30, -1, 10);
        for (int x = 0; x <= 28; ++x) build.push_back(place({x, 0, 0}, blocks::rail(RailKind::rail, RailShape::east_west)));
        for (int z = 2; z <= 9; ++z) build.push_back(place({5, 0, z}, blocks::rail(RailKind::rail, RailShape::north_south)));
        s.tick(build);
        // Two carts summoned touching push each other apart at once.
        s.tick({SummonEntity{"minecraft:minecart", {10.5, 0.0, 0.5}, {}, 0, 0}, SummonEntity{"minecraft:minecart", {11.2, 0.0, 0.5}, {}, 0, 0}});
        const auto carts = [&] {
            std::vector<circuit::gameplay::Entity> out;
            const auto d = s.oracle.dump();
            for (const auto& e : d.overworld().entities)
                if (e.kind == circuit::gameplay::EntityKind::minecart) out.push_back(e);
            std::ranges::sort(out, {}, [](const auto& e) { return e.id.value; });
            return out;
        };
        require(carts().size() == 2 && carts()[0].velocity.x < 0.0 && carts()[1].velocity.x > 0.0, "pushed apart");
        // A cart running into one at rest on a north-south line stops against
        // its box; the one at rest, its yaw 0, does not push, the runner (its
        // yaw along the line: -90 running south, 270 once it bounced back and
        // flipped) does.
        s.tick({SummonEntity{"minecraft:minecart", {5.5, 0.0, 2.5}, {0.0, 0.0, 0.8}, 0, 0}, SummonEntity{"minecraft:minecart", {5.5, 0.0, 6.5}, {}, 0, 0}});
        for (int i = 0; i < 8; ++i) s.tick();
        const auto now = carts();
        require(std::fmod(std::abs(now[2].yaw), 180.0f) == 90.0f && now[3].position.z > 6.5,
                std::format("the runner's yaw {}, the other's {} at z={}", now[2].yaw, now[3].yaw, now[3].position.z));
    });

    suite.run("detector rails sense minecarts and look again every 20 ticks", [] {
        using circuit::gameplay::blocks::RailKind;
        using circuit::gameplay::blocks::RailShape;
        Sim s;
        std::vector<CommandBody> build = floor(-2, 20, -1, 4);
        for (int x = 0; x <= 18; ++x)
            build.push_back(place({x, 0, 0}, blocks::rail(x == 10 ? RailKind::detector : RailKind::rail, RailShape::east_west)));
        build.push_back(place({10, 0, 1}, blocks::lamp()));
        build.push_back(place({10, 0, -1}, blocks::comparator(D::north)));
        s.tick(build);
        s.tick({SummonEntity{"minecraft:minecart", {6.5, 0.0, 0.5}, {0.4, 0.0, 0.0}, 0, 0}});
        Tick pressed = 0;
        for (int i = 0; i < 20 && !pressed; ++i) {
            s.tick();
            if (s.at({10, 0, 0}).flag()) pressed = s.oracle.time();
        }
        require(pressed != 0 && s.at({10, 0, 1}).flag(), "the cart pressed it and lit the lamp");
        // The comparator reads 0: the cart has no contents.
        require(!s.at({10, 0, -1}).flag(), "the comparator stays off");
        s.until(pressed + 19);
        require(s.at({10, 0, 0}).flag(), "still powered before its check");
        s.tick();
        require(!s.at({10, 0, 0}).flag(), "the check 20 ticks later finds no cart");
    });

    suite.run("coral fans die without water and leave their water when they break", [] {
        using circuit::gameplay::blocks::CoralKind;
        Sim s;
        s.tick(floor(-2, 8, -1, 4));
        const auto draws = [&] {
            std::uint64_t n = 0;
            for (const auto& r : of_kind(s.last, TraceKind::level_random_draw)) n += r.a;
            return n;
        };
        s.tick({place({0, 0, 0}, blocks::coral_fan(CoralKind::tube)), place({2, 0, 0}, blocks::coral_fan(CoralKind::fire, false, true)),
                place({4, 0, 0}, blocks::coral_fan(CoralKind::horn, true))});
        require(draws() == 1, "only the dry living fan draws its death's delay");
        const auto placed = s.oracle.time();
        while (s.oracle.time() < placed + 100 && s.at({0, 0, 0}) == blocks::coral_fan(CoralKind::tube)) s.tick();
        const auto died = s.oracle.time() - placed;
        require(s.at({0, 0, 0}) == blocks::coral_fan(CoralKind::tube, true) && died >= 60 && died < 100,
                std::format("dead after {} ticks", died));
        require(s.at({2, 0, 0}) == blocks::coral_fan(CoralKind::fire, false, true), "the waterlogged one lives");
        // Without support the waterlogged fan breaks and leaves its water.
        s.tick({place({2, -1, 0}, blocks::air())});
        require(s.at({2, 0, 0}) == blocks::water_source(), "its water stays");
    });

    suite.run("dispensers fire 3 ticks after the tick power reaches them or the block above them", [] {
        using circuit::gameplay::EntityKind;
        Sim s;
        s.tick(floor(-3, 8, -1, 6));
        // A dispenser placed beside a powered block is not triggered until an update reaches it.
        s.tick({place({-2, 0, 4}, blocks::redstone_block()), place({0, 0, 0}, blocks::dispenser(D::east)),
                insert({0, 0, 0}, 0, items::stone, 2)});
        s.tick({place({-1, 0, 4}, blocks::dispenser(D::east))});
        require(!s.at({-1, 0, 4}).flag(), "not triggered when placed beside power");
        s.tick({place({-1, 0, 5}, blocks::stone())});
        require(s.at({-1, 0, 4}).flag(), "triggered by the update");
        // Power at the block: triggered at once, one item 3 ticks later.
        const auto ejected = [&] {
            std::size_t n = 0;
            const auto d = s.oracle.dump();
            for (const auto& e : d.overworld().entities) n += e.kind == EntityKind::item ? e.stack.count : 0;
            return n;
        };
        s.tick({place({-1, 0, 0}, blocks::redstone_block())});
        const auto powered = s.oracle.time();
        require(s.at({0, 0, 0}).flag(), "triggered");
        while (ejected() == 0 && s.oracle.time() < powered + 10) s.tick();
        require(s.oracle.time() == powered + 3, std::format("fired after {} ticks", s.oracle.time() - powered));
        const auto spawned = s.last;
        bool found = false;
        for (const auto& r : of_kind(spawned, TraceKind::entity_spawn_position))
            found = found || (std::bit_cast<double>(r.a) == 1.2 && std::bit_cast<double>(r.b) == 0.5 - 0.15625 && std::bit_cast<double>(r.c) == 0.5);
        require(found, "ejected from 0.7 out of the center, 0.15625 lower");
        for (const auto& r : of_kind(spawned, TraceKind::entity_spawn_velocity)) {
            const auto vx = std::bit_cast<double>(r.a), vy = std::bit_cast<double>(r.b);
            require(vx >= 0.2 - 0.103365 && vx <= 0.3 + 0.103365 && vy >= 0.2 - 0.103365 && vy <= 0.2 + 0.103365, "ejection speed");
        }
        // Staying powered, it does not fire again.
        for (int i = 0; i < 8; ++i) s.tick();
        require(ejected() == 1, "one item while powered");
        // Power two above reaches it only through the block above it, when an update comes.
        s.tick({place({-1, 0, 0}, blocks::air())});
        require(!s.at({0, 0, 0}).flag(), "no longer triggered");
        s.tick({place({0, 2, 0}, blocks::redstone_block())});
        require(!s.at({0, 0, 0}).flag(), "no update reached it");
        s.tick({place({0, 0, 1}, blocks::stone())});
        require(s.at({0, 0, 0}).flag(), "the block above it is powered");
    });

    suite.run("droppers fill the container in front; dispensers pour and take water and set minecarts", [] {
        using circuit::gameplay::EntityKind;
        using circuit::gameplay::blocks::RailKind;
        using circuit::gameplay::blocks::RailShape;
        Sim s;
        std::vector<CommandBody> build = floor(-3, 12, -1, 14);
        // z=0: a dropper into an empty hopper; z=2: into a full chest.
        build.push_back(place({0, 0, 0}, blocks::dispenser(D::east, true)));
        build.push_back(place({1, 0, 0}, blocks::hopper(D::down)));
        build.push_back(place({0, 0, 2}, blocks::dispenser(D::east, true)));
        build.push_back(place({1, 0, 2}, blocks::chest()));
        // z=4: a water bucket into air; z=6: into dust; z=8: at a stone pressure plate.
        for (const int z : {4, 6, 8}) build.push_back(place({0, 0, z}, blocks::dispenser(D::east)));
        build.push_back(place({1, 0, 6}, blocks::wire()));
        build.push_back(place({1, 0, 8}, blocks::pressure_plate(blocks::ButtonKind::stone)));
        // z=10: two buckets at a source; z=12: a minecart at a rail; z=14: TNT.
        build.push_back(place({0, 0, 10}, blocks::dispenser(D::east)));
        build.push_back(place({1, 0, 10}, blocks::water_source()));
        build.push_back(place({2, 0, 10}, blocks::stone()));
        build.push_back(place({1, 0, 9}, blocks::stone()));
        build.push_back(place({1, 0, 11}, blocks::stone()));
        build.push_back(place({0, 0, 12}, blocks::dispenser(D::east)));
        build.push_back(place({1, 0, 12}, blocks::rail(RailKind::rail, RailShape::east_west)));
        build.push_back(place({0, 0, 14}, blocks::dispenser(D::east)));
        s.tick(build);
        std::vector<CommandBody> fill{insert({0, 0, 0}, 0, items::stone, 3), insert({0, 0, 2}, 0, items::dirt, 1),
                                      insert({0, 0, 4}, 0, items::water_bucket, 1), insert({0, 0, 6}, 0, items::water_bucket, 1),
                                      insert({0, 0, 8}, 0, items::water_bucket, 1), insert({0, 0, 10}, 0, items::bucket, 2),
                                      insert({0, 0, 12}, 0, items::minecart, 1), insert({0, 0, 14}, 0, items::tnt, 1)};
        for (std::uint8_t i = 0; i < 27; ++i) fill.push_back(insert({1, 0, 2}, i, items::stone, 64));
        s.tick(fill);
        std::vector<CommandBody> fire;
        for (int z = 0; z <= 14; z += 2) fire.push_back(place({-1, 0, z}, blocks::redstone_block()));
        s.tick(fire);
        for (int i = 0; i < 3; ++i) s.tick();
        const auto d = s.oracle.dump();
        const auto slots = [&](BlockPos p) {
            for (const auto& [pos, be] : d.overworld().block_entities)
                if (pos == p) return be.slots;
            return std::vector<ItemStack>{};
        };
        const auto hopper = slots({1, 0, 0});
        require(hopper[0] == ItemStack{items::stone, 1} && slots({0, 0, 0})[0] == ItemStack{items::stone, 2}, "one item into the hopper");
        for (const auto& [pos, be] : d.overworld().block_entities)
            // 8, less one for its own tick, which follows block ticks.
            if (pos == BlockPos{1, 0, 0}) require(be.cooldown == 7, std::format("the hopper waits 8 ticks, at {}", be.cooldown));
        require(slots({0, 0, 2})[0] == ItemStack{items::dirt, 1}, "a full chest leaves the dropper's item");
        require(state_at(d, {1, 0, 4}) == blocks::water_source() && slots({0, 0, 4})[0] == ItemStack{items::bucket, 1}, "poured into air");
        require(state_at(d, {1, 0, 6}) == blocks::water_source(), "the dust washed away");
        require(slots({0, 0, 8})[0].empty() && state_at(d, {1, 0, 8}).is(BlockType::pressure_plate), "the plate refuses it: ejected");
        require(state_at(d, {1, 0, 10}).is_air() && slots({0, 0, 10})[0] == ItemStack{items::bucket, 1}
                    && slots({0, 0, 10})[1] == ItemStack{items::water_bucket, 1},
                "the filled bucket goes to the next free slot");
        std::size_t carts = 0, tnt = 0, buckets = 0, redstone = 0;
        for (const auto& e : d.overworld().entities) {
            if (e.kind == EntityKind::minecart) {
                ++carts;
                require(e.position.x == 1.625 && e.position.z == 12.5, "the minecart is set 1.125 out on the rail");
            }
            if (e.kind == EntityKind::primed_tnt) tnt += std::abs(e.position.x - 1.5) < 0.1 && std::abs(e.position.z - 14.5) < 0.1;
            if (e.kind == EntityKind::item && e.stack.item == items::water_bucket) ++buckets;
            if (e.kind == EntityKind::item && e.stack.item == items::redstone) ++redstone;
        }
        require(carts == 1 && tnt == 1 && buckets == 1 && redstone == 1, std::format("{} carts, {} TNT, {} buckets, {} dust", carts, tnt, buckets, redstone));
    });

    suite.run("dispensers eject equipment and keep items whose use is not modeled", [] {
        using circuit::gameplay::EntityKind;
        Sim s;
        std::vector<CommandBody> build = floor(-3, 6, -1, 8);
        const std::array<std::pair<std::uint16_t, bool>, 5> lanes{{{items::iron_helmet, true}, {items::shears, false}, {items::saddle, true},
                                                                   {items::arrow, false}, {items::glass_bottle, true}}};
        for (std::size_t i = 0; i < lanes.size(); ++i) build.push_back(place({0, 0, static_cast<int>(2 * i)}, blocks::dispenser(D::east)));
        s.tick(build);
        std::vector<CommandBody> fill, fire;
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            fill.push_back(insert({0, 0, static_cast<int>(2 * i)}, 0, lanes[i].first, 1));
            fire.push_back(place({-1, 0, static_cast<int>(2 * i)}, blocks::redstone_block()));
        }
        s.tick(fill);
        s.tick(fire);
        for (int i = 0; i < 3; ++i) s.tick();
        const auto d = s.oracle.dump();
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            bool ejected = false;
            for (const auto& e : d.overworld().entities) ejected = ejected || (e.kind == EntityKind::item && e.stack.item == lanes[i].first);
            require(ejected == lanes[i].second, std::format("item {} ejected: {}", lanes[i].first, ejected));
        }
        Sim t;
        t.tick(with_floor(floor(-3, 3, -1, 1), {SetGameRule{"tntExplodes", 0}, place({0, 0, 0}, blocks::dispenser(D::east))}));
        t.tick({insert({0, 0, 0}, 0, items::tnt, 1)});
        t.tick({place({-1, 0, 0}, blocks::redstone_block())});
        for (int i = 0; i < 4; ++i) t.tick();
        const auto td = t.oracle.dump();
        require(td.overworld().entities.empty(), "with tntExplodes off the TNT stays");
    });

    suite.run("comparators read dispensers as containers of nine slots", [] {
        Sim s;
        s.tick(with_floor(floor(-3, 6, -1, 2), {place({0, 0, 0}, blocks::dispenser(D::up)), place({1, 0, 0}, blocks::comparator(D::east)),
                                                 place({2, 0, 0}, blocks::wire())}));
        s.tick({insert({0, 0, 0}, 0, items::minecart, 1), insert({0, 0, 0}, 1, items::minecart, 1), insert({0, 0, 0}, 2, items::minecart, 1)});
        for (int i = 0; i < 4; ++i) s.tick();
        // Three of nine slots full: 1 + floor(14 * 3 / 9) = 5.
        require(wire_level(s, {2, 0, 0}) == 5, std::format("dust at {}", wire_level(s, {2, 0, 0})));
    });

    suite.run("redstone conductors: full cubes with named exceptions, and soul sand", [] {
        // A repeater powers the block in front strongly; a lamp beyond lights
        // when that block conducts.
        const std::array<std::pair<BlockState, bool>, 8> lanes{{
            {blocks::slime(), true}, {registry_block("oak_slab", {{"type", "double"}}), true}, {registry_block("soul_sand"), true},
            {blocks::shulker_box(D::up), true}, {registry_block("oak_slab", {{"type", "bottom"}}), false},
            {registry_block("glowstone"), false}, {blocks::glass(), false}, {registry_block("sea_lantern"), false}}};
        Sim s;
        std::vector<CommandBody> build = floor(-2, 6, -1, 2 * static_cast<int>(lanes.size()));
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            const int z = 2 * static_cast<int>(i);
            build.push_back(place({1, 0, z}, blocks::repeater(D::east)));
            build.push_back(place({2, 0, z}, lanes[i].first));
            build.push_back(place({3, 0, z}, blocks::lamp()));
        }
        s.tick(build);
        std::vector<CommandBody> power;
        for (std::size_t i = 0; i < lanes.size(); ++i) power.push_back(place({0, 0, 2 * static_cast<int>(i)}, blocks::redstone_block()));
        s.tick(power);
        for (int i = 0; i < 6; ++i) s.tick();
        for (std::size_t i = 0; i < lanes.size(); ++i)
            require(s.at({3, 0, 2 * static_cast<int>(i)}).flag() == lanes[i].second,
                    std::format("{}: the lamp {}", circuit::gameplay::block_notation(lanes[i].first), lanes[i].second ? "lights" : "stays off"));
    });

    suite.run("entities landing on slime blocks keep less horizontal speed, minecarts twice", [] {
        using circuit::gameplay::EntityKind;
        Sim s;
        std::vector<CommandBody> build;
        for (int x = -2; x <= 8; ++x)
            for (int z = -1; z <= 4; ++z) build.push_back(place({x, -1, z}, blocks::slime()));
        s.tick(build);
        // Their first tick: falling 0.04, each lands and bounces back up at
        // 0.032; with less than 0.1 left it keeps 0.4 + 0.032 * 0.2 of its
        // horizontal speed (an item then loses 0.8 * 0.98 to friction on slime).
        s.tick({SpawnItem{{0.5, 0.0, 0.5}, {0.3, 0.0, 0.0}, ItemStack{items::stone, 1}, 0, 0},
                SummonEntity{"minecraft:minecart", {0.5, 0.0, 3.5}, {0.4, 0.0, 0.0}, 0, 0}});
        const double keep = 0.4 + 0.04 * 0.8 * 0.2;
        const auto d = s.oracle.dump();
        std::size_t seen = 0;
        for (const auto& e : d.overworld().entities) {
            if (e.kind == EntityKind::item) {
                require(e.velocity.x == 0.3 * keep * static_cast<double>(0.8f * 0.98f), std::format("the item keeps {}", e.velocity.x));
                ++seen;
            }
            if (e.kind == EntityKind::minecart) {
                require(e.velocity.x == 0.4 * keep * keep, std::format("the minecart keeps {}", e.velocity.x));
                ++seen;
            }
        }
        require(seen == 2, "both landed");
    });

    suite.run("blocks a piston sets down where they cannot stay break at once", [] {
        using circuit::gameplay::EntityKind;
        using circuit::gameplay::blocks::CoralKind;
        using circuit::gameplay::blocks::RailKind;
        using circuit::gameplay::blocks::RailShape;
        Sim s;
        // A rail pushed off its block over air, one pushed onto stone, and a
        // dead coral fan pushed away from its wall.
        s.tick(with_floor(floor(-2, 8, -1, 6),
                          {place({0, 1, 0}, blocks::piston(D::east)), place({1, 0, 0}, blocks::stone()), place({1, 1, 0}, blocks::rail(RailKind::rail, RailShape::east_west)),
                           place({0, 1, 2}, blocks::piston(D::east)), place({1, 0, 2}, blocks::stone()), place({2, 0, 2}, blocks::stone()),
                           place({1, 1, 2}, blocks::rail(RailKind::rail, RailShape::east_west)), place({0, 0, 4}, blocks::piston(D::east)),
                           place({1, 0, 3}, blocks::stone()), place({1, 0, 4}, blocks::coral_wall_fan(CoralKind::tube, D::south, true))}));
        s.tick({place({-1, 1, 0}, blocks::redstone_block()), place({-1, 1, 2}, blocks::redstone_block()), place({-1, 0, 4}, blocks::redstone_block())});
        for (int i = 0; i < 4; ++i) s.tick();
        require(s.at({2, 1, 0}).is_air(), "the rail over air broke");
        require(s.at({2, 1, 2}).is(BlockType::rail), "the rail on stone stays");
        require(s.at({2, 0, 4}).is_air(), "the fan away from its wall broke");
        std::size_t rails = 0;
        const auto d = s.oracle.dump();
        for (const auto& e : d.overworld().entities) rails += e.kind == EntityKind::item && e.stack.item == items::rail;
        require(rails == 1, std::format("the broken rail dropped itself ({} rail items)", rails));
    });

    suite.run("fences, panes and walls join what is set beside and above them, and where a piston sets them down", [] {
        namespace registry = circuit::gameplay::registry;
        using circuit::gameplay::blocks::DoorKind;
        const auto named = [](std::string_view name, std::vector<registry::Property> props = {}) {
            return BlockState::registered(registry::state_of(registry::find_block("1.21.8", name).value(), props).value());
        };
        const auto wall = named("cobblestone_wall"), fence = named("oak_fence"), pane = named("glass_pane");
        Sim s;
        s.tick(with_floor(floor(-2, 12, -1, 8), {place({2, 1, 0}, wall), place({6, 1, 0}, fence), place({10, 1, 0}, pane),
                                                   place({0, 1, 4}, blocks::piston(D::east)), place({1, 1, 4}, fence)}));
        // Neighbors set after them: a wall between two stones, a fence beside a
        // nether brick fence and a fence gate turned across, a pane beside a
        // melon and soul sand.
        s.tick({place({1, 1, 0}, blocks::stone()), place({3, 1, 0}, blocks::stone()), place({5, 1, 0}, named("nether_brick_fence")),
                place({7, 1, 0}, blocks::fence_gate(DoorKind::oak, D::north)), place({9, 1, 0}, named("melon")), place({11, 1, 0}, named("soul_sand")),
                place({2, 1, 5}, blocks::stone()), place({2, 1, 3}, named("nether_brick_fence"))});
        require(s.at({2, 1, 0}) == named("cobblestone_wall", {{"east", "low"}, {"west", "low"}, {"up", "false"}}),
                std::format("a straight wall without a post: {}", circuit::gameplay::block_notation(s.at({2, 1, 0}))));
        require(s.at({6, 1, 0}) == named("oak_fence", {{"east", "true"}}), "the fence joins the gate, not the nether brick fence");
        require(s.at({10, 1, 0}) == named("glass_pane", {{"east", "true"}}), "the pane joins soul sand, not the melon");
        // A slab on the wall raises both sides; a torch gives it its post back.
        s.tick({place({2, 2, 0}, named("stone_slab", {{"type", "bottom"}}))});
        require(s.at({2, 1, 0}) == named("cobblestone_wall", {{"east", "tall"}, {"west", "tall"}, {"up", "false"}}), "tall under a slab");
        s.tick({place({2, 2, 0}, named("torch"))});
        require(s.at({2, 1, 0}) == named("cobblestone_wall", {{"east", "low"}, {"west", "low"}, {"up", "true"}}), "a post under a torch");
        // The pushed fence takes its sides from where it arrives: stone to the
        // south, the piston head behind it; not the nether brick fence.
        s.tick({place({-1, 1, 4}, blocks::redstone_block())});
        for (int i = 0; i < 4; ++i) s.tick();
        require(s.at({2, 1, 4}) == named("oak_fence", {{"south", "true"}, {"west", "true"}}),
                std::format("the pushed fence: {}", circuit::gameplay::block_notation(s.at({2, 1, 4}))));
    });

    suite.run("stairs take corner shapes from the stairs beside them", [] {
        namespace registry = circuit::gameplay::registry;
        const auto stairs = [](std::string_view facing, std::string_view shape = "straight", std::string_view half = "bottom") {
            const std::vector<registry::Property> props{{"facing", std::string(facing)}, {"half", std::string(half)}, {"shape", std::string(shape)}};
            return BlockState::registered(registry::state_of(registry::find_block("1.21.8", "oak_stairs").value(), props).value());
        };
        Sim s;
        s.tick(with_floor(floor(-2, 12, -1, 8), {place({2, 1, 2}, stairs("north")), place({6, 1, 2}, stairs("north")), place({10, 1, 2}, stairs("north")),
                                                   place({0, 1, 6}, blocks::piston(D::east)), place({1, 1, 6}, stairs("north"))}));
        // A stair turned west in front makes an outer corner, one turned east
        // behind an inner corner; an upside-down one changes nothing.
        s.tick({place({2, 1, 1}, stairs("west")), place({6, 1, 3}, stairs("east")), place({10, 1, 1}, stairs("west", "straight", "top")),
                place({2, 1, 5}, stairs("west"))});
        require(s.at({2, 1, 2}) == stairs("north", "outer_left"), std::format("outer: {}", circuit::gameplay::block_notation(s.at({2, 1, 2}))));
        require(s.at({6, 1, 2}) == stairs("north", "inner_right"), std::format("inner: {}", circuit::gameplay::block_notation(s.at({6, 1, 2}))));
        require(s.at({10, 1, 2}) == stairs("north"), "halves do not join");
        // Removed, the corner straightens; pushed into a corner, a stair takes it.
        s.tick({place({2, 1, 1}, BlockState::of(BlockType::air)), place({-1, 1, 6}, blocks::redstone_block())});
        require(s.at({2, 1, 2}) == stairs("north"), "straight again");
        for (int i = 0; i < 4; ++i) s.tick();
        require(s.at({2, 1, 6}) == stairs("north", "outer_left"), std::format("pushed: {}", circuit::gameplay::block_notation(s.at({2, 1, 6}))));
    });

    suite.run("torches, ladders, carpets and flowers break with their drops when their support goes", [] {
        namespace registry = circuit::gameplay::registry;
        using circuit::gameplay::EntityKind;
        const auto named = [](std::string_view name, std::vector<registry::Property> props = {}) {
            return BlockState::registered(registry::state_of(registry::find_block("1.21.8", name).value(), props).value());
        };
        Sim s;
        s.tick(with_floor(floor(-2, 8, -1, 4), {place({0, 1, 0}, blocks::stone()), place({0, 2, 0}, named("torch")),
                                                 place({2, 1, 0}, blocks::stone()), place({2, 1, 1}, named("ladder", {{"facing", "south"}})),
                                                 place({4, 1, 0}, named("dirt")), place({4, 2, 0}, named("poppy")),
                                                 place({6, 1, 0}, blocks::stone()), place({6, 2, 0}, named("white_carpet"))}));
        // A block beside the torch leaves it; one beside the poppy too.
        s.tick({place({1, 2, 0}, blocks::stone()), place({5, 2, 0}, blocks::stone())});
        require(s.at({0, 2, 0}) == named("torch") && s.at({4, 2, 0}) == named("poppy"), "supported, they stay");
        s.tick({place({0, 1, 0}, blocks::air()), place({2, 1, 0}, blocks::air()), place({4, 1, 0}, blocks::stone()), place({6, 1, 0}, blocks::air())});
        for (const BlockPos p : {BlockPos{0, 2, 0}, BlockPos{2, 1, 1}, BlockPos{4, 2, 0}, BlockPos{6, 2, 0}})
            require(s.at(p).is_air(), std::format("{} {} {} broke", p.x, p.y, p.z));
        std::multiset<std::uint16_t> drops;
        const auto dump = s.oracle.dump();
        for (const auto& e : dump.overworld().entities)
            if (e.kind == EntityKind::item) drops.insert(e.stack.item);
        require(drops == std::multiset<std::uint16_t>{items::torch, items::ladder, items::poppy, items::white_carpet}, "each dropped itself");
    });

    suite.run("a block entity block placed from an item with components places nothing", [] {
        // A named beacon would lose its name: nothing is placed, and the item
        // stays; an unnamed one is placed with an empty block entity. (A
        // shulker box, with rules of its own, keeps its name and contents.)
        const circuit::contracts::PlayerKey alex{1, 2};
        const auto beacon = items::by_name("minecraft:beacon").value();
        Sim s;
        s.tick(with_floor(floor(-1, 4, -1, 1), {}));
        s.tick({circuit::contracts::SetPlayerSlot{alex, 36, named(beacon, 1, "Box")},
                circuit::contracts::PlayerUse{alex, 36, {0, -1, 0}, D::up, D::north, D::north, false, true, 3, 3},
                circuit::contracts::SetPlayerSlot{alex, 37, {beacon, 1, 0}},
                circuit::contracts::PlayerUse{alex, 37, {2, -1, 0}, D::up, D::north, D::north, false, true, 3, 3}});
        require(s.at({0, 0, 0}).is_air(), "the named beacon is not placed");
        const auto d = s.oracle.dump();
        const auto inventory = std::ranges::find(d.players, alex, &PlayerInventory::player);
        require(inventory != d.players.end() && inventory->slots[36] == named(beacon, 1, "Box") && inventory->slots[37].empty(),
                "the named beacon stays in the hand, the other was used");
        const auto* be = block_entity_at(d, {2, 0, 0});
        require(s.at({2, 0, 0}).is(BlockType::other) && be && be->kind == BlockType::other, "the unnamed beacon is placed with its block entity");
    });

    suite.run("buckets take and pour fluids along the player's view as both official servers do", [] {
        // circuit-clienttest/buckets.js: each use looks down from the south at
        // its cell, clicking the block its view meets.
        using circuit::contracts::BlockHit;
        using circuit::contracts::PlayerUseItem;
        using circuit::contracts::SetPlayerSlot;
        const circuit::contracts::PlayerKey alex{1, 2}, sam{3, 4};
        const auto look = [](circuit::contracts::Vec3 eye, circuit::contracts::Vec3 at) {
            const double dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
            const double yaw = std::atan2(-dx, dz) * 180.0 / std::numbers::pi;
            const double pitch = -std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0 / std::numbers::pi;
            return std::pair{static_cast<float>(yaw), static_cast<float>(pitch)};
        };
        // A use from above the stone south of the cell at `x`, toward `at`.
        const auto use = [&](circuit::contracts::PlayerKey who, std::int32_t x, circuit::contracts::Vec3 at, std::optional<BlockHit> hit,
                             bool survival = false) {
            const circuit::contracts::Vec3 eye{x + 0.5, 2.62, 1.5};
            const auto [yaw, pitch] = look(eye, at);
            return PlayerUseItem{who, 36, eye, yaw, pitch, survival ? 4.5 : 5.0, survival, hit};
        };
        const auto floor_hit = [](std::int32_t x) { return BlockHit{{x, -1, 0}, D::up, 0.5f, 1.0f, 0.3f}; };
        const auto lava = registry_block("lava", {{"level", "0"}});
        const auto slab = [](std::string_view type, bool wet) {
            return registry_block("oak_slab", {{"type", std::string(type)}, {"waterlogged", wet ? "true" : "false"}});
        };
        Sim s;
        auto build = floor(-1, 40, -1, 1);
        build.push_back(place({0, 0, 0}, blocks::water_source()));
        build.push_back(place({3, 0, 0}, blocks::water_source()));
        build.push_back(place({6, 0, 0}, blocks::water_flowing(2)));
        build.push_back(place({9, 0, 0}, lava));
        build.push_back(place({12, 0, 0}, slab("bottom", true)));
        build.push_back(place({15, 0, 0}, slab("bottom", false)));
        build.push_back(place({18, 0, 0}, slab("double", false)));
        build.push_back(place({21, 0, 0}, slab("bottom", false)));
        build.push_back(place({24, 0, 0}, lava));
        build.push_back(place({30, 0, 0}, blocks::water_source()));
        s.tick(with_floor(build, {}));
        const auto held = [&](circuit::contracts::PlayerKey who, std::uint16_t item, std::uint8_t count = 1) {
            return SetPlayerSlot{who, 36, {item, count, 0}};
        };
        s.tick({held(alex, items::bucket), use(alex, 0, {0.5, 0.4, 0.3}, floor_hit(0)), use(alex, 3, {3.5, 0.4, 0.3}, floor_hit(3)),
                use(alex, 6, {6.5, 0.1, 0.3}, floor_hit(6)), use(alex, 9, {9.5, 0.4, 0.3}, floor_hit(9)),
                use(alex, 12, {12.5, 0.5, 0.3}, BlockHit{{12, 0, 0}, D::up, 0.5f, 0.5f, 0.3f}),
                held(alex, items::water_bucket), use(alex, 15, {15.5, 0.5, 0.3}, BlockHit{{15, 0, 0}, D::up, 0.5f, 0.5f, 0.3f}),
                use(alex, 18, {18.5, 1.0, 0.3}, BlockHit{{18, 0, 0}, D::up, 0.5f, 1.0f, 0.3f}),
                held(alex, items::lava_bucket), use(alex, 21, {21.5, 0.5, 0.3}, BlockHit{{21, 0, 0}, D::up, 0.5f, 0.5f, 0.3f}),
                held(alex, items::water_bucket), use(alex, 24, {24.5, 0.0, 0.3}, floor_hit(24)),
                use(alex, 27, {27.5, 0.0, 0.3}, std::nullopt),
                held(sam, items::bucket, 2), use(sam, 30, {30.5, 0.4, 0.3}, floor_hit(30), true)});
        std::vector<std::uint64_t> outcomes;
        for (const auto& r : s.last) {
            if (r.kind == TraceKind::command_applied) outcomes.push_back(0);
            if (r.kind == TraceKind::command_rejected) outcomes.push_back(r.b);
        }
        const auto no = static_cast<std::uint64_t>(ErrorCode::not_usable);
        require(outcomes == std::vector<std::uint64_t>{0, 0, 0, no, 0, 0, 0, 0, 0, 0, 0, 0, 0, no, 0, 0}, "the uses that act");
        require(s.at({0, 0, 0}).is_air() && s.at({3, 0, 0}).is_air(), "the sources are taken");
        require(s.at({6, 0, 0}) == blocks::water_flowing(2), "flowing water is not");
        require(s.at({9, 0, 0}).is_air(), "the lava source is taken");
        require(s.at({12, 0, 0}) == slab("bottom", false), "the slab's water is taken");
        require(s.at({15, 0, 0}) == slab("bottom", true), "water poured into a slab");
        require(s.at({18, 0, 0}) == slab("double", false) && s.at({18, 1, 0}) == blocks::water_source(), "water above a double slab");
        require(s.at({21, 0, 0}) == slab("bottom", false) && s.at({21, 1, 0}) == lava, "lava above a slab");
        require(s.at({24, 0, 0}) == blocks::water_source(), "water replaces lava");
        require(s.at({30, 0, 0}).is_air(), "the survival player takes the source");
        const auto d = s.oracle.dump();
        const auto slots = [&](circuit::contracts::PlayerKey who) { return std::ranges::find(d.players, who, &PlayerInventory::player)->slots; };
        // The creative player got one water bucket and one lava bucket, beside the buckets used.
        const auto a = slots(alex);
        require(std::ranges::count(a, ItemStack{items::water_bucket, 1, 0}) == 2 && std::ranges::count(a, ItemStack{items::lava_bucket, 1, 0}) == 1,
                "the creative player's buckets");
        const auto b = slots(sam);
        require(b[36] == ItemStack{items::bucket, 1, 0} && b[37] == ItemStack{items::water_bucket, 1, 0}, "one of two buckets filled");
    });

    suite.run("random sequences roll as both official servers' loot tables do", [] {
        // The loot_many skeleton: 96 short grasses, 64 dead bushes and 32 ripe
        // sweet berry bushes broken in order on both versions (world seed 0).
        // Their rolls give what they dropped, and the states they leave are the
        // ones the official servers saved (data/random_sequences.dat).
        using circuit::gameplay::RandomSequence;
        const auto create = [](std::uint64_t a, std::uint64_t b) { return RandomSequence::create(0, {a, b}, {}); };
        auto grass = create(0x8bbf3c7183e5ac1eULL, 0x0e588cdc9bfba209ULL);
        std::string seeds;
        for (int i = 0; i < 96; ++i) {
            const bool drop = grass.next_float() < 0.125f;
            if (drop) (void)grass.next_int(1); // fortune's bonus, at level 0
            seeds += drop ? '1' : '0';
        }
        require(seeds == "000100010010001000100011010000000000000000000000110000000000000100000000000000100100000010000000", "grass seeds");
        require(std::bit_cast<std::int64_t>(grass.low()) == 2022789406501655404 && std::bit_cast<std::int64_t>(grass.high()) == -468754072450410102,
                "the grass sequence's saved state");
        auto bush = create(0xa00e2b904f10208bULL, 0x61ced0c97606ee23ULL);
        std::string sticks;
        for (int i = 0; i < 64; ++i) sticks += static_cast<char>('0' + bush.next_int(3));
        require(sticks == "0100020100111200120021010012212211022002122102021210121122201200", "dead bush sticks");
        require(std::bit_cast<std::int64_t>(bush.low()) == 980453112882939729 && std::bit_cast<std::int64_t>(bush.high()) == 5313872344700270703,
                "the dead bush sequence's saved state");
        auto berry = create(0x43d91b9af92bc74bULL, 0x41420935cf122c3bULL);
        std::string berries;
        for (int i = 0; i < 32; ++i) berries += static_cast<char>('0' + berry.next_int(2) + 2 + berry.next_int(1));
        require(berries == "22322222232333333322323332333322", "berries");
        require(std::bit_cast<std::int64_t>(berry.low()) == 4585398311947135046 && std::bit_cast<std::int64_t>(berry.high()) == -4978303496063927502,
                "the berry sequence's saved state");
    });

    suite.run("fortune and table bonuses roll as both official servers' loot tables do", [] {
        // The loot_tools skeleton (world seed 0): iron ores broken with fortune
        // 3, none, 1 and 2, and gravel by hand and with fortune 1 and 3, on
        // both versions. Fortune multiplies raw iron by 1 + max(0, an int below
        // level + 2, less 1), drawn only above level 0; a table bonus is a
        // float below the chance of the tool's level.
        using circuit::gameplay::RandomSequence;
        auto iron = RandomSequence::create(0, {0xee7ca3e4d6f665d3ULL, 0x5ca39fd2ffe3da36ULL}, {});
        const auto ores = [&](std::int32_t level, int n) {
            std::string out;
            for (int i = 0; i < n; ++i) out += static_cast<char>('0' + (level > 0 ? std::max(0, iron.next_int(level + 2) - 1) + 1 : 1));
            return out;
        };
        require(ores(3, 16) == "3311142134124344" && ores(0, 8) == "11111111" && ores(1, 16) == "2212211112122111" && ores(2, 16) == "1331313123121213",
                "raw iron");
        auto gravel = RandomSequence::create(0, {0x2fedfb509401412fULL, 0x6b4882392a3638a0ULL}, {});
        const auto flint = [&](float chance, int n) {
            std::string out;
            for (int i = 0; i < n; ++i) out += gravel.next_float() < chance ? 'f' : 'g';
            return out;
        };
        require(flint(0.1f, 16) == "gggggggggggggggg" && flint(0.14285715f, 16) == "fggggfggggggfggf" && flint(1.0f, 8) == "ffffffff", "flint");
    });

    suite.run("players place blocks into lava but cannot break it", [] {
        // As water: a block placed into a lava source replaces it; lava
        // cannot be mined.
        const circuit::contracts::PlayerKey alex{1, 2};
        const auto lava = registry_block("lava", {{"level", "0"}});
        Sim s;
        auto build = floor(-1, 4, -1, 1);
        build.push_back(place({0, 0, 0}, lava));
        build.push_back(place({2, 0, 0}, lava));
        s.tick(with_floor(build, {}));
        s.tick({circuit::contracts::SetPlayerSlot{alex, 36, {items::stone, 1, 0}},
                circuit::contracts::PlayerUse{alex, 36, {0, -1, 0}, D::up, D::north, D::north, false, true, 3, 3},
                circuit::contracts::PlayerBreak{alex, {2, 0, 0}, true}});
        require(s.at({0, 0, 0}) == blocks::stone(), "the stone replaced the lava");
        require(s.at({2, 0, 0}) == lava, "the other source stays");
    });

    suite.run("pistons move, destroy and leave blocks without rules by their push class", [] {
        using circuit::contracts::UseBlock;
        Sim s;
        std::vector<CommandBody> build = floor(-2, 6, -1, 12);
        const std::array<BlockState, 6> lanes{registry_block("cobblestone"), registry_block("white_glazed_terracotta", {{"facing", "north"}}),
                                              registry_block("dandelion"), registry_block("bedrock"), registry_block("dirt"),
                                              registry_block("enchanting_table")};
        for (int i = 0; i < static_cast<int>(lanes.size()); ++i) {
            build.push_back(place({0, 0, 2 * i}, blocks::piston(D::east, true)));
            build.push_back(place({1, 0, 2 * i}, lanes[static_cast<std::size_t>(i)]));
            build.push_back(place({-1, 0, 2 * i}, blocks::lever(false)));
        }
        s.tick(build);
        std::vector<CommandBody> on;
        for (int i = 0; i < static_cast<int>(lanes.size()); ++i) on.push_back(UseBlock{{-1, 0, 2 * i}});
        s.tick(on);
        for (int i = 0; i < 4; ++i) s.tick();
        require(s.at({2, 0, 0}) == lanes[0] && s.at({2, 0, 2}) == lanes[1] && s.at({2, 0, 8}) == lanes[4], "cobblestone, terracotta and dirt pushed");
        require(s.at({1, 0, 4}).is(BlockType::piston_head) && s.at({2, 0, 4}).is_air(), "the dandelion destroyed");
        require(s.at({1, 0, 6}) == lanes[3] && s.at({1, 0, 10}) == lanes[5], "bedrock and the enchanting table stay");
        s.tick(on);
        for (int i = 0; i < 4; ++i) s.tick();
        require(s.at({1, 0, 0}) == lanes[0] && s.at({1, 0, 8}) == lanes[4], "cobblestone and dirt pulled back");
        require(s.at({2, 0, 2}) == lanes[1] && s.at({1, 0, 2}).is_air(), "glazed terracotta is never pulled");
    });

    suite.run("entities leave with their chunk and come back under new ids", [] {
        Sim s(bundle(), 7, 0); // tick 1: chunk (0,0) entity ticking
        s.tick({place({4, -1, 4}, blocks::stone()), SpawnItem{at_block(4.5, 0.0, 4.5), {}, {items::stone, 3}}}); // 2
        s.until(5);
        const auto before = s.oracle.dump().overworld().entities.at(0);
        require(before.age == 4 && before.tick_count == 4, "ticking");
        s.tick({RemoveTicket{1}}); // 6: the chunk unloads, its item leaves with it
        const auto removed = of_kind(s.last, TraceKind::entity_removed);
        require(removed.size() == 1 && removed[0].a == before.id.value
                    && removed[0].b == static_cast<std::uint64_t>(circuit::gameplay::RemovalReason::unloaded),
                "removed as unloaded");
        require(s.oracle.dump().overworld().entities.size() == 1, "kept with the chunk");
        s.until(9);
        s.tick({ticket(2, {0, 0}, 0)}); // 10: reload
        auto d = s.oracle.dump();
        require(of_kind(s.last, TraceKind::entity_loaded).empty() && d.overworld().entities.at(0).returns == 11, "back in the next tick");
        s.tick(); // 11
        const auto loaded = of_kind(s.last, TraceKind::entity_loaded);
        d = s.oracle.dump();
        const auto& back = d.overworld().entities.at(0);
        require(loaded.size() == 1 && loaded[0].b == before.id.value && loaded[0].a == back.id.value && back.id.value > before.id.value
                    && back.returns == 0 && back.tick_count == 0 && back.age == 4 && back.stack == before.stack,
                "under a new id, with its tick count starting again");
        s.tick(); // 12: the chunk's entities tick again two ticks after the reload
        d = s.oracle.dump();
        require(d.overworld().entities.at(0).tick_count == 1 && d.overworld().entities.at(0).age == 5, "ticking again");
    });

    suite.run("entities migrate once per tick and fall out of the world", [] {
        Sim s;
        std::vector<CommandBody> build;
        for (int x = 10; x <= 20; ++x) build.push_back(place({x, 0, 0}, blocks::stone()));
        build.push_back(SpawnItem{at_block(15.5, 1.0, 0.5), {1200.0 / 4096.0, 0, 0}, {items::stone, 1}});
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
        // Tick 1: a forced ticket at chunk (0,0): entity ticking there, the
        // ring around it block ticking, the next ring border.
        Sim s(bundle(), 7, 0);
        // The levers come after the lamps: setting a block does not update the block itself.
        s.tick({place({0, 0, 0}, blocks::stone()), place({16, 0, 0}, blocks::stone()), place({32, 0, 0}, blocks::stone()),
                place({56, 0, 0}, blocks::stone()), place({2, 0, 5}, blocks::lamp()), place({1, 0, 5}, blocks::lever(true)),
                place({18, 0, 5}, blocks::lamp()), place({17, 0, 5}, blocks::lever(true)), place({34, 0, 5}, blocks::lamp()),
                place({33, 0, 5}, blocks::lever(true))});
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == 1 && rejected[0].b == static_cast<std::uint64_t>(ErrorCode::unloaded_chunk), "unloaded write");
        require(s.at({2, 0, 5}).flag() && s.at({18, 0, 5}).flag() && s.at({34, 0, 5}).flag(), "lamps lit at every loaded level");
        s.tick({use_lever({1, 0, 5}), use_lever({17, 0, 5}), use_lever({33, 0, 5})}); // 3: lamp ticks due 7
        s.tick({RemoveTicket{1}}); // 4
        require(of_kind(s.last, TraceKind::chunk_unloaded).size() == 25, "25 chunks unloaded");
        s.until(9);
        auto d = s.oracle.dump();
        require(d.overworld().ticks.size() == 3 && state_at(d, {2, 0, 5}).flag(), "pending ticks kept while unloaded");
        // 10: reload. The pending ticks keep the delay they had left when the
        // chunks dropped (they wait from tick 5), and run again from tick 12.
        s.tick({ticket(2, {0, 0}, 0)});
        d = s.oracle.dump();
        require(state_at(d, {2, 0, 5}).flag() && state_at(d, {18, 0, 5}).flag() && d.overworld().chunks[12].resumes == 12,
                "reloaded ticks wait");
        s.tick(); // 11
        require(s.at({2, 0, 5}).flag(), "still waiting");
        s.tick(); // 12
        d = s.oracle.dump();
        require(!state_at(d, {2, 0, 5}).flag() && !state_at(d, {18, 0, 5}).flag() && state_at(d, {34, 0, 5}).flag(),
                "entity- and block-ticking chunks drain; border waits");
        require(d.overworld().chunks.size() == 25 && d.overworld().chunks[12].pos == ChunkPos{0, 0}
                    && d.overworld().chunks[12].generation == 1,
                "reload increments generation");
        s.tick({SetBlock{{3, 0, 0}, blocks::stone().raw(), Generation{0}}, SetBlock{{4, 0, 0}, blocks::stone().raw(), Generation{1}}});
        const auto r = of_kind(s.last, TraceKind::command_rejected);
        require(r.size() == 1 && r[0].b == static_cast<std::uint64_t>(ErrorCode::stale_generation), "stale generation");
        require(s.at({4, 0, 0}).is(BlockType::stone), "current generation accepted");
        // A ticket takes effect in the chunk stage, after the tick's scheduled
        // ticks: the border chunk's overdue tick runs in the next tick.
        s.tick({ticket(3, {1, 0}, 0)}); // 14
        require(s.at({34, 0, 5}).flag(), "not before the chunk stage");
        s.tick();
        require(!s.at({34, 0, 5}).flag(), "border chunk tick drains once block ticking");
    });

    suite.run("command validation", [] {
        Sim s;
        s.tick({place({0, 0, 0}, blocks::hopper()), insert({0, 0, 0}, 0, items::stone, 60)});
        const auto piston_head = BlockState::of(BlockType::piston_head).raw();
        const auto extended = blocks::piston(D::up).with_flag(true).raw();
        s.tick({place({0, 1000, 0}, blocks::stone()), place({200, 0, 0}, blocks::stone()), SetBlock{{1, 0, 0}, 0xFFFFFFFFu, {}},
                SetBlock{{1, 0, 0}, piston_head, {}}, SetBlock{{1, 0, 0}, extended, {}}, use_lever({0, 0, 0}),
                insert({5, 5, 5}, 0, items::stone, 1), insert({0, 0, 0}, 5, items::stone, 1),
                insert({0, 0, 0}, 1, 9999, 1), insert({0, 0, 0}, 0, items::wheat, 1), insert({0, 0, 0}, 0, items::stone, 5),
                SpawnItem{at_block(0.5, 5, 0.5), {5000, 0, 0}, {items::stone, 1}}, RemoveEntity{{999}},
                ticket(1, {0, 0}, 1), RemoveTicket{77}, ticket(9, {0, 0}, 33)});
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
        s.tick(with_floor(floor(0, 2, 0, 0),
                          {place({0, 0, 0}, blocks::lever()), place({1, 0, 0}, blocks::comparator(D::east)), place({2, 0, 0}, blocks::lamp())}));
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
        s.tick(with_floor(floor(0, 4, 0, 4),
                          {place({0, 0, 0}, blocks::lever()), place({1, 0, 0}, blocks::comparator(D::east, true)),
                           place({2, 0, 0}, blocks::wire()), place({3, 0, 0}, blocks::wire()), place({4, 0, 0}, blocks::wire()),
                           place({1, 0, 1}, blocks::wire()), place({1, 0, 2}, blocks::wire()), place({1, 0, 3}, blocks::wire()),
                           place({1, 0, 4}, blocks::lever())}));
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
        s.tick(with_floor(floor(1, 2, 0, 5),
                          {place({0, 0, 0}, blocks::hopper(D::down)), place({1, 0, 0}, blocks::comparator(D::east)),
                           place({2, 0, 0}, blocks::lamp()), place({0, 0, 5}, blocks::chest()), place({1, 0, 5}, blocks::stone()),
                           place({2, 0, 5}, blocks::comparator(D::east))}));
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
        s.tick(with_floor(floor(15, 15, 1, 1), {place({15, 0, 0}, blocks::chest(D::north, blocks::ChestType::left)),
                                                place({16, 0, 0}, blocks::chest(D::north, blocks::ChestType::right)),
                                                place({15, 0, 1}, blocks::comparator(D::south))}));
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
        s.tick(with_floor(floor(0, 0, 0, 0), {place({0, 0, 0}, blocks::repeater(D::east, 4))}));
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

    suite.run("loot tables decide what blocks without rules drop", [] {
        namespace loot = circuit::gameplay::loot;
        using circuit::contracts::PlayerBreak;
        using circuit::contracts::SetPlayerSlot;
        const auto block_of = [](std::string_view name) { return registry::find_block("1.21.8", name).value(); };
        // Enchantment ids as a server numbers them; the test's tools carry
        // them as names, which its enchantment reader looks for.
        constexpr std::uint32_t silk_touch = 33, fortune = 13;
        const auto silk = named(items::iron_pickaxe, 1, "silk"), lucky = named(items::iron_pickaxe, 1, "lucky");
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->enchantment_level = [silk, lucky](const ItemStack& s, std::uint32_t e) -> long {
            if (e == silk_touch && s.components == silk.components) return 1;
            if (e == fortune && s.components == lucky.components) return 2;
            return 0;
        };
        using Entry = loot::Entry;
        using Function = loot::Function;
        // A bookshelf gives itself to silk touch, else three books.
        loot::Condition with_silk{.kind = loot::Condition::Kind::tool};
        with_silk.enchantments = {{silk_touch, 1, 255}};
        const Entry self{.kind = Entry::Kind::item, .item = items::bookshelf, .conditions = {with_silk}};
        const Entry books{.kind = Entry::Kind::item, .item = items::book, .functions = {{.kind = Function::Kind::set_count, .count = loot::Number{.value = 3}}}};
        data->block_loot[block_of("minecraft:bookshelf")] = one_pool({{.kind = Entry::Kind::alternatives, .children = {self, books}}});
        // Coal ore gives coal, which fortune multiplies; it needs a pickaxe.
        const Entry coal{.kind = Entry::Kind::item, .item = items::coal, .functions = {{.kind = Function::Kind::ore_bonus, .enchantment = fortune}}};
        data->block_loot[block_of("minecraft:coal_ore")] = one_pool({coal});
        data->block_tools[block_of("minecraft:coal_ore")] = {registry::Tool::pickaxe, 1};
        // A double slab gives two.
        const loot::Condition is_double{.kind = loot::Condition::Kind::block_state, .properties = {{"type", "double", {}, {}}}};
        const Entry slab{.kind = Entry::Kind::item, .item = items::oak_slab,
                         .functions = {{.kind = Function::Kind::set_count, .count = loot::Number{.value = 2}, .conditions = {is_double}}}};
        data->block_loot[block_of("minecraft:oak_slab")] = one_pool({slab});
        // More than a stack splits.
        const Entry balls{.kind = Entry::Kind::item, .item = items::clay_ball, .functions = {{.kind = Function::Kind::set_count, .count = loot::Number{.value = 100}}}};
        data->block_loot[block_of("minecraft:clay")] = one_pool({balls});

        const PlayerKey alex{9, 9};
        const std::vector<std::pair<BlockPos, BlockState>> blocks{
            {{0, 0, 0}, registry_block("minecraft:bookshelf")}, {{1, 0, 0}, registry_block("minecraft:bookshelf")},
            {{2, 0, 0}, registry_block("minecraft:coal_ore")},  {{3, 0, 0}, registry_block("minecraft:coal_ore")},
            {{4, 0, 0}, registry_block("minecraft:coal_ore")},  {{5, 0, 0}, registry_block("minecraft:oak_slab", {{"type", "double"}})},
            {{6, 0, 0}, registry_block("minecraft:oak_slab")},  {{7, 0, 0}, registry_block("minecraft:clay")},
            {{8, 0, 0}, registry_block("minecraft:dirt")}};
        const auto run = [&](std::shared_ptr<const circuit::gameplay::GameData> d) {
            Sim s(bundle(), 7, 2, std::move(d));
            std::vector<CommandBody> setup;
            for (const auto& [p, b] : blocks) setup.push_back(place(p, b));
            setup.push_back(SetPlayerSlot{alex, 36, silk});
            setup.push_back(SetPlayerSlot{alex, 37, {items::iron_pickaxe, 1}});
            setup.push_back(SetPlayerSlot{alex, 38, lucky});
            s.tick(setup);
            std::vector<std::vector<ItemStack>> drops;
            for (const auto& [p, slot] : std::vector<std::pair<BlockPos, std::uint8_t>>{
                     {{0, 0, 0}, 39}, {{1, 0, 0}, 36}, {{2, 0, 0}, 39}, {{3, 0, 0}, 37}, {{4, 0, 0}, 38},
                     {{5, 0, 0}, 39}, {{6, 0, 0}, 39}, {{7, 0, 0}, 39}, {{8, 0, 0}, 39}}) {
                const auto before = s.oracle.dump().overworld().entities.size();
                s.tick({PlayerBreak{alex, p, true, slot}});
                require(s.at(p).is_air(), "broken");
                const auto d = s.oracle.dump();
                std::vector<ItemStack> out;
                for (auto i = before; i < d.overworld().entities.size(); ++i)
                    if (d.overworld().entities[i].kind == circuit::gameplay::EntityKind::item) out.push_back(d.overworld().entities[i].stack);
                drops.push_back(out);
            }
            return drops;
        };
        const auto drops = run(data);
        const auto just = [](std::uint16_t item, std::uint8_t count) { return std::vector<ItemStack>{{item, count}}; };
        require(drops[0] == just(items::book, 3), "a bookshelf by hand: three books");
        require(drops[1] == just(items::bookshelf, 1), "with silk touch: the bookshelf");
        require(drops[2].empty(), "coal ore by hand: nothing");
        require(drops[3] == just(items::coal, 1), "with a pickaxe: coal");
        require(drops[4].empty(), "with fortune: nothing yet (a random bonus)");
        require(drops[5] == just(items::oak_slab, 2) && drops[6] == just(items::oak_slab, 1), "slabs: two when double");
        require(drops[7] == (std::vector<ItemStack>{{items::clay_ball, 64}, {items::clay_ball, 36}}), "a hundred split by the stack size");
        require(drops[8].empty(), "no table: nothing");
        const auto without = run(nullptr);
        require(std::ranges::all_of(without, [](const auto& d) { return d.empty(); }), "without data packs blocks without rules drop nothing");
    });

    suite.run("loot conditions, entries and functions", [] {
        namespace loot = circuit::gameplay::loot;
        using Entry = loot::Entry;
        using Condition = loot::Condition;
        using Function = loot::Function;
        const auto item = [](std::uint16_t id, std::vector<Condition> c = {}) { return Entry{.kind = Entry::Kind::item, .item = id, .conditions = std::move(c)}; };
        const auto table = [](std::vector<Entry> entries) { return one_pool(std::move(entries)); };
        loot::BreakContext ctx{{{"age", "5"}, {"half", "lower"}}, {items::shears, 1}, {}, nullptr, {}};
        const Condition age_3_to_6{.kind = Condition::Kind::block_state, .properties = {{"age", {}, 3, 6}}};
        const Condition age_7{.kind = Condition::Kind::block_state, .properties = {{"age", "7", {}, {}}}};
        const Condition shears{.kind = Condition::Kind::tool, .items = {items::shears}};
        const Condition other_block{.kind = Condition::Kind::block_state, .other_block = true};
        const auto one = [](std::uint16_t id) { return std::optional(std::vector<ItemStack>{{id, 1}}); };
        require(loot::drops(table({item(items::wheat, {age_3_to_6})}), ctx) == one(items::wheat), "a property range");
        require(loot::drops(table({item(items::wheat, {age_7})}), ctx)->empty(), "an exact property");
        require(loot::drops(table({item(items::wheat, {other_block})}), ctx)->empty(), "a condition on another block");
        require(loot::drops(table({item(items::vine, {shears})}), ctx) == one(items::vine), "a tool");
        require(loot::drops(table({item(items::vine, {{.kind = Condition::Kind::inverted, .children = {shears}}})}), ctx)->empty(), "inverted");
        require(loot::drops(table({item(items::vine, {{.kind = Condition::Kind::any_of, .children = {age_7, shears}}})}), ctx) == one(items::vine), "any of");
        require(loot::drops(table({item(items::vine, {{.kind = Condition::Kind::all_of, .children = {age_7, shears}}})}), ctx)->empty(), "all of");
        // Composites: a sequence stops at the first child that fails; two
        // candidates are a weighted random choice.
        const Entry sequence{.kind = Entry::Kind::sequence, .children = {item(items::wheat, {age_7}), item(items::vine)}};
        require(loot::drops(table({sequence}), ctx)->empty(), "a sequence");
        const Entry group{.kind = Entry::Kind::group, .children = {item(items::wheat), item(items::vine)}};
        require(!loot::drops(table({group}), ctx).has_value(), "two candidates need a random choice");
        require(loot::drops(table({Entry{.kind = Entry::Kind::empty}}), ctx)->empty(), "an empty entry");
        // Counts: limits, and a stack of none for a count of zero (which drops
        // no item, but as the official servers drop it: loot_rolls).
        Entry limited = item(items::wheat);
        limited.functions = {{.kind = Function::Kind::set_count, .count = loot::Number{.value = 9}}, {.kind = Function::Kind::limit_count, .max = 4}};
        require(loot::drops(table({limited}), ctx) == std::optional(std::vector<ItemStack>{{items::wheat, 4}}), "a limit");
        Entry none = item(items::wheat);
        none.functions = {{.kind = Function::Kind::set_count, .count = loot::Number{.value = 0}}};
        const auto zero = loot::drops(table({none}), ctx);
        require(zero && zero->size() == 1 && zero->front().count == 0, "a count of zero");
        // A roll that draws needs a random sequence.
        Entry chance = item(items::wheat);
        chance.conditions = {{.kind = loot::Condition::Kind::random_chance, .chance = 0.5}};
        require(!loot::drops(table({chance}), ctx).has_value(), "a chance without a sequence");
    });

    suite.run("recipes match anywhere in the grid, mirrored, in any order", [] {
        namespace recipes = circuit::gameplay::recipes;
        using recipes::Ingredient;
        using recipes::Recipe;
        const Ingredient iron{{items::iron_ingot}}, stick{{items::stick}}, coal_or_charcoal{{items::charcoal, items::coal}};
        const std::vector<Recipe> book{
            {Recipe::Kind::shaped, "test:shears", 2, 2, {{}, iron, iron, {}}, {items::shears, 1}},
            {Recipe::Kind::shaped, "test:torch", 1, 2, {coal_or_charcoal, stick}, {items::torch, 4}},
            {Recipe::Kind::shapeless, "test:pair", 0, 0, {coal_or_charcoal, Ingredient{{items::coal}}}, {items::black_dye, 1}},
        };
        std::array<ItemStack, 9> g{};
        const auto made = [&] {
            const auto m = recipes::match(book, {3, 3, g});
            return m ? m->recipe->id : std::string("none");
        };
        g[1] = g[3] = {items::iron_ingot, 1};
        require(made() == "test:shears", "top left corner");
        g = {};
        g[5] = g[7] = {items::iron_ingot, 1};
        require(made() == "test:shears", "bottom right corner");
        g = {};
        g[4] = g[8] = {items::iron_ingot, 1};
        require(made() == "test:shears", "mirrored");
        g[0] = {items::stick, 1};
        require(made() == "none", "an extra item");
        g = {};
        g[2] = {items::charcoal, 1};
        g[5] = {items::stick, 1};
        require(made() == "test:torch", "a column at the right edge");
        // Shapeless: the coal must take the coal ingredient, the charcoal the other.
        g = {};
        g[0] = {items::coal, 1};
        g[8] = {items::charcoal, 1};
        require(made() == "test:pair", "assignment by augmenting paths");
        g[8] = {items::stick, 1};
        require(made() == "none", "no assignment");
        // Transmute: the result keeps the input's components; an input that
        // is the result already makes nothing.
        Ingredient boxes{{items::shulker_box, items::red_shulker_box}}, dye{{items::red_dye}};
        std::ranges::sort(boxes.items); // ingredients list their items in ascending order
        const std::vector<Recipe> transmute{{Recipe::Kind::transmute, "test:red_box", 0, 0, {boxes, dye}, {items::red_shulker_box, 1}}};
        std::array<ItemStack, 4> small{};
        small[0] = {items::red_dye, 1};
        small[3] = named(items::shulker_box, 1, "Box");
        const auto m = recipes::match(transmute, {2, 2, small});
        require(m && m->result.item == items::red_shulker_box && m->result.components == small[3].components, "components kept");
        small[3] = {items::red_shulker_box, 1};
        require(!recipes::match(transmute, {2, 2, small}), "already the result");
        require(recipes::remainder(items::honey_bottle) == ItemStack{items::glass_bottle, 1} &&
                    recipes::remainder(items::milk_bucket) == ItemStack{items::bucket, 1} && recipes::remainder(items::coal).empty(),
                "remainders");
    });

    suite.run("the crafting grid crafts with the data packs' recipes", [] {
        namespace recipes = circuit::gameplay::recipes;
        using circuit::contracts::CloseContainer;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::SetPlayerSlot;
        using recipes::Ingredient;
        using recipes::Recipe;
        auto data = std::make_shared<circuit::gameplay::GameData>();
        const Ingredient log{{items::oak_log}}, iron{{items::iron_ingot}}, honey{{items::honey_bottle}};
        data->crafting = {
            {Recipe::Kind::shapeless, "test:planks", 0, 0, {log}, {items::oak_planks, 4}},
            {Recipe::Kind::shaped, "test:shears", 2, 2, {{}, iron, iron, {}}, {items::shears, 1}},
            {Recipe::Kind::shapeless, "test:sugar", 0, 0, {honey}, {items::sugar, 3}},
            {Recipe::Kind::transmute, "test:red_box", 0, 0, {Ingredient{{items::shulker_box}}, Ingredient{{items::red_dye}}}, {items::red_shulker_box, 1}},
        };
        const PlayerKey alex{4, 2};
        Sim s(bundle(), 7, 2, data);
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::nullopt, slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            const auto it = std::ranges::find(d.players, alex, &PlayerInventory::player);
            if (it != d.players.end()) return *it;
            PlayerInventory none;
            none.player = alex;
            return none;
        };
        const auto slot = [&](std::size_t i) { return player().slots[i]; };
        // The recipes taken results came from, for the recipe book (kind 0: crafting, by index).
        const auto used = [&] {
            std::vector<std::uint64_t> out;
            for (const auto& r : of_kind(s.last, TraceKind::recipe_used))
                if (r.a == alex.hi && r.b == alex.lo) out.push_back(r.c);
            return out;
        };
        // Placing a log shows planks; taking them uses one log each time.
        s.tick({SetPlayerSlot{alex, 9, {items::oak_log, 3}}});
        s.tick({click(9, 0, 0)});
        s.tick({click(1, 1, 0)});
        require(slot(0) == ItemStack{items::oak_planks, 4} && slot(1) == ItemStack{items::oak_log, 1}, "the result follows the grid");
        require(used().empty(), "no recipe used before a take");
        s.tick({click(1, 0, 0)});
        s.tick({click(0, 0, 0)});
        require(player().cursor == ItemStack{items::oak_planks, 4} && slot(1).count == 2 && slot(0).count == 4, "taken onto the cursor");
        require(used() == std::vector<std::uint64_t>{0}, "the planks' recipe used");
        s.tick({click(0, 1, 0)});
        require(player().cursor.count == 8 && slot(1).count == 1, "a right click takes the whole result too");
        s.tick({click(20, 0, 0)});
        s.tick({click(0, 0, 1)});
        require(slot(20).count == 12 && slot(1).empty() && slot(0).empty(), "shift-click crafts into the inventory");
        // Shapes: the pattern mirrored, and not another one.
        s.tick({SetPlayerSlot{alex, 1, {items::iron_ingot, 1}}, SetPlayerSlot{alex, 4, {items::iron_ingot, 1}}});
        require(slot(0) == ItemStack{items::shears, 1}, "mirrored shears");
        s.tick({SetPlayerSlot{alex, 4, {}}, SetPlayerSlot{alex, 2, {items::iron_ingot, 1}}});
        require(slot(0).empty(), "a row of iron is no recipe");
        s.tick({SetPlayerSlot{alex, 2, {}}, SetPlayerSlot{alex, 4, {items::iron_ingot, 1}}});
        s.tick({click(0, 2, 2)});
        require(slot(38) == ItemStack{items::shears, 1} && slot(1).empty() && slot(4).empty(), "a number key crafts into the hotbar");
        require(used() == std::vector<std::uint64_t>{1}, "the shears' recipe used");
        // A honey bottle leaves its bottle: in the inventory while honey is
        // left, else in the grid.
        s.tick({SetPlayerSlot{alex, 2, {items::honey_bottle, 2}}});
        s.tick({click(0, 0, 0)});
        require(player().cursor == ItemStack{items::sugar, 3} && slot(2) == ItemStack{items::honey_bottle, 1} &&
                    slot(36) == ItemStack{items::glass_bottle, 1},
                "the bottle goes into the inventory");
        s.tick({click(0, 0, 0)});
        require(player().cursor == ItemStack{items::sugar, 6} && slot(2) == ItemStack{items::glass_bottle, 1} && slot(0).empty(),
                "the bottle stays in the grid");
        s.tick({click(21, 0, 0), SetPlayerSlot{alex, 2, {}}});
        // Transmuting keeps the box's name; throwing the result crafts.
        s.tick({SetPlayerSlot{alex, 1, named(items::shulker_box, 1, "Box")}, SetPlayerSlot{alex, 4, {items::red_dye, 2}}});
        require(slot(0) == named(items::red_shulker_box, 1, "Box"), "the name is kept");
        const auto before = s.oracle.dump().overworld().entities.size();
        s.tick({click(0, 0, 4)});
        const auto d = s.oracle.dump();
        require(d.overworld().entities.size() == before + 1 && d.overworld().entities.back().stack == named(items::red_shulker_box, 1, "Box") &&
                    slot(1).empty() && slot(4) == ItemStack{items::red_dye, 1} && slot(0).empty(),
                "thrown from the result");
        // Closing the window returns the grid and clears the result.
        s.tick({SetPlayerSlot{alex, 4, {}}, SetPlayerSlot{alex, 1, {items::oak_log, 1}}});
        require(slot(0) == ItemStack{items::oak_planks, 4}, "planks again");
        s.tick({CloseContainer{alex, at_block(0.5, 2, 0.5), {}}});
        require(slot(0).empty() && slot(1).empty() && slot(4).empty(), "closed: the grid is empty");
        // Without data packs crafting is not modeled.
        Sim plain;
        plain.tick({SetPlayerSlot{alex, 1, {items::oak_log, 1}}});
        const auto p = plain.oracle.dump().players;
        require(p.size() == 1 && p[0].slots[0].empty(), "no result without data");
    });

    suite.run("recipes of the recipe book are placed into grids and furnaces", [] {
        namespace recipes = circuit::gameplay::recipes;
        using circuit::contracts::PlaceRecipe;
        using circuit::contracts::SetPlayerSlot;
        using circuit::gameplay::blocks::FurnaceKind;
        using recipes::Ingredient;
        using recipes::Recipe;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        const auto raw_iron = item("minecraft:raw_iron"), egg = item("minecraft:egg"), sugar = item("minecraft:sugar"),
                   pumpkin = item("minecraft:pumpkin");
        auto data = std::make_shared<circuit::gameplay::GameData>();
        const Ingredient log{{items::oak_log}}, p{{items::oak_planks}};
        data->crafting = {
            {Recipe::Kind::shapeless, "test:planks", 0, 0, {log}, {items::oak_planks, 4}},
            {Recipe::Kind::shaped, "test:table", 2, 2, {p, p, p, p}, {item("minecraft:crafting_table"), 1}},
            {Recipe::Kind::shaped, "test:sticks", 1, 2, {p, p}, {item("minecraft:stick"), 4}},
            {Recipe::Kind::shaped, "test:chest", 3, 3, {p, p, p, p, {}, p, p, p, p}, {item("minecraft:chest"), 1}},
            {Recipe::Kind::shaped, "test:slab", 3, 1, {p, p, p}, {item("minecraft:oak_slab"), 6}},
            {Recipe::Kind::shapeless, "test:pie", 0, 0, {Ingredient{{pumpkin}}, Ingredient{{sugar}}, Ingredient{{egg}}}, {item("minecraft:pumpkin_pie"), 1}},
        };
        data->cooking = {{recipes::Cooking::Kind::smelting, "test:iron", {{raw_iron}}, {item("minecraft:iron_ingot"), 1}, 0.7f, 200}};
        data->stonecutting = {{"test:stone_slab", {{items::stone}}, {item("minecraft:stone_slab"), 2}}};
        const PlayerKey alex{4, 2};
        const BlockPos table{0, 0, 0}, furnace{2, 0, 0};
        Sim s(bundle(), 7, 2, data);
        s.tick({place(table, registry_block("crafting_table")), place(furnace, blocks::furnace(FurnaceKind::furnace, Direction::south))});
        const auto player = [&] {
            const auto d = s.oracle.dump();
            const auto it = std::ranges::find(d.players, alex, &PlayerInventory::player);
            if (it != d.players.end()) return *it;
            PlayerInventory none;
            none.player = alex;
            return none;
        };
        const auto slot = [&](std::size_t i) { return player().slots[i]; };
        const auto counts = [&](std::span<const std::size_t> slots) {
            std::vector<int> out;
            for (const auto i : slots) out.push_back(slot(i).item == items::oak_planks || slot(i).empty() ? slot(i).count : -1);
            return out;
        };
        const auto own = [&](std::uint8_t kind, std::uint32_t recipe, bool all) { return PlaceRecipe{alex, std::nullopt, kind, recipe, all}; };
        const auto ghosts = [&] {
            std::vector<std::uint64_t> out;
            for (const auto& r : of_kind(s.last, TraceKind::recipe_ghost))
                if (r.a == alex.hi && r.b == alex.lo) out.push_back(r.c);
            return out;
        };
        const std::array<std::size_t, 4> grid{1, 2, 3, 4};
        // Once, once more, as many as fit: from the hotbar first.
        s.tick({SetPlayerSlot{alex, 9, {items::oak_log, 2}}, SetPlayerSlot{alex, 36, {items::oak_log, 2}}});
        s.tick({own(0, 0, false)});
        require(slot(1) == ItemStack{items::oak_log, 1} && slot(36).count == 1 && slot(9).count == 2 && slot(0) == ItemStack{items::oak_planks, 4},
                "once, from the hotbar");
        s.tick({own(0, 0, false)});
        require(slot(1).count == 2 && slot(36).empty() && slot(9).count == 2 && ghosts().empty(), "once more");
        s.tick({own(0, 0, true)});
        require(slot(1).count == 4 && slot(9).empty(), "as many as fit");
        // Too little for one: the grid goes back (the first free slot of the
        // hotbar) and the ghost is shown.
        s.tick({own(0, 1, false)});
        require(slot(1).empty() && slot(0).empty() && slot(36) == ItemStack{items::oak_log, 4}, "the grid went back");
        require(ghosts() == std::vector<std::uint64_t>{1}, "the table's ghost");
        // Shapes: a narrow recipe at the left, a big one cut to the grid
        // (still needing all its items); the grid's own items count.
        s.tick({SetPlayerSlot{alex, 36, {}}, SetPlayerSlot{alex, 10, {items::oak_planks, 8}}});
        s.tick({own(0, 2, false)});
        require(counts(grid) == std::vector{1, 0, 1, 0} && slot(10).count == 6, "sticks in the left column");
        s.tick({own(0, 3, false)});
        require(counts(grid) == std::vector{1, 1, 1, 1} && slot(10).count == 4, "a chest cut to the grid");
        s.tick({SetPlayerSlot{alex, 10, {items::oak_planks, 3}}});
        s.tick({own(0, 3, false)});
        require(counts(grid) == std::vector{0, 0, 0, 0} && slot(10).count == 7 && ghosts() == std::vector<std::uint64_t>{3}, "seven planks make no chest");
        // Once more than the grid's smallest stack.
        s.tick({SetPlayerSlot{alex, 1, {items::oak_planks, 2}}, SetPlayerSlot{alex, 2, {items::oak_planks, 1}},
                SetPlayerSlot{alex, 3, {items::oak_planks, 1}}, SetPlayerSlot{alex, 4, {items::oak_planks, 1}}, SetPlayerSlot{alex, 10, {items::oak_planks, 3}}});
        s.tick({own(0, 1, false)});
        require(counts(grid) == std::vector{2, 2, 2, 2} && slot(10).empty(), "2 1 1 1 becomes 2 2 2 2");
        // As many as fit is limited by each item's stack size (16 eggs).
        s.tick({SetPlayerSlot{alex, 11, {pumpkin, 30}}, SetPlayerSlot{alex, 12, {sugar, 30}}, SetPlayerSlot{alex, 13, {egg, 16}},
                SetPlayerSlot{alex, 14, {egg, 16}}});
        s.tick({own(0, 5, true)});
        require(slot(1) == ItemStack{pumpkin, 16} && slot(2) == ItemStack{sugar, 16} && slot(3) == ItemStack{egg, 16} && slot(13).empty() &&
                    slot(14).count == 16 && slot(36) == ItemStack{items::oak_planks, 8},
                "sixteen pies");
        // A cooking recipe in a crafting grid: nothing when there is raw
        // iron, else the grid goes back and the ghost is shown.
        s.tick({SetPlayerSlot{alex, 15, {raw_iron, 1}}});
        s.tick({own(1, 0, false)});
        require(slot(1) == ItemStack{pumpkin, 16} && ghosts().empty(), "a cooking recipe changes nothing");
        s.tick({SetPlayerSlot{alex, 15, {}}});
        s.tick({own(1, 0, false)});
        require(slot(1).empty() && slot(11) == ItemStack{pumpkin, 30} && ghosts() == std::vector<std::uint64_t>{std::uint64_t{1} << 32}, "no raw iron");
        // A full inventory: the grid cannot go back, and nothing happens.
        std::vector<CommandBody> full;
        for (std::uint8_t i = 9; i < 45; ++i) full.push_back(SetPlayerSlot{alex, i, {items::cobblestone, 64}});
        full.push_back(SetPlayerSlot{alex, 1, {items::oak_log, 1}});
        s.tick(std::move(full));
        s.tick({own(0, 1, false)});
        require(slot(1) == ItemStack{items::oak_log, 1} && ghosts().empty(), "nothing without room");
        std::vector<CommandBody> empty;
        for (std::uint8_t i = 1; i < 45; ++i) empty.push_back(SetPlayerSlot{alex, i, {}});
        s.tick(std::move(empty));
        // Going back, the grid's items fill the main hand's slot first.
        s.tick({SetPlayerSlot{alex, 36, {items::oak_log, 10}}, SetPlayerSlot{alex, 38, {items::oak_log, 10}}, SetPlayerSlot{alex, 1, {items::oak_log, 1}}});
        s.tick({PlaceRecipe{alex, std::nullopt, 0, 1, false, 38}});
        require(slot(1).empty() && slot(36).count == 10 && slot(38).count == 11, "the log into the selected slot");
        s.tick({SetPlayerSlot{alex, 36, {}}, SetPlayerSlot{alex, 38, {}}});
        // A crafting table centers narrow and low recipes; it shows no ghost
        // of a cooking recipe.
        const auto at_table = [&](std::uint32_t recipe, bool all, std::uint8_t kind = 0) { return PlaceRecipe{alex, table, kind, recipe, all}; };
        const auto cells = [&] {
            std::vector<int> out;
            for (std::size_t k = 1; k <= 9; ++k) out.push_back(player().table[k].count);
            return out;
        };
        s.tick({SetPlayerSlot{alex, 36, {items::oak_planks, 6}}});
        s.tick({at_table(2, false)});
        require(cells() == std::vector{0, 1, 0, 0, 1, 0, 0, 0, 0}, "sticks in the middle column");
        s.tick({at_table(4, false)});
        require(cells() == std::vector{0, 0, 0, 1, 1, 1, 0, 0, 0} && player().table[0].item == item("minecraft:oak_slab"), "a slab in the middle row");
        s.tick({at_table(1, true)});
        require(cells() == std::vector{1, 1, 0, 1, 1, 0, 0, 0, 0}, "a table at the top left");
        s.tick({at_table(0, false, 1)});
        require(cells() == std::vector<int>(9, 0) && slot(36).count == 6 && ghosts().empty(), "no ghost of a cooking recipe at a table");
        // A furnace's input: once, as many as fit; recipes of other kinds.
        const auto at_furnace = [&](std::uint8_t kind, std::uint32_t recipe, bool all) { return PlaceRecipe{alex, furnace, kind, recipe, all}; };
        const auto input = [&] { return block_entity_at(s.oracle.dump(), furnace)->slots[0]; };
        s.tick({SetPlayerSlot{alex, 37, {raw_iron, 10}}, SetPlayerSlot{alex, 38, {items::oak_log, 1}}});
        s.tick({at_furnace(1, 0, false)});
        require(input() == ItemStack{raw_iron, 1} && slot(37).count == 9, "one raw iron");
        s.tick({at_furnace(1, 0, true)});
        require(input() == ItemStack{raw_iron, 10} && slot(37).empty(), "all of it");
        s.tick({at_furnace(0, 0, false)});
        require(input() == ItemStack{raw_iron, 10} && ghosts().empty(), "a crafting recipe with its items changes nothing");
        s.tick({at_furnace(2, 0, false)});
        require(input().empty() && slot(37) == ItemStack{raw_iron, 10} && ghosts() == std::vector<std::uint64_t>{std::uint64_t{2} << 32},
                "a stonecutting recipe without stone");
    });

    suite.run("with doLimitedCrafting on, grids make only the recipes a player knows", [] {
        namespace recipes = circuit::gameplay::recipes;
        using circuit::contracts::KnowRecipes;
        using circuit::contracts::SetGameRule;
        using circuit::contracts::SetPlayerSlot;
        using recipes::Ingredient;
        using recipes::Recipe;
        auto data = std::make_shared<circuit::gameplay::GameData>();
        const Ingredient log{{items::oak_log}}, p{{items::oak_planks}};
        data->crafting = {
            {Recipe::Kind::shapeless, "test:planks", 0, 0, {log}, {items::oak_planks, 4}},
            {Recipe::Kind::shaped, "test:table", 2, 2, {p, p, p, p}, {*circuit::gameplay::items::by_name("minecraft:crafting_table"), 1}},
        };
        const PlayerKey alex{4, 2};
        Sim s(bundle(), 7, 2, data);
        const auto player = [&] {
            const auto d = s.oracle.dump();
            const auto it = std::ranges::find(d.players, alex, &PlayerInventory::player);
            if (it != d.players.end()) return *it;
            PlayerInventory none;
            none.player = alex;
            return none;
        };
        const auto result = [&] { return player().slots[0]; };
        s.tick({SetGameRule{"doLimitedCrafting", 1}, SetPlayerSlot{alex, 1, {items::oak_log, 1}}});
        require(result().empty(), "an unknown recipe makes nothing");
        s.tick({KnowRecipes{alex, {0}}});
        require(result().empty() && player().known_crafting == std::vector<std::uint32_t>{0}, "learned: the result waits for the grid");
        s.tick({SetPlayerSlot{alex, 1, {items::oak_log, 2}}});
        require(result() == ItemStack{items::oak_planks, 4}, "the grid changed");
        s.tick({KnowRecipes{alex, {0}, false}});
        require(result() == ItemStack{items::oak_planks, 4} && player().known_crafting.empty(), "forgotten: the result stays");
        s.tick({SetPlayerSlot{alex, 1, {items::oak_log, 1}}});
        require(result().empty(), "until the grid changes");
        s.tick({SetGameRule{"doLimitedCrafting", 0}});
        s.tick({SetPlayerSlot{alex, 1, {items::oak_log, 2}}});
        require(result() == ItemStack{items::oak_planks, 4}, "the rule off");
        // Known recipes stay ascending, each once; replacing sets them all.
        s.tick({KnowRecipes{alex, {1, 0, 1}}});
        require((player().known_crafting == std::vector<std::uint32_t>{0, 1}), "ascending, once each");
        s.tick({KnowRecipes{alex, {1}, true, true}});
        require((player().known_crafting == std::vector<std::uint32_t>{1}), "replaced");
        s.tick({KnowRecipes{alex, {}, false, true}});
        require(player().known_crafting.empty(), "none known");
    });

    suite.run("smithing tables upgrade and trim by the data packs' recipes", [] {
        namespace recipes = circuit::gameplay::recipes;
        using circuit::contracts::CloseContainer;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::SetPlayerSlot;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        const auto upgrade = item("minecraft:netherite_upgrade_smithing_template"), coast = item("minecraft:coast_armor_trim_smithing_template");
        const auto netherite = item("minecraft:netherite_ingot"), chestplate = item("minecraft:iron_chestplate");
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->smithing = {
            {recipes::Smithing::Kind::transform, "minecraft:netherite_pickaxe_smithing", recipes::Ingredient{{upgrade}},
             recipes::Ingredient{{items::diamond_pickaxe}}, recipes::Ingredient{{netherite}}, {item("minecraft:netherite_pickaxe"), 1}, {}},
            {recipes::Smithing::Kind::trim, "minecraft:coast_armor_trim_smithing_template_smithing_trim", recipes::Ingredient{{coast}},
             recipes::Ingredient{{chestplate}}, recipes::Ingredient{{items::iron_ingot, netherite}}, {}, "minecraft:coast"},
        };
        data->smithing_template = {upgrade, coast};
        std::ranges::sort(data->smithing_template);
        data->smithing_base = {items::diamond_pickaxe, chestplate};
        std::ranges::sort(data->smithing_base);
        data->smithing_addition = {items::iron_ingot, netherite};
        std::ranges::sort(data->smithing_addition);
        data->trim_materials = {{items::iron_ingot, "minecraft:iron"}, {netherite, "minecraft:netherite"}};
        // A stand-in for the server's encoding: the trim as one component holding its names.
        data->trimmed = [](const circuit::contracts::ComponentPatch&, std::string_view material, std::string_view pattern) {
            const auto text = std::string(material) + "/" + std::string(pattern);
            std::vector<std::byte> bytes;
            for (const char ch : text) bytes.push_back(static_cast<std::byte>(ch));
            return std::optional(circuit::contracts::ComponentPatch::make(772, {{1, bytes}}, {}, {}));
        };
        const PlayerKey alex{4, 2};
        const BlockPos table{0, 0, 0};
        Sim s(bundle(), 7, 2, data);
        s.tick({place(table, registry_block("smithing_table"))});
        // Smithing window: 0 template, 1 base, 2 addition, 3 result, 4-30 the main inventory, 31-39 the hotbar.
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::optional(table), slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            const auto it = std::ranges::find(d.players, alex, &PlayerInventory::player);
            if (it != d.players.end()) return *it;
            PlayerInventory none;
            none.player = alex;
            return none;
        };
        s.tick({SetPlayerSlot{alex, 36, {upgrade, 2}}, SetPlayerSlot{alex, 37, {items::diamond_pickaxe, 1, 40}},
                SetPlayerSlot{alex, 38, {netherite, 2}}, SetPlayerSlot{alex, 39, {items::stone, 5}}, SetPlayerSlot{alex, 40, {coast, 1}},
                SetPlayerSlot{alex, 41, {chestplate, 1}}, SetPlayerSlot{alex, 42, {items::iron_ingot, 1}}});
        s.tick({click(31, 0, 1), click(32, 0, 1), click(34, 0, 1), click(33, 0, 1)});
        require(player().smith[0] == ItemStack{upgrade, 2} && player().smith[1].item == items::diamond_pickaxe &&
                    player().slots[9] == ItemStack{items::stone, 5} && player().smith[3] == ItemStack{item("minecraft:netherite_pickaxe"), 1, 40},
                "each input to its slot, the stone across; the result keeps the damage");
        s.tick({click(3, 0, 0)});
        require(player().cursor.item == item("minecraft:netherite_pickaxe") && player().smith[0].count == 1 && player().smith[1].empty() &&
                    player().smith[2].count == 1 && player().smith[3].empty(),
                "taken: one of each input used");
        // The addition slot holds netherite: iron finds no slot and goes across.
        s.tick({click(5, 0, 0), click(0, 0, 1), click(35, 0, 1), click(36, 0, 1), click(37, 0, 1)});
        require(player().smith[0] == ItemStack{coast, 1} && player().smith[1].item == chestplate && player().smith[2].item == netherite &&
                    player().slots[12].item == items::iron_ingot,
                "the iron went across, to the first empty slot of the main inventory");
        require(player().smith[3].item == chestplate && !player().smith[3].components.empty(), "a netherite trim");
        const auto trimmed = player().smith[3];
        s.tick({click(3, 7, 2)});
        require(player().slots[43] == trimmed && player().smith[0].empty() && player().smith[1].empty(), "a number key takes it");
        // The trimmed chestplate again, with its own trim: nothing.
        s.tick({SetPlayerSlot{alex, 40, {coast, 1}}});
        s.tick({click(35, 0, 1), click(38, 0, 1)});
        require(player().smith[1] == trimmed && player().smith[3].empty(), "the same trim makes nothing");
        s.tick({click(4, 0, 0), click(3, 0, 0), click(1, 0, 0), click(0, 0, 0), click(4, 0, 0)});
        require(player().smith[3].empty() && player().slots[9] == ItemStack{items::stone, 5} && player().smith[0].item == coast &&
                    player().smith[1] == trimmed,
                "stone fits no smithing slot");
        s.tick({CloseContainer{alex, at_block(0.5, 2, 0.5), {}}});
        require(std::ranges::all_of(player().smith, &ItemStack::empty), "closing gives the inputs back");
    });

    suite.run("looms add the chosen pattern's layer and keep their choice as both official servers do", [] {
        using circuit::contracts::ComponentPatch;
        using circuit::contracts::ContainerButton;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::SetPlayerSlot;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->loom_patterns = {"minecraft:square_bottom_left", "minecraft:stripe_top", "minecraft:gradient"};
        data->pattern_item_patterns = {{item("minecraft:creeper_banner_pattern"), {"minecraft:creeper"}}};
        // A stand-in for the server's encoding: one byte per layer's pattern
        // name length and color, appended.
        data->with_banner_layer = [](const ComponentPatch& base, std::string_view pattern, std::uint32_t color) {
            std::vector<std::byte> bytes;
            for (const auto& a : base.added()) bytes = a.value;
            bytes.push_back(static_cast<std::byte>(pattern.size()));
            bytes.push_back(static_cast<std::byte>(color));
            return std::optional(ComponentPatch::make(772, {{1, bytes}}, {}, {}));
        };
        const PlayerKey alex{4, 2};
        const BlockPos loom{0, 0, 0};
        Sim s(bundle(), 7, 2, data);
        s.tick({place(loom, registry_block("loom", {{"facing", "north"}}))});
        // Loom window: 0 banner, 1 dye, 2 pattern item, 3 result, 4-30 the main inventory, 31-39 the hotbar.
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::optional(loom), slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto press = [&](std::int32_t b) { return ContainerButton{alex, std::optional(loom), b}; };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, alex, &PlayerInventory::player);
        };
        const auto banner = item("minecraft:white_banner");
        s.tick({SetPlayerSlot{alex, 36, {banner, 2}}, SetPlayerSlot{alex, 37, {item("minecraft:red_dye"), 3}},
                SetPlayerSlot{alex, 38, {item("minecraft:creeper_banner_pattern"), 1}}, SetPlayerSlot{alex, 39, {item("minecraft:blue_dye"), 1}}});
        s.tick({press(1), click(31, 0, 1)});
        require(player().loom_pattern == -1, "no button without a banner and a dye");
        s.tick({click(32, 0, 1), press(1)});
        require(player().loom_pattern == 1 && player().loom[3].item == banner && !player().loom[3].components.empty(), "the second pattern");
        s.tick({press(3)});
        require(player().loom_pattern == 1, "a button beyond the patterns does nothing");
        s.tick({click(34, 0, 1)});
        require(player().loom[1].item == item("minecraft:red_dye") && player().slots[39].item == item("minecraft:blue_dye"),
                "a dye goes nowhere while another fills its slot");
        s.tick({click(3, 0, 0)});
        require(player().loom[0].count == 1 && player().loom[1].count == 2 && player().loom_pattern == 1 && !player().loom[3].empty(),
                "taken: a banner and a dye used, the choice kept");
        s.tick({click(33, 0, 1)});
        require(player().loom_pattern == 0, "a pattern item with one pattern chooses it");
        s.tick({click(2, 0, 1)});
        require(player().loom_pattern == -1 && player().loom[3].empty(), "without it the creeper is not on offer");
        s.tick({press(2), click(3, 0, 1)});
        require(player().loom[0].empty() && player().loom_pattern == -1, "shift-clicked until the banners ran out; the choice cleared");
    });

    suite.run("grindstones remove all but curses and merge durability", [] {
        using circuit::contracts::CloseContainer;
        using circuit::contracts::ComponentPatch;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::SetPlayerSlot;
        using Levels = std::vector<std::pair<std::uint32_t, std::int32_t>>;
        // Stand-ins for the server's encoding: type 1 the enchantments, 2 the
        // stored ones, 3 the repair cost, one byte per id and level.
        const auto patch = [](const Levels& plain, const Levels& stored, std::int32_t cost) {
            std::vector<ComponentPatch::Added> added;
            for (const auto& [type, list] : {std::pair{std::uint16_t{1}, plain}, std::pair{std::uint16_t{2}, stored}}) {
                if (list.empty()) continue;
                std::vector<std::byte> bytes;
                for (const auto& [id, level] : list) {
                    bytes.push_back(static_cast<std::byte>(id));
                    bytes.push_back(static_cast<std::byte>(level));
                }
                added.push_back({type, bytes});
            }
            if (cost > 0) added.push_back({3, {static_cast<std::byte>(cost)}});
            return added.empty() ? ComponentPatch{} : ComponentPatch::make(772, std::move(added), {}, {});
        };
        const auto read = [](const ComponentPatch& p, bool stored) {
            Levels out;
            for (const auto& a : p.added())
                if (a.type == (stored ? 2 : 1))
                    for (std::size_t k = 0; k + 1 < a.value.size(); k += 2)
                        out.emplace_back(static_cast<std::uint32_t>(a.value[k]), static_cast<std::int32_t>(a.value[k + 1]));
            return out;
        };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->curses = {2, 40};
        data->enchantments_of = read;
        data->with_enchantments = [&](const ComponentPatch& base, bool stored, const Levels& levels, bool, std::int32_t cost) {
            return std::optional(stored ? patch(read(base, false), levels, cost) : patch(levels, read(base, true), cost));
        };
        const PlayerKey alex{4, 2};
        const BlockPos grindstone{0, 0, 0};
        Sim s(bundle(), 7, 2, data);
        s.tick({place(grindstone, registry_block("grindstone", {{"face", "floor"}, {"facing", "north"}}))});
        // Grindstone window: 0 and 1 the inputs, 2 the result, 3-29 the main inventory, 30-38 the hotbar.
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::optional(grindstone), slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, alex, &PlayerInventory::player);
        };
        auto cursed = ItemStack{items::diamond_pickaxe, 1, 1000};
        cursed.components = patch({{40, 1}, {13, 2}}, {}, 0);
        auto fortune = ItemStack{items::diamond_pickaxe, 1, 900};
        fortune.components = patch({{13, 3}}, {}, 0);
        s.tick({SetPlayerSlot{alex, 36, cursed}, SetPlayerSlot{alex, 37, fortune}, SetPlayerSlot{alex, 38, {items::stone, 3}},
                SetPlayerSlot{alex, 39, {items::iron_pickaxe, 1, 50}}});
        s.tick({click(30, 0, 1)});
        auto expected = ItemStack{items::diamond_pickaxe, 1, 1000};
        expected.components = patch({{40, 1}}, {}, 1);
        require(player().grind[2] == expected, "the curse kept, its repair cost 1");
        s.tick({click(31, 0, 1)});
        expected.damage = 261;
        require(player().grind[2] == expected, "two of a kind: both durabilities and 5% of 1561");
        s.tick({click(2, 0, 1)});
        require(player().grind[0].empty() && player().grind[1].empty() && player().slots[44] == expected, "taken: both inputs used up");
        s.tick({click(32, 0, 1), click(33, 0, 1)});
        require(player().slots[9].item == items::stone && player().grind[0].item == items::iron_pickaxe && player().grind[2].empty(),
                "stone goes across; a plain damaged pickaxe alone makes nothing");
        s.tick({CloseContainer{alex, at_block(0.5, 2, 0.5), {}}});
        require(player().grind[0].empty() && player().slots[36].item == items::iron_pickaxe, "closing gives the input back");
    });

    suite.run("anvils repair, combine, enchant and rename for levels", [] {
        using circuit::contracts::ChangeExperience;
        using circuit::contracts::CloseContainer;
        using circuit::contracts::ComponentPatch;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::RenameItem;
        using circuit::contracts::SetPlayerSlot;
        using Levels = std::vector<std::pair<std::uint32_t, std::int32_t>>;
        // Stand-ins for the server's encoding: type 1 the enchantments, 2 the
        // stored ones (one byte per id and level), 3 the repair cost, 4 the
        // custom name (its bytes).
        struct Parts {
            Levels plain{}, stored{};
            std::int32_t cost{};
            std::optional<std::string> name{};
        };
        const auto make = [](const Parts& parts) {
            std::vector<ComponentPatch::Added> added;
            for (const auto& [type, list] : {std::pair{std::uint16_t{1}, parts.plain}, std::pair{std::uint16_t{2}, parts.stored}}) {
                if (list.empty()) continue;
                std::vector<std::byte> bytes;
                for (const auto& [id, level] : list) {
                    bytes.push_back(static_cast<std::byte>(id));
                    bytes.push_back(static_cast<std::byte>(level));
                }
                added.push_back({type, bytes});
            }
            if (parts.cost > 0) added.push_back({3, {static_cast<std::byte>(parts.cost)}});
            if (parts.name) {
                std::vector<std::byte> bytes;
                for (const auto ch : *parts.name) bytes.push_back(static_cast<std::byte>(ch));
                added.push_back({4, bytes});
            }
            return added.empty() ? ComponentPatch{} : ComponentPatch::make(772, std::move(added), {}, {});
        };
        const auto parts_of = [](const ComponentPatch& p) {
            Parts out;
            for (const auto& a : p.added()) {
                if (a.type == 1 || a.type == 2)
                    for (std::size_t k = 0; k + 1 < a.value.size(); k += 2)
                        (a.type == 1 ? out.plain : out.stored)
                            .emplace_back(static_cast<std::uint32_t>(a.value[k]), static_cast<std::int32_t>(a.value[k + 1]));
                if (a.type == 3) out.cost = static_cast<std::int32_t>(a.value[0]);
                if (a.type == 4) {
                    out.name.emplace();
                    for (const auto b : a.value) *out.name += static_cast<char>(b);
                }
            }
            return out;
        };
        const auto stack = [&](std::uint16_t item, Parts parts, std::uint16_t damage = 0) {
            return ItemStack{item, 1, damage, make(parts)};
        };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->enchantments_of = [&](const ComponentPatch& p, bool stored) { return stored ? parts_of(p).stored : parts_of(p).plain; };
        data->with_enchantments = [&](const ComponentPatch& base, bool stored, const Levels& levels, bool, std::int32_t cost) {
            auto parts = parts_of(base);
            (stored ? parts.stored : parts.plain) = levels;
            parts.cost = cost;
            return std::optional(make(parts));
        };
        data->repair_cost_of = [&](const ComponentPatch& p) { return parts_of(p).cost; };
        data->custom_name_of = [&](const ComponentPatch& p) { return parts_of(p).name; };
        data->with_custom_name = [&](const ComponentPatch& base, const std::optional<std::string>& name) {
            auto parts = parts_of(base);
            parts.name = name;
            return std::optional(make(parts));
        };
        // Efficiency (8): cost 1, up to V on pickaxes; Unbreaking (34): cost
        // 2, up to III on pickaxes and swords; Fortune (13) and Silk Touch
        // (20) exclude each other.
        data->enchantment_rules[8] = {1, 5, {items::iron_pickaxe, items::diamond_pickaxe}, {}};
        data->enchantment_rules[34] = {2, 3, {items::iron_pickaxe, items::diamond_pickaxe, items::diamond_sword}, {}};
        data->enchantment_rules[13] = {4, 3, {items::diamond_pickaxe}, {13, 20}};
        data->enchantment_rules[20] = {8, 1, {items::diamond_pickaxe}, {13, 20}};
        for (auto& [id, rules] : data->enchantment_rules) std::ranges::sort(rules.supported);
        data->repair_materials[items::iron_pickaxe] = {items::iron_ingot};
        const PlayerKey alex{4, 2};
        const BlockPos anvil{0, 0, 0};
        Sim s(bundle(), 7, 2, data);
        // On a floor: an anvil falls without one.
        s.tick({place({0, -1, 0}, blocks::stone()), place(anvil, registry_block("anvil", {{"facing", "north"}}))});
        // Anvil window: 0 and 1 the inputs, 2 the result, 3-29 the main inventory, 30-38 the hotbar.
        const auto click = [&](std::int16_t slot, std::uint8_t button = 0, std::uint8_t mode = 1, bool creative = false) {
            return ContainerClick{alex, std::optional(anvil), slot, button, mode, creative, at_block(0.5, 2, 0.5), {}};
        };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, alex, &PlayerInventory::player);
        };
        s.tick({SetPlayerSlot{alex, 36, {items::iron_pickaxe, 1, 200}}, SetPlayerSlot{alex, 37, {items::iron_ingot, 3}},
                SetPlayerSlot{alex, 38, {items::iron_pickaxe, 1, 150}}, SetPlayerSlot{alex, 39, stack(items::diamond_pickaxe, {{{8, 1}}})},
                SetPlayerSlot{alex, 40, stack(items::enchanted_book, {{}, {{8, 1}}})}, SetPlayerSlot{alex, 41, {items::stone, 1}},
                ChangeExperience{alex, 30, true, true}});
        s.tick({click(30), click(31)});
        require(player().anvil[2] == stack(items::iron_pickaxe, {{}, {}, 1}, 14) && player().anvil_cost == 3,
                "three ingots each repair a quarter, a level each; the repair cost 1");
        s.tick({click(2)});
        require(player().anvil[0].empty() && player().anvil[1].empty() && player().slots[44] == stack(items::iron_pickaxe, {{}, {}, 1}, 14)
                    && player().xp_level == 27 && player().anvil_cost == 0,
                "taken for three levels; all three ingots used");
        s.tick({click(38), click(32)});
        require(player().anvil[2] == stack(items::iron_pickaxe, {{}, {}, 3}) && player().anvil_cost == 3,
                "two pickaxes: both durabilities and 12%, 2 levels and the prior 1; the repair cost 1 doubled and one more");
        s.tick({click(2), click(33), click(34)});
        require(player().xp_level == 24 && player().anvil[2] == stack(items::diamond_pickaxe, {{{8, 2}}, {}, 1}) && player().anvil_cost == 2,
                "Efficiency I and a book of it: Efficiency II, half its cost per level");
        s.tick({RenameItem{alex, "Digger", false}});
        require(player().anvil[2] == stack(items::diamond_pickaxe, {{{8, 2}}, {}, 1, "Digger"}) && player().anvil_cost == 3,
                "a name costs a level more");
        s.tick({click(2), click(35)});
        require(player().xp_level == 21 && player().anvil[2] == stack(items::stone, {{}, {}, 0, "Digger"}) && player().anvil_cost == 1,
                "the typed name stays for the next item; renaming alone keeps the repair cost");
        s.tick({click(0), RenameItem{alex, "Stone", false}, click(30)});
        require(player().anvil[2].empty() && player().anvil_cost == 0, "the name the stone shows already: nothing to do");
        s.tick({click(0)});

        // Conflicts: Silk Touch and Unbreaking onto a Fortune pickaxe.
        s.tick({SetPlayerSlot{alex, 9, stack(items::diamond_pickaxe, {{{13, 3}}})}, SetPlayerSlot{alex, 10, stack(items::enchanted_book, {{}, {{20, 1}}})},
                SetPlayerSlot{alex, 11, stack(items::enchanted_book, {{}, {{20, 1}, {34, 1}}})}, RenameItem{alex, "", false}});
        s.tick({click(3), click(4)});
        require(player().anvil[2].empty() && player().anvil_cost == 0, "Silk Touch alone conflicts with Fortune: no result");
        s.tick({click(1), click(5)});
        require(player().anvil[2] == stack(items::diamond_pickaxe, {{{13, 3}, {34, 1}}, {}, 1}) && player().anvil_cost == 2,
                "Unbreaking joins (1 level), the conflict costs another");
        s.tick({click(0), click(1)});

        // Too expensive: 39 prior levels and a repair; renaming alone stays at 39.
        s.tick({SetPlayerSlot{alex, 12, stack(items::iron_pickaxe, {{}, {}, 39}, 100)}, SetPlayerSlot{alex, 13, {items::iron_ingot, 1}}});
        s.tick({click(6), click(7)});
        require(player().anvil[0].item == items::iron_pickaxe && player().anvil[1].item == items::iron_ingot && player().anvil[2].empty()
                    && player().anvil_cost == 40,
                "40 levels is too expensive: no result, the cost shown");
        s.tick({click(2, 0, 1, true)});
        require(player().anvil[0].item == items::iron_pickaxe, "nothing to take");
        s.tick({click(1), RenameItem{alex, "Old", false}});
        require(player().anvil_cost == 39 && player().anvil[2] == stack(items::iron_pickaxe, {{}, {}, 39, "Old"}, 100),
                "renaming alone is capped at 39");
        s.tick({click(2)});
        require(player().anvil[0].item == items::iron_pickaxe && player().xp_level == 21, "21 levels are too few");
        s.tick({ChangeExperience{alex, 40, true, true}});
        s.tick({click(2)});
        require(player().anvil[0].empty() && player().xp_level == 1, "with 40 it is taken for 39");

        // Closing gives the inputs back and forgets the name.
        s.tick({RenameItem{alex, "Kept", false}, click(4)});
        require(!player().anvil[0].empty() && player().anvil_name == "Kept", "an input and a name");
        s.tick({CloseContainer{alex, at_block(0.5, 2, 0.5), {}}});
        require(player().anvil[0].empty() && player().anvil[2].empty() && player().anvil_name.empty() && player().anvil_cost == 0,
                "closed: the input back, the name gone");

        // Names are refused with control characters or the section sign, or
        // longer than 50 UTF-16 code units.
        const auto refused = [&](CommandBody body) {
            s.tick({std::move(body)});
            return of_kind(s.last, TraceKind::command_rejected).size() == 1;
        };
        require(!refused(RenameItem{alex, std::string(50, 'y'), false}) && refused(RenameItem{alex, std::string(51, 'y'), false})
                    && refused(RenameItem{alex, "a\xC2\xA7" "b", false}) && refused(RenameItem{alex, "a\nb", false})
                    && !refused(RenameItem{alex, "", false}),
                "name limits");

        // Each use wears the anvil with a chance: chipped, damaged, then gone.
        std::vector<std::string> worn;
        for (int use = 0; use < 400 && worn.size() < 3; ++use) {
            s.tick({ChangeExperience{alex, 10, true, true}, SetPlayerSlot{alex, 42, {items::stone, 1}}, SetPlayerSlot{alex, 44, {}},
                    RenameItem{alex, use % 2 == 0 ? "a" : "b", false}, click(36)});
            s.tick({click(2)});
            const auto here = state_at(s.oracle.dump(), anvil);
            const auto name = here.is_air() ? std::string("air")
                                            : std::string(registry::game_name(*registry::block_of(here.registry_state())));
            if (worn.empty() ? name != "anvil" : name != worn.back()) worn.push_back(name);
        }
        require(worn == std::vector<std::string>{"chipped_anvil", "damaged_anvil", "air"}, "worn down to nothing");
    });

    suite.run("enchanting tables offer and enchant by the bookshelves and the player's seed", [] {
        using circuit::contracts::ChangeExperience;
        using circuit::contracts::ComponentPatch;
        using circuit::contracts::ContainerButton;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::SetPlayerSlot;
        using Levels = std::vector<std::pair<std::uint32_t, std::int32_t>>;
        // Stand-ins for the server's encoding: type 1 the enchantments, 2 the
        // stored ones, one byte per id and level.
        const auto make = [](const Levels& plain, const Levels& stored) {
            std::vector<ComponentPatch::Added> added;
            for (const auto& [type, list] : {std::pair{std::uint16_t{1}, plain}, std::pair{std::uint16_t{2}, stored}}) {
                if (list.empty()) continue;
                std::vector<std::byte> bytes;
                for (const auto& [id, level] : list) {
                    bytes.push_back(static_cast<std::byte>(id));
                    bytes.push_back(static_cast<std::byte>(level));
                }
                added.push_back({type, bytes});
            }
            return added.empty() ? ComponentPatch{} : ComponentPatch::make(772, std::move(added), {}, {});
        };
        const auto read = [](const ComponentPatch& p, bool stored) {
            Levels out;
            for (const auto& a : p.added())
                if (a.type == (stored ? 2 : 1))
                    for (std::size_t k = 0; k + 1 < a.value.size(); k += 2)
                        out.emplace_back(static_cast<std::uint32_t>(a.value[k]), static_cast<std::int32_t>(a.value[k + 1]));
            return out;
        };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->enchantments_of = read;
        data->with_enchantments = [&](const ComponentPatch& base, bool stored, const Levels& levels, bool, std::int32_t) {
            return std::optional(stored ? make(read(base, false), levels) : make(levels, read(base, true)));
        };
        data->repair_cost_of = [](const ComponentPatch&) { return 0; };
        // Sharpness (32) and Smite (34) exclude each other; Knockback (17);
        // Efficiency (8) on pickaxes; Unbreaking (39) on both.
        using Offered = circuit::gameplay::GameData::TableEnchantment;
        std::vector<std::uint16_t> sword{items::diamond_sword}, pickaxe{items::iron_pickaxe}, both{items::diamond_sword, items::iron_pickaxe};
        std::ranges::sort(both);
        data->table_enchantments = {Offered{32, 10, 5, {1, 11}, {21, 11}, sword}, Offered{34, 5, 5, {5, 8}, {25, 8}, sword},
                                    Offered{17, 5, 2, {5, 20}, {55, 20}, sword}, Offered{8, 10, 5, {1, 10}, {51, 10}, pickaxe},
                                    Offered{39, 5, 3, {5, 8}, {55, 8}, both}};
        data->enchantment_rules[32] = {1, 5, {}, {34}};
        data->enchantment_rules[34] = {1, 5, {}, {32}};
        data->power_providers = {*registry::find_block("1.21.8", "bookshelf")};
        data->power_transmitters = {*registry::find_block("1.21.8", "air")};
        const PlayerKey alex{4, 2}; // enchantment seed 405050550
        const BlockPos table{0, 0, 0};
        // Enchanting table window: 0 the item, 1 the lapis, 2-28 the main inventory, 29-37 the hotbar.
        const auto click = [&](std::int16_t slot) {
            return ContainerClick{alex, std::optional(table), slot, 0, 1, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto offers = [](const PlayerInventory& p) {
            return std::tuple{p.enchant_costs, p.enchant_clues, p.enchant_clue_levels};
        };
        using Offers = std::tuple<std::array<std::int32_t, 3>, std::array<std::int32_t, 3>, std::array<std::int32_t, 3>>;
        // The sixteen cells two blocks away, fifteen of them bookshelves.
        std::vector<CommandBody> shelves;
        for (std::int32_t dx = -2; dx <= 2; ++dx)
            for (std::int32_t dz = -2; dz <= 2; ++dz)
                if ((std::abs(dx) == 2 || std::abs(dz) == 2) && !(dx == 2 && dz == 2)) shelves.push_back(place({dx, 0, dz}, registry_block("bookshelf")));
        {
            Sim s(bundle(), 7, 2, data);
            const auto player = [&] {
                const auto d = s.oracle.dump();
                return *std::ranges::find(d.players, alex, &PlayerInventory::player);
            };
            s.tick({place(table, registry_block("enchanting_table")), SetPlayerSlot{alex, 36, {items::diamond_sword, 1}},
                    SetPlayerSlot{alex, 37, {items::lapis_lazuli, 2}}, SetPlayerSlot{alex, 38, {items::lapis_lazuli, 64}},
                    SetPlayerSlot{alex, 39, {items::stone, 3}}});
            s.tick({click(29)});
            require(offers(player()) == Offers{{1, 5, 4}, {32, 39, 32}, {1, 1, 1}} && player().enchant[0].item == items::diamond_sword,
                    "no bookshelves: the offers and their clues");
            s.tick({click(0)});
            require(player().slots[44].item == items::diamond_sword, "the item goes back to the hotbar's end");
            s.tick(shelves);
            s.tick({click(37), click(30), click(32)});
            require(offers(player()) == Offers{{4, 19, 30}, {32, 39, 39}, {1, 3, 3}} && player().enchant[1].count == 2 && player().slots[39].count == 3,
                    "fifteen bookshelves (the sword back from the hotbar's end); lapis goes into its slot, stone nowhere");
            s.tick({ChangeExperience{alex, 2, true, true}, ContainerButton{alex, table, 2}});
            require(player().enchant[0] == ItemStack{items::diamond_sword, 1} && player().xp_level == 2, "two levels are too few");
            s.tick({ChangeExperience{alex, 30, true, true}, ContainerButton{alex, table, 2}});
            require(player().enchant[0] == ItemStack{items::diamond_sword, 1}, "two lapis are too few for the third offer");
            s.tick({click(31)});
            require(player().enchant[1].count == 64 && player().slots[38].count == 2, "the lapis slot fills up");
            s.tick({ContainerButton{alex, table, 2}});
            require(player().enchant[0] == ItemStack{items::diamond_sword, 1, 0, make({{32, 3}, {39, 3}, {17, 2}}, {})}
                        && player().enchant[1].count == 61 && player().xp_level == 27 && offers(player()) == Offers{{0, 0, 0}, {-1, -1, -1}, {-1, -1, -1}}
                        && player().xp_seed != 0 && player().xp_seed != 405050550,
                    "enchanted for three levels and three lapis; the seed changes, and an enchanted sword is offered nothing");
            s.tick({click(0), click(1)});
            require(player().enchant[0].empty() && player().enchant[1].empty() && player().slots[44].item == items::diamond_sword
                        && player().slots[38].count == 63,
                    "the item goes back to the hotbar's end, the lapis onto the lapis left there");
        }
        {
            Sim s(bundle(), 7, 2, data);
            const auto player = [&] {
                const auto d = s.oracle.dump();
                return *std::ranges::find(d.players, alex, &PlayerInventory::player);
            };
            s.tick({place(table, registry_block("enchanting_table")), SetPlayerSlot{alex, 36, {items::book, 5}},
                    SetPlayerSlot{alex, 37, {items::lapis_lazuli, 64}}, ChangeExperience{alex, 30, true, true}});
            s.tick(shelves);
            s.tick({click(29), click(30)});
            require(offers(player()) == Offers{{4, 19, 30}, {8, 34, 39}, {1, 3, 3}} && player().enchant[0] == ItemStack{items::book, 1}
                        && player().slots[36].count == 4,
                    "a book: one of the five, offered any enchantment");
            // A cell halfway to a bookshelf that is not air blocks it: stone
            // there blocks the three in the middle of the row behind.
            s.tick({click(0), place({0, 0, -1}, blocks::stone())});
            s.tick({click(29)});
            require(offers(player()) == Offers{{7, 13, 24}, {8, 34, 8}, {1, 2, 3}}, "twelve bookshelves (the book back onto the others)");
            s.tick({click(0), place({0, 0, -1}, blocks::air())});
            s.tick({click(29), ContainerButton{alex, table, 2}});
            require(player().enchant[0] == ItemStack{items::enchanted_book, 1, 0, make({}, {{8, 4}, {39, 3}})} && player().xp_level == 27,
                    "an enchanted book, which kept all but one");
        }
    });

    suite.run("lecterns take a book, turn its pages with a pulse and give it back", [] {
        using circuit::contracts::ComponentPatch;
        using circuit::contracts::ContainerButton;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::PlayerBreak;
        using circuit::contracts::PlayerUse;
        using circuit::contracts::SetPlayerSlot;
        // Stand-ins for the server's encoding: component 1 holds the page count.
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->lectern_books = {items::writable_book, items::written_book};
        std::ranges::sort(data->lectern_books);
        data->book_pages = [](const ItemStack& s) {
            for (const auto& a : s.components.added())
                if (a.type == 1 && !a.value.empty()) return static_cast<std::int32_t>(a.value[0]);
            return 0;
        };
        const auto book = [](std::uint8_t pages) {
            return ItemStack{items::writable_book, 1, 0, ComponentPatch::make(772, {{1, {static_cast<std::byte>(pages)}}}, {}, {})};
        };
        const PlayerKey alex{4, 2};
        const BlockPos lectern{0, 1, 0};
        Sim s(bundle(), 7, 2, data);
        // A lectern on the floor, a comparator behind it reading it and dust after it.
        std::vector<CommandBody> build;
        for (std::int32_t x = -1; x <= 1; ++x)
            for (std::int32_t z = -1; z <= 3; ++z) build.push_back(place({x, 0, z}, blocks::stone()));
        build.push_back(place(lectern, blocks::lectern(Direction::north)));
        build.push_back(place({0, 1, 1}, blocks::comparator(Direction::south)));
        build.push_back(place({0, 1, 2}, blocks::wire()));
        s.tick(build);
        const auto player = [&] {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, alex, &PlayerInventory::player);
        };
        const auto page = [&] {
            const auto d = s.oracle.dump();
            const auto* be = block_entity_at(d, lectern);
            return be && !be->slots.empty() ? be->cook_time : -1;
        };
        const auto dust = [&] { return s.at({0, 1, 2}).level(); };
        const auto use = [&](std::uint8_t slot) { return PlayerUse{alex, slot, lectern, Direction::up, Direction::south, Direction::south, false, true, 3, 3}; };
        s.tick({SetPlayerSlot{alex, 36, {items::book, 1}}, SetPlayerSlot{alex, 37, book(3)}});
        s.tick({use(36)});
        require(s.at(lectern) == blocks::lectern(Direction::north), "a plain book does not go on");
        s.tick({use(37)});
        require(s.at(lectern) == blocks::lectern(Direction::north, true) && player().slots[37].empty() && page() == 0, "the book on, used up");
        s.tick();
        s.tick();
        require(dust() == 1, "the comparator reads the first of three pages");
        s.tick({ContainerButton{alex, lectern, 2}});
        require(s.at(lectern) == blocks::lectern(Direction::north, true, true) && page() == 1, "the next page, a pulse");
        s.tick();
        require(s.at(lectern) == blocks::lectern(Direction::north, true) && dust() == 8, "the pulse ends; the second page");
        s.tick({ContainerButton{alex, lectern, 108}});
        s.tick();
        require(page() == 2 && dust() == 15, "a page beyond the book: its last");
        s.tick();
        s.tick({ContainerButton{alex, lectern, 2}});
        require(page() == 2 && !s.at(lectern).flag(), "no page after the last: no pulse");
        s.tick({ContainerButton{alex, lectern, 3}});
        require(s.at(lectern) == blocks::lectern(Direction::north) && player().slots[36].item == items::book && player().slots[37] == book(3),
                "the book taken into the inventory");
        s.tick();
        s.tick();
        require(dust() == 0, "nothing to read");
        // A click on the window's slot takes the book onto the cursor.
        s.tick({use(37)});
        s.tick({ContainerClick{alex, lectern, 0, 0, 0, false, at_block(0.5, 2, 0.5), {}}});
        require(player().cursor == book(3) && s.at(lectern) == blocks::lectern(Direction::north), "clicked off");
        // Broken, the lectern's book pops out on top, a quarter of a block toward its front.
        s.tick({SetPlayerSlot{alex, 38, book(1)}, use(38)});
        s.tick({PlayerBreak{alex, lectern, true}});
        const auto d = s.oracle.dump();
        const auto popped = std::ranges::find_if(d.overworld().entities, [&](const Entity& e) { return e.kind == EntityKind::item && e.stack == book(1); });
        // (It has made its first move, of at most 0.1 to each side.)
        require(s.at(lectern).is_air() && popped != d.overworld().entities.end() && std::abs(popped->position.x - 0.5) <= 0.1
                    && std::abs(popped->position.z - 0.25) <= 0.1 && popped->position.y > 2.0,
                "the book on top, toward the front");
    });

    suite.run("chests count their viewers; trapped chests give the count as power", [] {
        using circuit::contracts::CloseContainer;
        using circuit::contracts::PlayerDeath;
        using circuit::contracts::PlayerPresence;
        using circuit::contracts::PlayerUse;
        const PlayerKey alex{4, 2}, bob{4, 3};
        const BlockPos chest{0, 1, 0};
        Sim s(bundle(), 7, 2);
        // A trapped chest on a stone block, dust beside it and beside the stone.
        std::vector<CommandBody> build;
        for (std::int32_t x = -2; x <= 3; ++x)
            for (std::int32_t z = -1; z <= 1; ++z) build.push_back(place({x, -1, z}, blocks::stone()));
        build.push_back(place({0, 0, 0}, blocks::stone()));
        build.push_back(place({-1, 0, 0}, blocks::stone()));
        build.push_back(place(chest, blocks::chest(Direction::north, blocks::ChestType::single, true)));
        build.push_back(place({-1, 1, 0}, blocks::wire()));
        build.push_back(place({1, 0, 0}, blocks::wire()));
        s.tick(build);
        s.tick({PlayerPresence{alex, at_block(0.5, 1, 2.5)}, PlayerPresence{bob, at_block(1.5, 1, 2.5)}});
        const auto beside = [&] { return std::pair{s.at({-1, 1, 0}).level(), s.at({1, 0, 0}).level()}; };
        const auto use = [&](PlayerKey who) { return PlayerUse{who, 36, chest, Direction::up, Direction::south, Direction::south, false, true, 3, 3}; };
        const auto close = [&](PlayerKey who, BlockPos at) { return CloseContainer{who, at_block(0.5, 2, 2.5), {}, false, at}; };
        const auto lids = [&] {
            std::vector<std::uint64_t> counts;
            for (const auto& r : of_kind(s.last, TraceKind::block_event_queued))
                if (r.pos == chest) counts.push_back(r.c);
            return counts;
        };
        const auto viewing = [&](PlayerKey who) {
            const auto d = s.oracle.dump();
            const auto it = std::ranges::find(d.players, who, &PlayerInventory::player);
            return it == d.players.end() ? std::nullopt : it->viewing;
        };
        s.tick({use(alex)});
        require(beside() == std::pair<std::uint8_t, std::uint8_t>{1, 1} && lids() == std::vector<std::uint64_t>{1} && viewing(alex) == chest,
                "one viewer: power 1 beside it and through the block below");
        s.tick({use(bob)});
        require(beside() == std::pair<std::uint8_t, std::uint8_t>{2, 2} && lids() == std::vector<std::uint64_t>{2}, "two viewers");
        // Rechecks every 5 ticks queue the lid again with the same count.
        std::size_t rechecks = 0;
        for (int i = 0; i < 10; ++i) {
            s.tick();
            rechecks += lids().size();
        }
        require(rechecks == 2 && beside() == std::pair<std::uint8_t, std::uint8_t>{2, 2}, "rechecked twice in 10 ticks, both still near");
        s.tick({close(alex, {5, 1, 5})});
        require(beside() == std::pair<std::uint8_t, std::uint8_t>{2, 2} && viewing(alex) == chest, "a close naming another block");
        s.tick({close(alex, chest)});
        require(beside() == std::pair<std::uint8_t, std::uint8_t>{1, 1} && lids() == std::vector<std::uint64_t>{1} && !viewing(alex),
                "alex closes it");
        // Bob walks 8 blocks away and stays a viewer; gone from the level, the
        // next recheck drops him.
        s.tick({PlayerPresence{bob, at_block(1.5, 1, 8.5)}});
        for (int i = 0; i < 5; ++i) s.tick();
        require(beside() == std::pair<std::uint8_t, std::uint8_t>{1, 1}, "a viewer farther away stays one");
        s.tick({PlayerPresence{bob, {}, false, false}});
        for (int i = 0; i < 5; ++i) s.tick();
        require(beside() == std::pair<std::uint8_t, std::uint8_t>{0, 0}, "a viewer gone from the level is dropped");
        // A player who dies stops viewing.
        s.tick({use(alex)});
        require(beside().first == 1, "alex again");
        s.tick({PlayerDeath{alex, at_block(0.5, 1, 2.5)}});
        require(beside() == std::pair<std::uint8_t, std::uint8_t>{0, 0} && !viewing(alex), "and dies");
    });

    suite.run("barrels are open from their first viewer to their last", [] {
        using circuit::contracts::CloseContainer;
        using circuit::contracts::PlayerPresence;
        using circuit::contracts::PlayerUse;
        const PlayerKey alex{4, 2}, bob{4, 3};
        const BlockPos barrel{0, 0, 0};
        Sim s(bundle(), 7, 2);
        // A barrel under a stone (which does not keep it from opening), and
        // one set open.
        s.tick({place(barrel, blocks::barrel(Direction::north)), place({0, 1, 0}, blocks::stone()),
                place({3, 0, 0}, blocks::barrel(Direction::up, true))});
        s.tick({PlayerPresence{alex, at_block(0.5, 0, 2.5)}, PlayerPresence{bob, at_block(1.5, 0, 2.5)}});
        const auto use = [&](PlayerKey who) { return PlayerUse{who, 36, barrel, Direction::south, Direction::north, Direction::north, false, true, 3, 3}; };
        const auto close = [&](PlayerKey who) { return CloseContainer{who, at_block(0.5, 1, 2.5), {}, false, barrel}; };
        s.tick({use(alex)});
        require(s.at(barrel) == blocks::barrel(Direction::north, true) && of_kind(s.last, TraceKind::container_opened).size() == 1
                    && of_kind(s.last, TraceKind::block_event_queued).empty(),
                "opened, with a block above it; no block event");
        s.tick({use(bob)});
        for (int i = 0; i < 10; ++i) s.tick();
        s.tick({close(alex)});
        require(s.at(barrel).flag(), "open while bob views it");
        s.tick({close(bob)});
        require(s.at(barrel) == blocks::barrel(Direction::north), "closed with the last viewer");
        for (int i = 0; i < 10; ++i) s.tick();
        require(s.at({3, 0, 0}) == blocks::barrel(Direction::up, true), "a barrel set open stays so");
        // Gone from the level, a viewer is dropped at the next recheck.
        s.tick({use(alex)});
        s.tick({PlayerPresence{alex, {}, false, false}});
        for (int i = 0; i < 5; ++i) s.tick();
        require(s.at(barrel) == blocks::barrel(Direction::north), "the recheck closes it");
    });

    suite.run("composters fill by their items' chances and give bone meal when ready", [] {
        using circuit::contracts::PlayerUse;
        using circuit::contracts::SetPlayerSlot;
        const PlayerKey alex{4, 2};
        const BlockPos composter{0, 1, 0};
        Sim s(bundle(), 7, 2);
        // A composter on stone with a comparator behind it and dust after it.
        std::vector<CommandBody> build;
        for (std::int32_t z = 0; z <= 2; ++z) build.push_back(place({0, 0, z}, blocks::stone()));
        build.push_back(place(composter, blocks::composter()));
        build.push_back(place({0, 1, 1}, blocks::comparator(Direction::south)));
        build.push_back(place({0, 1, 2}, blocks::wire()));
        s.tick(build);
        const auto pie = items::by_name("minecraft:pumpkin_pie").value();
        const auto use = [&] { return PlayerUse{alex, 36, composter, Direction::up, Direction::south, Direction::south, false, true, 3, 3}; };
        const auto held = [&] {
            const auto d = s.oracle.dump();
            return std::ranges::find(d.players, alex, &PlayerInventory::player)->slots[36];
        };
        s.tick({SetPlayerSlot{alex, 36, {items::wheat_seeds, 2}}});
        s.tick({use()});
        require(s.at(composter) == blocks::composter(1) && held() == ItemStack{items::wheat_seeds, 1}
                    && of_kind(s.last, TraceKind::level_event).size() == 1 && of_kind(s.last, TraceKind::level_random_draw).empty(),
                "the first item always adds a layer, drawing nothing");
        s.tick({SetPlayerSlot{alex, 36, {pie, 8}}});
        for (int i = 0; i < 6; ++i) s.tick({use()});
        require(s.at(composter) == blocks::composter(7) && held() == ItemStack{pie, 2}, "pies fill it");
        s.tick({use()});
        require(held() == ItemStack{pie, 2}, "a full composter takes nothing");
        for (int i = 0; i < 20; ++i) s.tick();
        require(s.at(composter) == blocks::composter(8) && s.at({0, 1, 2}).level() == 8, "ready 20 ticks after; the comparator reads 8");
        s.tick({use()});
        const auto d = s.oracle.dump();
        const auto meal = std::ranges::find_if(d.overworld().entities, [](const Entity& e) { return e.stack.item == items::bone_meal; });
        require(s.at(composter) == blocks::composter() && meal != d.overworld().entities.end() && meal->position.y > 1.65,
                "a use pops the bone meal out on top");
        // A hopper below takes the bone meal of a ready composter; a dropper
        // below does not put items in.
        s.tick({place({2, 1, 0}, blocks::composter(8)), place({2, 0, 0}, blocks::hopper(Direction::down))});
        s.tick();
        const auto after = s.oracle.dump();
        require(s.at({2, 1, 0}) == blocks::composter() && block_entity_at(after, {2, 0, 0})->slots[0] == ItemStack{items::bone_meal, 1},
                "the hopper below takes the bone meal");
    });

    suite.run("ender chests show each player their own ender inventory", [] {
        using circuit::contracts::CloseContainer;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::PlayerPresence;
        using circuit::contracts::PlayerUse;
        using circuit::contracts::SetPlayerSlot;
        const PlayerKey alex{4, 2}, bob{4, 3};
        const BlockPos one{0, 1, 0}, two{3, 1, 0}, covered{6, 1, 0};
        Sim s(bundle(), 7, 2);
        std::vector<CommandBody> build;
        for (std::int32_t x = 0; x <= 6; ++x) build.push_back(place({x, 0, 0}, blocks::stone()));
        build.push_back(place(one, blocks::ender_chest(Direction::south)));
        build.push_back(place(two, blocks::ender_chest(Direction::south)));
        build.push_back(place(covered, blocks::ender_chest(Direction::south)));
        build.push_back(place({6, 2, 0}, blocks::stone()));
        s.tick(build);
        s.tick({PlayerPresence{alex, at_block(0.5, 1, 2.5)}, PlayerPresence{bob, at_block(1.5, 1, 2.5)},
                SetPlayerSlot{alex, 36, {items::iron_ingot, 64}}});
        const auto use = [&](PlayerKey who, BlockPos at) { return PlayerUse{who, 37, at, Direction::up, Direction::north, Direction::north, false, true, 3, 3}; };
        const auto click = [&](PlayerKey who, BlockPos at, std::int16_t slot) {
            return ContainerClick{who, std::optional(at), slot, 0, 1, false, at_block(0.5, 2, 2.5), {}};
        };
        const auto player = [&](PlayerKey who) {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, who, &PlayerInventory::player);
        };
        const auto lids = [&](BlockPos at) {
            std::vector<std::uint64_t> counts;
            for (const auto& r : of_kind(s.last, TraceKind::block_event_queued))
                if (r.pos == at) counts.push_back(r.c);
            return counts;
        };
        s.tick({use(alex, one)});
        require(lids(one) == std::vector<std::uint64_t>{1} && of_kind(s.last, TraceKind::container_opened).size() == 1, "opened: its lid");
        // A shift-click of the hotbar's first slot (window slot 27 + 27) moves the iron in.
        s.tick({click(alex, one, 54)});
        require(player(alex).ender[0] == ItemStack{items::iron_ingot, 64} && player(alex).slots[36].empty(), "into alex's ender inventory");
        s.tick({use(bob, one)});
        require(lids(one) == std::vector<std::uint64_t>{2} && player(bob).ender[0].empty(), "bob opens it too: his own, empty");
        s.tick({CloseContainer{alex, at_block(0.5, 2, 2.5), {}, false, one}, CloseContainer{bob, at_block(1.5, 2, 2.5), {}, false, one}});
        s.tick({use(alex, two)});
        s.tick({click(alex, two, 0)});
        require(player(alex).ender[0].empty() && player(alex).slots[44] == ItemStack{items::iron_ingot, 64},
                "the other ender chest holds the same iron; shift-clicked out to the hotbar's end");
        s.tick({use(alex, covered)});
        require(of_kind(s.last, TraceKind::container_opened).empty() && lids(covered).empty(), "one under a stone does not open");
    });

    suite.run("shulker boxes open when their lid can rise and keep their contents in their item", [] {
        using circuit::contracts::ComponentPatch;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::PlayerBreak;
        using circuit::contracts::PlayerUse;
        using circuit::contracts::SetPlayerSlot;
        // Stand-ins for the server's encoding: component 1 holds the slots,
        // each as an item id (2 bytes) and a count.
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->container_of = [](const ComponentPatch& p) {
            std::vector<ItemStack> out;
            for (const auto& a : p.added())
                if (a.type == 1)
                    for (std::size_t k = 0; k + 2 < a.value.size(); k += 3)
                        out.push_back({static_cast<std::uint16_t>(std::to_integer<int>(a.value[k]) | std::to_integer<int>(a.value[k + 1]) << 8),
                                       static_cast<std::uint8_t>(a.value[k + 2])});
            return out;
        };
        data->with_container = [](const ComponentPatch&, std::span<const ItemStack> stacks) -> std::optional<ComponentPatch> {
            std::vector<std::byte> bytes;
            for (const auto& st : stacks) {
                bytes.push_back(static_cast<std::byte>(st.item & 0xFF));
                bytes.push_back(static_cast<std::byte>(st.item >> 8));
                bytes.push_back(static_cast<std::byte>(st.count));
            }
            if (std::ranges::all_of(stacks, [](const ItemStack& st) { return st.empty(); })) return ComponentPatch{};
            return ComponentPatch::make(772, {{1, std::move(bytes)}}, {}, {});
        };
        const PlayerKey alex{4, 2};
        const BlockPos open_box{0, 1, 0}, covered{2, 1, 0}, sideways{4, 1, 0};
        Sim s(bundle(), 7, 2, data);
        std::vector<CommandBody> build;
        for (std::int32_t x = 0; x <= 5; ++x) build.push_back(place({x, 0, 0}, blocks::stone()));
        build.push_back(place(open_box, blocks::shulker_box(Direction::up)));
        build.push_back(place(covered, blocks::shulker_box(Direction::up, 15)));
        build.push_back(place({2, 2, 0}, blocks::stone()));
        build.push_back(place(sideways, blocks::shulker_box(Direction::north)));
        build.push_back(place({4, 2, 0}, blocks::stone()));
        s.tick(build);
        const auto box = items::by_name("minecraft:shulker_box").value();
        s.tick({SetPlayerSlot{alex, 36, {items::iron_ingot, 64}}, SetPlayerSlot{alex, 37, {items::by_name("minecraft:blue_shulker_box").value(), 1}}});
        const auto use = [&](BlockPos at) { return PlayerUse{alex, 38, at, Direction::up, Direction::north, Direction::north, false, true, 3, 3}; };
        const auto lids = [&](BlockPos at) {
            std::vector<std::uint64_t> counts;
            for (const auto& r : of_kind(s.last, TraceKind::block_event_queued))
                if (r.pos == at) counts.push_back(r.c);
            return counts;
        };
        s.tick({use(covered)});
        require(of_kind(s.last, TraceKind::container_opened).empty(), "a stone over its lid keeps it shut");
        s.tick({use(sideways)});
        require(of_kind(s.last, TraceKind::container_opened).size() == 1 && lids(sideways) == std::vector<std::uint64_t>{1},
                "a box facing north opens with a stone above it");
        s.tick({use(open_box)});
        // Shift-clicks: the iron goes in, the blue shulker box does not.
        s.tick({ContainerClick{alex, std::optional(open_box), 54, 0, 1, false, at_block(0.5, 2, 2.5), {}},
                ContainerClick{alex, std::optional(open_box), 55, 0, 1, false, at_block(0.5, 2, 2.5), {}}});
        const auto d = s.oracle.dump();
        const auto* be = block_entity_at(d, open_box);
        require(be && be->slots[0] == ItemStack{items::iron_ingot, 64} && be->slots[1].empty(), "iron in, no shulker box");
        for (int i = 0; i < 10; ++i) s.tick();
        require(lids(open_box).empty(), "no rechecks");
        // Broken, it drops one item that keeps the iron; placed, it gives it back.
        s.tick({PlayerBreak{alex, open_box, true}});
        const auto after = s.oracle.dump();
        const auto dropped = std::ranges::find_if(after.overworld().entities, [&](const Entity& e) { return e.stack.item == box; });
        require(dropped != after.overworld().entities.end() && dropped->stack.count == 1 && !dropped->stack.components.empty()
                    && std::ranges::none_of(after.overworld().entities, [](const Entity& e) { return e.stack.item == items::iron_ingot; }),
                "one box item with the iron in it, nothing scattered");
        s.tick({SetPlayerSlot{alex, 39, dropped->stack}});
        s.tick({PlayerUse{alex, 39, {1, 0, 0}, Direction::up, Direction::north, Direction::north, false, true, 3, 3}});
        const auto again = s.oracle.dump();
        const auto* refilled = block_entity_at(again, {1, 1, 0});
        require(refilled && refilled->kind == BlockType::shulker_box && refilled->slots[0] == ItemStack{items::iron_ingot, 64},
                "placed again with its iron");
    });

    suite.run("chiseled bookshelves take books by the slot clicked and give them back", [] {
        using circuit::contracts::PlayerUse;
        using circuit::contracts::SetPlayerSlot;
        const PlayerKey alex{4, 2};
        const BlockPos shelf{0, 1, 0};
        Sim s(bundle(), 7, 2);
        // A bookshelf facing south on stone, a comparator behind it and dust after it.
        std::vector<CommandBody> build;
        for (std::int32_t z = -2; z <= 0; ++z) build.push_back(place({0, 0, z}, blocks::stone()));
        build.push_back(place(shelf, blocks::chiseled_bookshelf(Direction::south)));
        build.push_back(place({0, 1, -1}, blocks::comparator(Direction::north)));
        build.push_back(place({0, 1, -2}, blocks::wire()));
        s.tick(build);
        const auto click = [&](float x, float y) {
            PlayerUse u{alex, 36, shelf, Direction::south, Direction::north, Direction::north, false, true, 3, 3};
            u.cursor_x = x;
            u.cursor_y = y;
            u.cursor_z = 1.0f;
            return u;
        };
        const auto held = [&] {
            const auto d = s.oracle.dump();
            return std::ranges::find(d.players, alex, &PlayerInventory::player)->slots[36];
        };
        s.tick({SetPlayerSlot{alex, 36, {items::book, 3}}});
        s.tick({click(0.83f, 0.75f)});
        s.tick();
        s.tick();
        require(s.at(shelf) == blocks::chiseled_bookshelf(Direction::south, 0b000100) && held() == ItemStack{items::book, 2} && s.at({0, 1, -2}).level() == 3,
                "upper right (slot 2): one book in, the comparator at 3");
        s.tick({click(0.17f, 0.25f)});
        require(s.at(shelf) == blocks::chiseled_bookshelf(Direction::south, 0b001100), "lower left (slot 3)");
        // A book on a full slot takes its book back into the hand.
        s.tick({click(0.83f, 0.75f)});
        require(s.at(shelf) == blocks::chiseled_bookshelf(Direction::south, 0b001000) && held() == ItemStack{items::book, 2}, "taken back");
        // A stick on an empty slot does nothing of the bookshelf's.
        s.tick({SetPlayerSlot{alex, 36, {items::stick, 1}}, click(0.5f, 0.75f)});
        require(s.at(shelf) == blocks::chiseled_bookshelf(Direction::south, 0b001000), "nothing for a stick");
    });

    suite.run("experience changes as the /xp command makes them", [] {
        using circuit::contracts::ChangeExperience;
        const PlayerKey alex{4, 2};
        Sim s(bundle(), 7, 2);
        const auto player = [&] {
            const auto d = s.oracle.dump();
            return *std::ranges::find(d.players, alex, &PlayerInventory::player);
        };
        const auto xp = [&] {
            const auto p = player();
            return std::tuple{p.xp_level, p.xp_progress, p.xp_total};
        };
        // The official servers' levels, progress and totals, step by step.
        s.tick({ChangeExperience{alex, 1, true, true}});
        s.tick({ChangeExperience{alex, 10, false, false}});
        require(xp() == std::tuple{2, 0.09090913087129593f, 10}, "10 points from level 1");
        s.tick({ChangeExperience{alex, -20, false, false}});
        require(xp() == std::tuple{0, 0.0f, 0}, "20 taken: down to nothing");
        s.tick({ChangeExperience{alex, 3, true, false}, ChangeExperience{alex, -100, true, false}});
        require(xp() == std::tuple{0, 0.0f, 0}, "levels below 0");
        s.tick({ChangeExperience{alex, 30, false, false}});
        require(xp() == std::tuple{3, 0.23076921701431274f, 30}, "30 points");
        s.tick({ChangeExperience{alex, 5, false, true}, ChangeExperience{alex, 3, true, true}});
        require(xp() == std::tuple{3, 0.38461539149284363f, 30}, "5 points set, then the same level");
        const auto refused = [&](CommandBody body) {
            s.tick({std::move(body)});
            return of_kind(s.last, TraceKind::command_rejected).size() == 1;
        };
        require(refused(ChangeExperience{alex, 13, false, true}) && refused(ChangeExperience{alex, -1, true, true})
                    && !refused(ChangeExperience{alex, 12, false, true}) && xp() == std::tuple{3, 12.0f / 13.0f, 30},
                "points beyond the level's need, or a level below 0, are refused");
        s.tick({ChangeExperience{alex, 2147483647, false, false}});
        require(xp() == std::tuple{21863, 0.3686237931251526f, 0}, "the largest int: the total wraps to 0");
    });

    suite.run("stonecutters cut by the chosen recipe until the input runs out", [] {
        namespace recipes = circuit::gameplay::recipes;
        using circuit::contracts::CloseContainer;
        using circuit::contracts::ContainerButton;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::SetPlayerSlot;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        const recipes::Ingredient stone{{items::stone}};
        data->stonecutting = {
            {"minecraft:stone_bricks_from_stone_stonecutting", stone, {item("minecraft:stone_bricks"), 1}},
            {"minecraft:stone_slab_from_stone_stonecutting", stone, {item("minecraft:stone_slab"), 2}},
        };
        require(recipes::cuttings(data->stonecutting, items::stone).size() == 2 && recipes::cuttings(data->stonecutting, items::dirt).empty(),
                "the recipes that cut an item");
        const PlayerKey alex{4, 2};
        const BlockPos cutter{0, 0, 0};
        Sim s(bundle(), 7, 2, data);
        s.tick({place(cutter, registry_block("stonecutter"))});
        // Stonecutter window: 0 the input, 1 the result, 2-28 the main inventory, 29-37 the hotbar.
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::optional(cutter), slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto press = [&](std::int32_t button) { return ContainerButton{alex, std::optional(cutter), button}; };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            const auto it = std::ranges::find(d.players, alex, &PlayerInventory::player);
            if (it != d.players.end()) return *it;
            PlayerInventory none;
            none.player = alex;
            return none;
        };
        s.tick({SetPlayerSlot{alex, 36, {items::stone, 3}}, SetPlayerSlot{alex, 37, {items::oak_planks, 5}}});
        s.tick({click(29, 0, 1), click(30, 0, 1)});
        require(player().cutter[0] == ItemStack{items::stone, 3} && player().slots[9] == ItemStack{items::oak_planks, 5},
                "stone into the input, planks (uncut) into the main inventory");
        s.tick({press(2)});
        require(player().cutter_recipe == -1 && player().cutter[1].empty(), "a button beyond the list does nothing");
        s.tick({press(1)});
        require(player().cutter_recipe == 1 && player().cutter[1] == ItemStack{item("minecraft:stone_slab"), 2}, "the second recipe");
        s.tick({click(1, 0, 0)});
        require(player().cursor == ItemStack{item("minecraft:stone_slab"), 2} && player().cutter[0].count == 2 &&
                    player().cutter[1] == ItemStack{item("minecraft:stone_slab"), 2},
                "taken: one stone used, the result made again");
        s.tick({click(1, 0, 1)});
        require(player().cutter[0].empty() && player().cutter[1].empty() && player().cutter_recipe == -1 &&
                    player().slots[44] == ItemStack{item("minecraft:stone_slab"), 4},
                "shift-clicked until the input ran out, into the hotbar from its end");
        // Another item in the input clears the choice; closing gives it back.
        s.tick({SetPlayerSlot{alex, 36, {items::stone, 2}}});
        s.tick({click(29, 0, 1), press(0)});
        require(player().cutter[1] == ItemStack{item("minecraft:stone_bricks"), 1}, "bricks");
        s.tick({click(2, 0, 0), click(0, 0, 0)});
        require(player().cutter[0] == ItemStack{items::oak_planks, 5} && player().cutter_recipe == -1 && player().cutter[1].empty() &&
                    player().cursor == ItemStack{items::stone, 2},
                "planks swapped in: no recipe");
        s.tick({click(29, 0, 0), CloseContainer{alex, at_block(0.5, 2, 0.5), {}}});
        require(player().cutter[0].empty() && player().slots[36] == ItemStack{items::stone, 2} && player().slots[37] == ItemStack{items::oak_planks, 5},
                "closing gives the input back, to the first empty hotbar slot");
    });

    suite.run("crafting tables craft 3x3 recipes and give their grid back, or drop it", [] {
        namespace recipes = circuit::gameplay::recipes;
        using circuit::contracts::CloseContainer;
        using circuit::contracts::ContainerClick;
        using circuit::contracts::SetPlayerSlot;
        using recipes::Ingredient;
        using recipes::Recipe;
        const auto item = [](std::string_view n) { return *circuit::gameplay::items::by_name(n); };
        auto data = std::make_shared<circuit::gameplay::GameData>();
        const Ingredient log{{items::oak_log}}, cobble{{items::cobblestone}}, stick{{item("minecraft:stick")}};
        data->crafting = {
            {Recipe::Kind::shapeless, "test:planks", 0, 0, {log}, {items::oak_planks, 4}},
            {Recipe::Kind::shaped, "test:pickaxe", 3, 3, {cobble, cobble, cobble, {}, stick, {}, {}, stick, {}}, {item("minecraft:stone_pickaxe"), 1}},
        };
        const PlayerKey alex{4, 2};
        const BlockPos table{0, 0, 0};
        Sim s(bundle(), 7, 2, data);
        s.tick({place(table, registry_block("crafting_table"))});
        // Crafting table window: 0 the result, 1-9 the grid, 10-36 the main inventory, 37-45 the hotbar.
        const auto click = [&](std::int16_t slot, std::uint8_t button, std::uint8_t mode) {
            return ContainerClick{alex, std::optional(table), slot, button, mode, false, at_block(0.5, 2, 0.5), {}};
        };
        const auto player = [&] {
            const auto d = s.oracle.dump();
            const auto it = std::ranges::find(d.players, alex, &PlayerInventory::player);
            if (it != d.players.end()) return *it;
            PlayerInventory none;
            none.player = alex;
            return none;
        };
        s.tick({SetPlayerSlot{alex, 36, {items::oak_log, 3}}, SetPlayerSlot{alex, 37, {item("minecraft:stick"), 2}},
                SetPlayerSlot{alex, 38, {items::cobblestone, 3}}, SetPlayerSlot{alex, 9, {items::stone, 64}}});
        s.tick({click(37, 0, 1)});
        require(player().table[1] == ItemStack{items::oak_log, 3} && player().table[0] == ItemStack{items::oak_planks, 4},
                "a shift-click from the hotbar fills the grid");
        s.tick({click(0, 0, 1)});
        require(player().table[1].empty() && player().slots[44] == ItemStack{items::oak_planks, 12}, "the planks into the hotbar from its end");
        // The pickaxe needs the whole 3x3 grid.
        s.tick({click(39, 0, 0), click(-999, 0, 5), click(1, 1, 5), click(2, 1, 5), click(3, 1, 5), click(-999, 2, 5)});
        s.tick({click(38, 0, 0), click(5, 1, 0), click(8, 1, 0)});
        require(player().table[0] == ItemStack{item("minecraft:stone_pickaxe"), 1}, "a pickaxe");
        s.tick({click(0, 0, 0)});
        require(player().cursor.item == item("minecraft:stone_pickaxe") && player().table[0].empty() && player().table[5].empty(),
                "taken: the grid is used");
        s.tick({click(43, 0, 0), click(10, 0, 1)});
        require(player().table[1] == ItemStack{items::stone, 64} && player().slots[9].empty(), "the stone into the first empty grid slot");
        // Closing gives the grid back to the inventory.
        s.tick({CloseContainer{alex, at_block(0.5, 2, 0.5), {}}});
        require(std::ranges::all_of(player().table, &ItemStack::empty) && player().slots[36] == ItemStack{items::stone, 64},
                "the grid back in the inventory, hotbar first");
        // A player who leaves drops the grids and the carried stack.
        s.tick({click(37, 0, 1), SetPlayerSlot{alex, 9, {item("minecraft:stick"), 1}}});
        s.tick({ContainerClick{alex, std::nullopt, 9, 0, 0, false, {}, {}}});
        const auto before = s.oracle.dump().overworld().entities.size();
        s.tick({CloseContainer{alex, at_block(0.5, 2, 0.5), {}, true}});
        require(std::ranges::all_of(player().table, &ItemStack::empty) && player().cursor.empty() &&
                    s.oracle.dump().overworld().entities.size() == before + 2,
                "the grid and the cursor dropped");
        // A death drops an open table's grid with the inventory.
        s.tick({SetPlayerSlot{alex, 36, {items::oak_log, 2}}});
        s.tick({click(37, 0, 1)});
        s.tick({circuit::contracts::PlayerDeath{alex, at_block(0.5, 2, 0.5)}});
        const auto d = s.oracle.dump();
        require(std::ranges::find(d.players, alex, &PlayerInventory::player) == d.players.end() &&
                    std::ranges::count(d.overworld().entities, ItemStack{items::oak_log, 2}, &circuit::gameplay::Entity::stack) == 1,
                "the log dropped at death");
    });

    suite.run("held maps explore the terrain as both official servers draw it", [] {
        using circuit::contracts::PlayerPresence;
        using circuit::contracts::PlayerUseItem;
        using circuit::contracts::SetPlayerSlot;
        // Components as one byte: the map's id (the server reads them through the data packs).
        auto data = std::make_shared<circuit::gameplay::GameData>();
        data->int_component = [](const ComponentPatch& p, std::string_view) -> std::optional<std::int32_t> {
            if (p.empty() || p.added().empty()) return std::nullopt;
            return static_cast<std::int32_t>(p.added()[0].value[0]);
        };
        data->with_int_component = [](const ComponentPatch&, std::string_view, std::optional<std::int32_t> v) -> std::optional<ComponentPatch> {
            return ComponentPatch::make(772, {{1, {static_cast<std::byte>(v.value_or(0))}}}, {}, {});
        };
        Sim s(bundle(), 7, 4, data);
        const PlayerKey alex{1, 2};
        const auto stone = blocks::stone(), grass = registry_block("grass_block"), glass = registry_block("glass");
        // Grass at y 0 under x and z -8 to 7; on it a stone pillar, two pools of water 3 deep in stone rings
        // (at pixels of both parities), and glass on stone.
        std::vector<CommandBody> build;
        for (int x = -8; x < 8; ++x)
            for (int z = -8; z < 8; ++z) build.push_back(place({x, 0, z}, grass));
        build.push_back(place({2, 1, 2}, stone));
        for (const int cx : {-4, -1})
            for (int x = cx - 1; x <= cx + 1; ++x)
                for (int z = -5; z <= -3; ++z)
                    for (int y = 1; y <= 3; ++y) build.push_back(place({x, y, z}, x == cx && z == -4 ? blocks::water_source() : stone));
        build.push_back(place({4, 1, -4}, stone));
        build.push_back(place({4, 2, -4}, glass));
        s.tick(build);
        const auto pixel = [&](int x, int z) {
            const auto d = s.oracle.dump();
            return d.maps.empty() ? 0 : d.maps.front().colors[static_cast<std::size_t>((x + 64) + (z + 64) * 128)];
        };
        s.tick({SetPlayerSlot{alex, 36, {items::map, 1}}, PlayerPresence{alex, {0.5, 1, 0.5}, false, true, 0}});
        s.tick({PlayerUseItem{alex, 36, {0.5, 2.62, 0.5}, 0.0f, 90.0f, 4.5, true, {}}});
        // In the tick the map is made, held, it explores: every column but the first (the step is 1).
        require(s.oracle.dump().maps.size() == 1 && s.oracle.dump().maps.front().holders == std::vector<MapHolder>{{alex, 1}},
                "map 0 made and explored once");
        require(pixel(2, 2) == 11 * 4 + 2 && pixel(2, 3) == 1 * 4 + 0 && pixel(2, 1) == 1 * 4 + 1 && pixel(0, 0) == 1 * 4 + 1,
                "the pillar bright, the grass south of it dark, grass level");
        require(pixel(-4, -4) == 12 * 4 + 2 && pixel(-1, -4) == 12 * 4 + 1, "water 3 deep: bright on an even pixel, normal on an odd one");
        require(pixel(4, -4) == 11 * 4 + 2, "the stone under the glass");
        // Outside the grass the void draws as stone; the first column waits for the 16th explore.
        require(pixel(20, 20) == 11 * 4 + 1 && pixel(-64, 0) == 0, "void as stone; column 0 unexplored");
        for (int i = 0; i < 14; ++i) s.tick();
        require(pixel(-64, 0) == 0, "column 0 after 15 explores");
        s.tick();
        require(pixel(-64, 0) == 11 * 4 + 1 && s.oracle.dump().maps.front().holders.front().step == 16, "column 0 at the 16th");
        // Changes show where the step or a changed column next to it reaches; in 16 ticks everywhere.
        s.tick({place({-6, 1, 6}, stone)});
        for (int i = 0; i < 16; ++i) s.tick();
        require(pixel(-6, 6) == 11 * 4 + 2, "a new block drawn");
        // Not held (another hotbar slot), the map does not explore and its step stays.
        s.tick({PlayerPresence{alex, {0.5, 1, 0.5}, false, true, 1}, place({-6, 1, 5}, stone)});
        for (int i = 0; i < 20; ++i) s.tick();
        require(pixel(-6, 5) == 1 * 4 + 1 && s.oracle.dump().maps.front().holders.front().step == 33, "unheld: no change, the step kept");
        // In the off hand it explores.
        const auto filled = s.oracle.dump().players.front().slots[36];
        s.tick({SetPlayerSlot{alex, 36, {}}, SetPlayerSlot{alex, 45, filled}});
        for (int i = 0; i < 16; ++i) s.tick();
        require(pixel(-6, 5) == 11 * 4 + 2, "the off hand explores");
        // A player who no longer carries the map is forgotten: holding it again starts at step 1.
        s.tick({SetPlayerSlot{alex, 45, {}}});
        require(s.oracle.dump().maps.front().holders.empty(), "forgotten");
        s.tick({SetPlayerSlot{alex, 36, filled}, PlayerPresence{alex, {0.5, 1, 0.5}, false, true, 0}});
        require(s.oracle.dump().maps.front().holders == std::vector<MapHolder>{{alex, 1}}, "a new step");
        // The main hand is one of the nine hotbar slots.
        s.tick({PlayerPresence{alex, {0.5, 1, 0.5}, false, true, 9}});
        require(of_kind(s.last, TraceKind::command_rejected).size() == 1, "hotbar slot 9 refused");
    });

    return suite.finish();
}
