// SPDX-License-Identifier: AGPL-3.0-only
// Reference-rule fixtures with expectations derived by hand from the rule
// definitions (not from running the runtime). They check the rules and the
// oracle; vanilla compatibility is not established by these values.
#include "../oracle/oracle.hpp"
#include "../support/check.hpp"
#include "../support/light_reference.hpp"
#include "../support/scenario.hpp"
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/light.hpp>
#include <circuit/gameplay/loot.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <circuit/gameplay/rules.hpp>
#include <circuit/gameplay/survival.hpp>
#include <cmath>
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

// A stone floor at y = -1 under x in [x0, x1] and z in [z0, z1]: dust and
// diodes need support.
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
        // Pistons cannot move them.
        {
            Sim s;
            s.tick({place({1, 0, 0}, dirt), place({0, 0, 0}, blocks::piston(D::east))});
            s.tick({place({0, 0, 1}, blocks::redstone_block())});
            require(!s.at({0, 0, 0}).flag() && s.at({1, 0, 0}) == dirt && of_kind(s.last, TraceKind::piston_blocked).size() == 1,
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
                require(e.position.y == (e.stack.item == items::redstone ? fixed_one : 0), std::format("item rests at {}", e.position.y));
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
        require(d.overworld().entities.size() == 4, std::format("four drops, got {}", d.overworld().entities.size()));
        require(d.overworld().entities[0].stack == ItemStack{items::redstone, 1} && d.overworld().entities[1].stack == ItemStack{items::comparator, 1}
                    && d.overworld().entities[2].stack == ItemStack{items::wheat, 1} && d.overworld().entities[3].stack == ItemStack{items::wheat_seeds, 1},
                "drops in event order");
        // Spawned at the dust's center inside the arriving block, then lifted onto it by the entity tick.
        require(d.overworld().entities[0].position == FixedVec{2 * fixed_one + 2048, fixed_one, 2048} && d.overworld().entities[0].on_ground,
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
        require(d.overworld().entities.size() == 1 && d.overworld().entities[0].position.x == 16 * fixed_one, "pushed half a block, out of the cell");
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

        // Survival placement uses the item up; a water bucket leaves a bucket.
        const circuit::contracts::PlayerKey sam{3, 4};
        s.tick({SetBlock{{8, 0, 8}, blocks::stone().raw(), {}}, SetBlock{{9, 0, 9}, blocks::stone().raw(), {}},
                circuit::contracts::SetPlayerSlot{sam, 36, {items::stone, 2, 0}},
                circuit::contracts::SetPlayerSlot{sam, 37, {items::water_bucket, 1, 0}},
                circuit::contracts::PlayerUse{sam, 36, {8, 0, 8}, D::up, D::south, D::south, false, true, 3, 3},
                circuit::contracts::PlayerUse{sam, 37, {9, 0, 9}, D::up, D::south, D::south, false, true, 3, 3},
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
        // Creative breaking drops the chest's contents, not the chest.
        s.tick({circuit::contracts::PlayerBreak{alex, {0, 0, 0}, false, 36}});
        auto d = s.oracle.dump();
        require(d.overworld().entities.size() == 2 && d.overworld().entities[0].stack == ItemStack{items::iron_ingot, 5, 0}
                    && d.overworld().entities[1].stack == ItemStack{items::redstone, 64, 0},
                "contents drop in slot order");
        // Survival with a stone pickaxe: stone gives cobblestone, obsidian nothing; the
        // pickaxe (131 uses) breaks on its last use.
        s.tick({circuit::contracts::PlayerBreak{alex, {2, 0, 0}, true, 36}});
        d = s.oracle.dump();
        require(d.overworld().entities.size() == 3 && d.overworld().entities[2].stack.item == items::cobblestone, "cobblestone");
        require(d.players.empty(), "the pickaxe broke");
        s.tick({circuit::contracts::PlayerBreak{alex, {4, 0, 0}, true, 36}});
        require(s.oracle.dump().overworld().entities.size() == 3 && s.at({4, 0, 0}).is_air(), "obsidian by hand drops nothing");
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
        std::set<std::pair<std::int64_t, std::int64_t>> directions;
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
        // 16x16 wheat sealed in stone (floor, walls and roof); with a
        // glowstone in the middle, light 15 - distance reaches the crops.
        for (const bool lit : {false, true}) {
            Sim s(bundle(), 5, 1);
            s.tick({SetGameRule{"randomTickSpeed", 200}});
            std::vector<CommandBody> box, crops;
            for (int x = -1; x <= 16; ++x)
                for (int z = -1; z <= 16; ++z) {
                    box.push_back(place({x, -1, z}, blocks::stone()));
                    box.push_back(place({x, 1, z}, blocks::stone()));
                    if (x == -1 || x == 16 || z == -1 || z == 16) box.push_back(place({x, 0, z}, blocks::stone()));
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
            require(lit ? grown > 20 : grown == 0, std::format("{}: {} crops grew", lit ? "lit" : "dark", grown));
            require(far == 0, std::format("{} crops grew with light below 9", far));
        }
    });

    suite.run("idle levels and random tick ranges", [] {
        // A level is awake while active, and for 300 ticks after it was last
        // active or started.
        require(level_awake(true, 1000, 0, 0) && level_awake(false, 300, 0, 0) && !level_awake(false, 301, 0, 0)
                    && level_awake(false, 400, 100, 0) && !level_awake(false, 401, 100, 0) && level_awake(false, 900, 0, 600),
                "idle after 300 ticks");
        // 1.21.4: within 128 blocks (horizontally, exclusive) of a
        // non-spectator player's position, from the chunk's center.
        std::vector<Presence> presences{{PlayerKey{1, 1}, FixedVec{8 * fixed_one, 0, 8 * fixed_one}, false}};
        const auto near = [&](ChunkPos c) { return player_near(presences, c); };
        require(near({0, 0}) && near({7, 0}) && !near({8, 0}) && near({-7, 0}) && !near({-8, 0}) && near({5, 5}) && !near({6, 6}),
                "128 blocks from the chunk center");
        presences[0].position.x = 8 * fixed_one + 1;
        require(near({8, 0}) && !near({-8, 0}), "measured in fixed point");
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
        // 2^44 fixed units is 2^32 blocks: narrowing first would alias block (0, 1, 0).
        s.tick({SpawnItem{{std::int64_t{1} << 44, fixed_one, 0}, {}, {items::stone, 1}},
                SpawnItem{{0, INT64_MIN, 0}, {}, {items::stone, 1}}});
        const auto rejected = of_kind(s.last, TraceKind::command_rejected);
        require(rejected.size() == 2 && rejected[0].b == static_cast<std::uint64_t>(ErrorCode::out_of_bounds)
                    && rejected[1].b == static_cast<std::uint64_t>(ErrorCode::out_of_bounds),
                "rejected as out of bounds");
        auto after = s.oracle.dump();
        after.time = before.time;
        after.day_time = before.day_time;
        after.weather = before.weather;
        for (auto& l : after.levels) l.last_active = before.level(l.dimension).last_active;
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
        const Entry books{.kind = Entry::Kind::item, .item = items::book, .functions = {{.kind = Function::Kind::set_count, .count = 3}}};
        data->block_loot[block_of("minecraft:bookshelf")] = {{{{{.kind = Entry::Kind::alternatives, .children = {self, books}}}}}};
        // Coal ore gives coal, which fortune multiplies; it needs a pickaxe.
        const Entry coal{.kind = Entry::Kind::item, .item = items::coal, .functions = {{.kind = Function::Kind::ore_bonus, .enchantment = fortune}}};
        data->block_loot[block_of("minecraft:coal_ore")] = {{{{coal}}}};
        data->block_tools[block_of("minecraft:coal_ore")] = {registry::Tool::pickaxe, 1};
        // A double slab gives two.
        const loot::Condition is_double{.kind = loot::Condition::Kind::block_state, .properties = {{"type", "double", {}, {}}}};
        const Entry slab{.kind = Entry::Kind::item, .item = items::oak_slab,
                         .functions = {{.kind = Function::Kind::set_count, .count = 2, .conditions = {is_double}}}};
        data->block_loot[block_of("minecraft:oak_slab")] = {{{{slab}}}};
        // More than a stack splits.
        const Entry balls{.kind = Entry::Kind::item, .item = items::clay_ball, .functions = {{.kind = Function::Kind::set_count, .count = 100}}};
        data->block_loot[block_of("minecraft:clay")] = {{{{balls}}}};

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
                for (auto i = before; i < d.overworld().entities.size(); ++i) out.push_back(d.overworld().entities[i].stack);
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
        const auto table = [](std::vector<Entry> entries) { return loot::Table{{loot::Pool{std::move(entries)}}}; };
        loot::BreakContext ctx{{{"age", "5"}, {"half", "lower"}}, {items::shears, 1}, {}};
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
        // Counts: limits, and nothing for a count of zero.
        Entry limited = item(items::wheat);
        limited.functions = {{.kind = Function::Kind::set_count, .count = 9}, {.kind = Function::Kind::limit_count, .max = 4}};
        require(loot::drops(table({limited}), ctx) == std::optional(std::vector<ItemStack>{{items::wheat, 4}}), "a limit");
        Entry none = item(items::wheat);
        none.functions = {{.kind = Function::Kind::set_count, .count = 0}};
        require(loot::drops(table({none}), ctx)->empty(), "a count of zero");
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
        // Placing a log shows planks; taking them uses one log each time.
        s.tick({SetPlayerSlot{alex, 9, {items::oak_log, 3}}});
        s.tick({click(9, 0, 0)});
        s.tick({click(1, 1, 0)});
        require(slot(0) == ItemStack{items::oak_planks, 4} && slot(1) == ItemStack{items::oak_log, 1}, "the result follows the grid");
        s.tick({click(1, 0, 0)});
        s.tick({click(0, 0, 0)});
        require(player().cursor == ItemStack{items::oak_planks, 4} && slot(1).count == 2 && slot(0).count == 4, "taken onto the cursor");
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

    return suite.finish();
}
