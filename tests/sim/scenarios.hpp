// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../support/scenario.hpp"
#include <array>
#include <format>
#include <type_traits>
#include <variant>
#include <functional>
#include <string>

// Scenario library for differential runs. Each scenario is a fixed sequence of
// per-tick inputs; machines deliberately straddle chunk edges and corners.
namespace circuit::test {
using D = Direction;

struct Scenario {
    std::string name;
    std::uint64_t seed{};
    Tick ticks{};
    std::function<std::vector<CommandBody>(Tick)> inputs;
    Tick dump_every{10};
    // The levels of a tick's commands, in order; empty: every command is the overworld's.
    std::function<std::vector<DimensionId>(Tick)> levels{};
};

// A tick's commands with admissions and levels.
inline std::vector<Command> admitted(Admissions& admit, const Scenario& s, Tick t) {
    if (!s.levels) return admit(s.inputs(t));
    const auto levels = s.levels(t);
    return admit(s.inputs(t), [&](std::size_t i) { return levels.at(i); });
}

inline std::vector<CommandBody> redstone_line(std::int32_t x, std::int32_t y, std::int32_t z, std::uint8_t delay) {
    return {place({x, y, z}, blocks::lever()), place({x + 1, y, z}, blocks::wire()), place({x + 2, y, z}, blocks::wire()),
            place({x + 3, y, z}, blocks::wire()), place({x + 4, y, z}, blocks::wire()),
            place({x + 5, y, z}, blocks::repeater(D::east, delay)), place({x + 6, y, z}, blocks::wire()),
            place({x + 7, y, z}, blocks::lamp())};
}


inline Scenario slice_scenario() {
    return {"vertical_slice", 3, 80, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2) {
            auto b = redstone_line(13, 0, 0, 1);
            append(b, redstone_line(12, 0, 8, 3));
            return b;
        }
        std::vector<CommandBody> c;
        if (t % 10 == 3) c.push_back(use_lever({13, 0, 0}));
        if (t % 7 == 4) c.push_back(use_lever({12, 0, 8}));
        return c;
    }};
}

inline Scenario clock_scenario() {
    return {"observer_clocks", 5, 200, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2)
            return {place({14, 0, 0}, blocks::lamp()), place({17, 0, 0}, blocks::lamp()),
                    place({15, 0, 0}, blocks::observer(D::east)), place({16, 0, 0}, blocks::observer(D::west)),
                    place({5, 0, 14}, blocks::lamp()), place({5, 0, 17}, blocks::lamp()),
                    place({5, 0, 15}, blocks::observer(D::south)), place({5, 0, 16}, blocks::observer(D::north))};
        if (t == 120) return {place({15, 0, 0}, blocks::air())};
        return {};
    }};
}

inline Scenario piston_scenario() {
    return {"pistons", 9, 100, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2) {
            std::vector<CommandBody> b{place({14, 0, 4}, blocks::slime()), place({14, 1, 4}, blocks::stone()),
                                       place({15, 0, 4}, blocks::stone()), place({14, 0, 5}, blocks::stone()),
                                       place({13, 0, 4}, blocks::piston(D::east, true)), place({13, 0, 3}, blocks::lever()),
                                       place({-3, 0, -2}, blocks::stone()), place({-2, 0, -2}, blocks::piston(D::west)),
                                       place({-1, 0, -2}, blocks::lever())};
            for (int z = 11; z <= 20; ++z) b.push_back(place({3, 0, z}, blocks::stone()));
            b.push_back(place({3, 0, 10}, blocks::piston(D::south, true)));
            b.push_back(place({16, 0, 3}, blocks::observer(D::south)));
            b.push_back(place({16, 0, 2}, blocks::lamp()));
            return b;
        }
        std::vector<CommandBody> c;
        if (t % 4 == 3) c.push_back(use_lever({13, 0, 3}));
        if (t > 2 && t % 6 == 1) c.push_back(use_lever({-1, 0, -2}));
        if (t % 10 == 5) c.push_back(place({2, 0, 10}, blocks::redstone_block()));
        if (t % 10 == 0 && t > 2) c.push_back(place({2, 0, 10}, blocks::air()));
        return c;
    }};
}

inline Scenario hopper_scenario() {
    return {"hoppers_and_items", 13, 120, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2) {
            std::vector<CommandBody> b{place({15, 1, 20}, blocks::chest()), insert({15, 1, 20}, 0, items::iron_ingot, 64),
                                       insert({15, 1, 20}, 1, items::stone, 40), place({15, 0, 20}, blocks::hopper(D::east)),
                                       place({16, 0, 20}, blocks::hopper(D::east)), place({17, 0, 20}, blocks::hopper(D::east)),
                                       place({18, 0, 20}, blocks::chest()), place({16, 0, 26}, blocks::chest())};
            for (std::uint8_t i = 0; i < 26; ++i) b.push_back(insert({16, 0, 26}, i, items::stone, 64));
            b.push_back(insert({16, 0, 26}, 26, items::stone, 60));
            b.push_back(place({15, 0, 26}, blocks::hopper(D::east)));
            b.push_back(insert({15, 0, 26}, 0, items::stone, 10));
            b.push_back(place({17, 0, 26}, blocks::hopper(D::west)));
            b.push_back(insert({17, 0, 26}, 0, items::stone, 10));
            for (int x = 12; x <= 20; ++x) b.push_back(place({x, 0, 31}, x == 17 ? blocks::hopper(D::down) : blocks::stone()));
            b.push_back(place({0, 0, 40}, blocks::hopper(D::north)));
            b.push_back(place({0, 0, 39}, blocks::chest()));
            return b;
        }
        std::vector<CommandBody> c;
        if (t < 60 && t % 7 == 3) c.push_back(SpawnItem{at_block(15.0, 1.0, 31.5), {4000.0 / 4096.0, 0, 0}, {items::redstone, 3}});
        if (t < 80 && t % 9 == 4) c.push_back(SpawnItem{at_block(0.5, 6.0, 40.5), {0, 200.0 / 4096.0, 0}, {items::wheat, 1}});
        if (t == 50) c.push_back(place({14, 0, 20}, blocks::lever(true)));
        if (t == 70) c.push_back(use_lever({14, 0, 20}));
        return c;
    }};
}

inline Scenario water_scenario() {
    return {"water_corner", 17, 160, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2) {
            std::vector<CommandBody> b;
            for (int x = 8; x < 24; ++x)
                for (int z = 8; z < 24; ++z) b.push_back(place({x, 0, z}, blocks::stone()));
            return b;
        }
        if (t == 3) return {place({15, 1, 15}, blocks::water_source())};
        if (t == 60) return {place({20, 1, 20}, blocks::water_source())};
        if (t == 80) return {place({15, 1, 15}, blocks::air())};
        return {};
    }};
}

inline Scenario wheat_scenario() {
    return {"wheat_observers", 21, 300, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        if (t == 2) {
            std::vector<CommandBody> b;
            // Wheat stays on farmland only.
            for (int x = 10; x <= 21; ++x) {
                b.push_back(place({x, -1, 5}, registry_block("farmland")));
                b.push_back(place({x, 0, 5}, blocks::wheat()));
                b.push_back(place({x, 0, 7}, blocks::lamp()));
                b.push_back(place({x, 0, 6}, blocks::observer(D::north)));
            }
            for (int z = 12; z < 20; ++z)
                for (int x = 12; x < 20; ++x) {
                    b.push_back(place({x, -1, z}, registry_block("farmland")));
                    b.push_back(place({x, 0, z}, blocks::wheat()));
                }
            return b;
        }
        return {};
    }};
}

// One self-contained machine per chunk over a 9x9 area: mostly independent work.
inline Scenario many_machines_scenario() {
    return {"many_machines", 23, 100, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 4)};
        std::vector<CommandBody> c;
        for (int cx = -4; cx <= 4; ++cx)
            for (int cz = -4; cz <= 4; ++cz) {
                const int x = cx * 16, z = cz * 16;
                if (t == 2) {
                    append(c, redstone_line(x + 1, 0, z + 2, static_cast<std::uint8_t>(1 + ((cx + 4 + cz + 4) % 4))));
                    append(c, {place({x + 2, 1, z + 8}, blocks::chest()), insert({x + 2, 1, z + 8}, 0, items::stone, 20),
                               place({x + 2, 0, z + 8}, blocks::hopper(D::east)), place({x + 3, 0, z + 8}, blocks::chest()),
                               place({x + 11, 0, z + 12}, blocks::lamp()), place({x + 14, 0, z + 12}, blocks::lamp()),
                               place({x + 12, 0, z + 12}, blocks::observer(D::east)),
                               place({x + 13, 0, z + 12}, blocks::observer(D::west))});
                } else if (t > 2 && static_cast<int>(t % 9) == ((cx + 4) * 3 + cz + 4) % 9) {
                    c.push_back(use_lever({x + 1, 0, z + 2}));
                }
            }
        return c;
    }, 25};
}

inline Scenario ticket_scenario() {
    return {"tickets", 29, 80, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1), ticket(2, {3, 0}, 0)};
        if (t == 2) {
            auto b = redstone_line(13, 0, 0, 2);
            append(b, {place({49, 0, 5}, blocks::lever(true)), place({50, 0, 5}, blocks::lamp()),
                       place({52, 0, 5}, blocks::piston(D::east)), place({51, 0, 5}, blocks::lever()),
                       place({53, 0, 5}, blocks::stone())});
            return b;
        }
        if (t == 10) return {use_lever({13, 0, 0}), use_lever({49, 0, 5}), use_lever({51, 0, 5})};
        if (t == 11) return {RemoveTicket{2}};
        if (t == 20) return {use_lever({13, 0, 0})};
        if (t == 30) return {ticket(2, {3, 0}, 0)};
        if (t == 31) return {SetBlock{{50, 1, 5}, blocks::stone().raw(), Generation{0}},
                             SetBlock{{50, 2, 5}, blocks::stone().raw(), Generation{1}}};
        if (t == 40) return {ticket(3, {1, 0}, 0), use_lever({51, 0, 5})};
        if (t == 50) return {RemoveTicket{1}};
        if (t == 60) return {ticket(1, {0, 0}, 0)};
        return {};
    }};
}

// Piston chains whose block events queue further events in the same phase:
// nine independent chains plus one straddling a chunk edge, all toggled together.
inline std::vector<CommandBody> piston_chain(std::int32_t x0, std::int32_t z0) {
    return {place({x0 + 3, 0, z0 + 2}, blocks::redstone_block()), place({x0 + 4, 0, z0 + 4}, blocks::redstone_block()),
            place({x0 + 6, 0, z0 + 5}, blocks::lamp()), place({x0 + 2, 0, z0 + 2}, blocks::piston(D::east, true)),
            place({x0 + 4, 0, z0 + 3}, blocks::piston(D::south, true)), place({x0 + 5, 0, z0 + 5}, blocks::piston(D::east, true)),
            place({x0 + 1, 0, z0 + 2}, blocks::lever())};
}

inline Scenario piston_chain_scenario() {
    return {"piston_chains", 31, 60, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        std::vector<CommandBody> c;
        for (int cx = -1; cx <= 1; ++cx)
            for (int cz = -1; cz <= 1; ++cz) {
                if (t == 2) append(c, piston_chain(cx * 16, cz * 16));
                else if (t % 5 == 3) c.push_back(use_lever({cx * 16 + 1, 0, cz * 16 + 2}));
            }
        if (t == 2) append(c, piston_chain(12, 36));
        else if (t % 5 == 3) c.push_back(use_lever({13, 0, 38}));
        return c;
    }, 5};
}

// A double chest straddling x=16, fed by hoppers in both chunks, drained into
// a third chest, and read by comparators in both chunks.
inline Scenario comparator_chest_scenario() {
    using blocks::ChestType;
    return {"comparators_and_double_chests", 37, 200, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2)
            return {place({15, 1, 10}, blocks::chest(D::north, ChestType::left)),
                    place({16, 1, 10}, blocks::chest(D::north, ChestType::right)),
                    place({15, 3, 10}, blocks::chest()), insert({15, 3, 10}, 0, items::iron_ingot, 64),
                    place({16, 3, 10}, blocks::chest()), insert({16, 3, 10}, 0, items::stone, 64),
                    insert({16, 3, 10}, 1, items::stone, 64),
                    place({15, 2, 10}, blocks::hopper(D::down)), place({16, 2, 10}, blocks::hopper(D::down)),
                    place({16, 0, 10}, blocks::hopper(D::east)), place({17, 0, 10}, blocks::chest()),
                    // Supports for the components on top.
                    place({15, 0, 11}, blocks::stone()), place({15, 0, 12}, blocks::stone()), place({15, 0, 13}, blocks::stone()),
                    place({16, 0, 11}, blocks::stone()), place({17, 0, 11}, blocks::stone()), place({18, 0, 11}, blocks::stone()),
                    place({17, -1, 9}, blocks::stone()),
                    place({15, 1, 11}, blocks::comparator(D::south)), place({15, 1, 12}, blocks::wire()),
                    place({15, 1, 13}, blocks::wire()), place({15, 1, 14}, blocks::lamp()),
                    place({16, 1, 11}, blocks::comparator(D::south, true)), place({16, 1, 12}, blocks::lamp()),
                    place({17, 1, 11}, blocks::wire()), place({18, 1, 11}, blocks::lever()),
                    place({17, 0, 9}, blocks::comparator(D::north)), place({17, 0, 8}, blocks::lamp())};
        std::vector<CommandBody> c;
        if (t % 13 == 5) c.push_back(use_lever({18, 1, 11}));
        if (t == 50) c.push_back(use_block({16, 1, 11}));
        if (t == 120) c.push_back(place({15, 1, 10}, blocks::air()));
        if (t == 140) c.push_back(place({15, 1, 10}, blocks::chest(D::north, ChestType::left)));
        return c;
    }};
}

// Short and long pulses on sticky pistons whose moves cross chunk edges:
// block dropping, pulls, destroyed dust, and moving blocks next to lamps.
inline Scenario piston_pulse_scenario() {
    return {"piston_pulses", 41, 120, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        std::vector<CommandBody> c;
        for (int row = 0; row < 4; ++row) {
            const int z = row * 3;
            if (t == 2)
                append(c, {place({15, 0, z}, blocks::stone()), place({16, 0, z}, blocks::wire()),
                           place({14, 0, z}, blocks::piston(D::east, true)), place({14, 0, z + 1}, blocks::lever()),
                           place({17, 0, z + 1}, blocks::lamp()), place({16, 0, z + 1}, blocks::redstone_block()),
                           place({15, 0, z + 1}, blocks::piston(D::east))});
            // Row r pulses with length r + 1 every 12 ticks; rows start at different ticks.
            const Tick start = 5 + static_cast<Tick>(row) * 2;
            if (t >= start && (t - start) % 12 == 0) c.push_back(use_lever({14, 0, z + 1}));
            if (t >= start + row + 1 && (t - start - row - 1) % 12 == 0) c.push_back(use_lever({14, 0, z + 1}));
        }
        return c;
    }};
}

// In every chunk of a 5x5 area at once: sticky pistons destroy a different
// destroyable block each (drops spawned in parallel roots), and other pistons
// push items across the east chunk edge. The cycle repeats every 20 ticks.
inline Scenario drops_scenario() {
    return {"drops_and_pushes", 43, 100, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        std::vector<CommandBody> c;
        int kind = 0;
        for (int cx = -2; cx <= 2; ++cx)
            for (int cz = -2; cz <= 2; ++cz, ++kind) {
                const int x = cx * 16, z = cz * 16;
                const BlockState destroyables[]{blocks::wire(), blocks::lever(), blocks::repeater(D::east),
                                                blocks::comparator(D::east), blocks::wheat(7), blocks::water_source()};
                const auto destroyable = destroyables[kind % 6];
                if (t == 2) {
                    for (int dx = 0; dx < 16; ++dx) {
                        c.push_back(place({x + dx, -1, z + 3}, blocks::stone()));
                        c.push_back(place({x + dx, -1, z + 8}, blocks::stone()));
                    }
                    append(c, {place({x + 2, 0, z + 3}, blocks::stone()), place({x + 3, 0, z + 3}, destroyable),
                               place({x + 1, 0, z + 3}, blocks::piston(D::east, true)),
                               place({x + 14, 0, z + 8}, blocks::stone()), place({x + 13, 0, z + 8}, blocks::piston(D::east, true))});
                }
                const auto phase = t % 20;
                if (phase == 4) c.push_back(SpawnItem{at_block(x + 15.5, 0.0, z + 8.5), {}, {items::iron_ingot, 1}});
                if (phase == 5) {
                    c.push_back(place({x + 1, 1, z + 3}, blocks::redstone_block()));
                    c.push_back(place({x + 13, 1, z + 8}, blocks::redstone_block()));
                }
                if (phase == 15) {
                    c.push_back(place({x + 1, 1, z + 3}, blocks::air()));
                    c.push_back(place({x + 13, 1, z + 8}, blocks::air()));
                }
                if (phase == 18 && t > 20) c.push_back(place({x + 3, 0, z + 3}, destroyable));
            }
        return c;
    }, 5};
}

// Observers watching piston writes across the x=16 chunk edge (pushed block,
// head cell, a sticky pull's vacated cell), and a diode chain whose
// tick priority depends on the diode in front, across the same edge.
inline Scenario observed_piston_scenario() {
    return {"observed_pistons", 53, 120, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2)
            return {place({15, 0, 0}, blocks::stone()), place({14, 0, 0}, blocks::piston(D::east)),
                    place({16, 1, 0}, blocks::observer(D::down)), place({16, 2, 0}, blocks::lamp()),
                    place({15, 0, 1}, blocks::observer(D::north)), place({15, 0, 2}, blocks::lamp()),
                    place({13, 0, 0}, blocks::lever()),
                    place({15, 0, 9}, blocks::repeater(D::east, 1)), place({16, 0, 9}, blocks::repeater(D::east, 1)),
                    place({17, 0, 9}, blocks::comparator(D::east)), place({18, 0, 9}, blocks::lamp()),
                    place({16, 0, 10}, blocks::observer(D::north)), place({14, 0, 9}, blocks::lever()),
                    place({15, 0, 12}, blocks::stone()), place({14, 0, 12}, blocks::piston(D::east, true)),
                    place({16, 0, 13}, blocks::observer(D::north)), place({16, 0, 14}, blocks::lamp()),
                    place({13, 0, 12}, blocks::lever())};
        std::vector<CommandBody> c;
        if (t > 2 && t % 7 == 3) c.push_back(use_lever({13, 0, 0}));
        if (t > 2 && t % 11 == 4) c.push_back(use_lever({14, 0, 9}));
        if (t > 2 && t % 5 == 1) c.push_back(use_lever({13, 0, 12}));
        return c;
    }};
}

// A piston whose power source is pushed away by a second piston retracts in
// the same block-event phase and removes the head it just created: the head's
// creation order is consumed although it never reaches commit. Copies sit
// inside a chunk, across x and z chunk edges and across a corner; they are
// cleared and rebuilt every 12 ticks.
inline Scenario transient_head_scenario() {
    return {"transient_heads", 59, 100, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        const std::array<BlockPos, 4> origins{{{0, 0, 0}, {16, 0, 8}, {8, 0, 16}, {-1, 0, -8}}};
        std::vector<CommandBody> c;
        for (const auto o : origins) {
            const auto at = [o](int dx, int dz) { return BlockPos{o.x + dx, o.y, o.z + dz}; };
            if (t % 12 == 2)
                append(c, {place(at(0, 0), blocks::piston(D::east)), place(at(-1, -1), blocks::piston(D::east)),
                           place(at(0, -1), blocks::redstone_block()), place(at(-1, -2), blocks::redstone_block())});
            if (t % 12 == 8)
                for (int dx = -1; dx <= 2; ++dx)
                    for (int dz = -2; dz <= 0; ++dz) c.push_back(place(at(dx, dz), blocks::air()));
        }
        return c;
    }};
}

// Blocks without rules of their own among machines, across the x=16 chunk
// edge: an opaque block conducts a repeater's power and a translucent one does
// not; dust survives on a top slab and pops off a bottom slab; a piston is
// blocked by an immovable block; items land on blocks with collision and fall
// through grass; water stops at a wall. Registry blocks also replace each
// other and are removed. Named and damaged items pass through hoppers across
// the edge, and a named item falls into one.
inline Scenario registry_block_scenario() {
    return {"registry_blocks", 61, 120, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        const auto dirt = registry_block("dirt");
        const auto top_slab = registry_block("oak_slab", {{"type", "top"}});
        if (t == 2) {
            std::vector<CommandBody> b{
                place({15, -1, 0}, dirt), place({15, 0, 0}, blocks::repeater(D::east)), place({16, 0, 0}, dirt),
                place({17, 0, 0}, blocks::lamp()), place({14, 0, 0}, blocks::lever()),
                place({15, -1, 2}, top_slab), place({15, 0, 2}, blocks::repeater(D::east)),
                place({16, 0, 2}, registry_block("white_stained_glass")), place({17, 0, 2}, blocks::lamp()),
                place({14, 0, 2}, blocks::lever()),
                place({20, -1, 4}, registry_block("oak_slab", {{"type", "bottom"}})), place({20, 0, 4}, blocks::wire()),
                place({21, -1, 4}, top_slab), place({21, 0, 4}, blocks::wire()),
                place({15, 0, 6}, dirt), place({14, 0, 6}, blocks::piston(D::east)), place({13, 0, 6}, blocks::lever()),
                place({18, 0, 8}, dirt), place({19, -1, 8}, blocks::stone()), place({19, 0, 8}, registry_block("short_grass")),
                SpawnItem{at_block(18.5, 3.5, 8.5), {}, {items::redstone, 3}}, SpawnItem{at_block(19.5, 3.5, 8.5), {}, {items::stone, 2}}};
            for (int x = 20; x <= 24; ++x) b.push_back(place({x, 0, 10}, blocks::stone()));
            b.push_back(place({23, 1, 10}, registry_block("cobblestone")));
            b.push_back(place({22, 1, 10}, blocks::water_source()));
            append(b, {place({15, 1, 14}, blocks::chest()), place({15, 0, 14}, blocks::hopper(D::east)),
                       place({16, 0, 14}, blocks::hopper(D::east)), place({17, 0, 14}, blocks::chest()),
                       InsertItem{{15, 1, 14}, 0, named(items::stone, 5, "tagged")},
                       InsertItem{{15, 1, 14}, 1, ItemStack{items::iron_pickaxe, 1, 40}},
                       InsertItem{{15, 1, 14}, 2, named(items::iron_pickaxe, 1, "worn", 7)},
                       SpawnItem{at_block(16.5, 3.5, 14.5), {}, named(items::redstone, 3, "dust")}});
            return b;
        }
        std::vector<CommandBody> c;
        if (t == 5) append(c, {place({20, 0, 5}, blocks::stone()), place({21, 0, 5}, blocks::stone())});
        if (t > 2 && t % 7 == 3) append(c, {use_lever({14, 0, 0}), use_lever({14, 0, 2})});
        if (t > 2 && t % 9 == 4) c.push_back(use_lever({13, 0, 6}));
        if (t == 40) c.push_back(place({16, 0, 0}, registry_block("cobblestone")));
        if (t == 70) c.push_back(place({16, 0, 0}, blocks::air()));
        return c;
    }};
}

// Chunk levels and what each runs. In the overworld, a forced ticket at
// chunk (0,0) makes it entity ticking, the ring around it block ticking and
// the next ring border; a player's loading ticket at chunk (6,0) loads
// without simulating, except while their simulation ticket is there (ticks
// 60-79); a post-teleport ticket loads chunk (-6,0) for 5 ticks; forced
// chunk (-12,0) drops with pending work at tick 4 and returns at 30. Every
// probed chunk holds a lamp switched off at tick 3 (a scheduled tick), a
// piston powered at tick 3 (a block event), a hopper filling a chest (a
// block entity) and a falling item (an entity). In the Nether, where no
// chunk is forced, a start ticket keeps chunk (0,0) entity ticking: its
// hopper and items stop after tick 300 (idle level) and resume while a
// player is there (ticks 400-419).
inline std::vector<std::pair<CommandBody, DimensionId>> chunk_level_commands(Tick t) {
    using enum DimensionId;
    std::vector<std::pair<CommandBody, DimensionId>> c;
    const auto probe = [&](std::int32_t x, DimensionId level) {
        if (t == 2) {
            c.push_back({place({x + 2, 0, 2}, blocks::lamp()), level});
            c.push_back({place({x + 1, 0, 2}, blocks::lever(true)), level});
            c.push_back({place({x + 5, 0, 2}, blocks::piston(D::up)), level});
            c.push_back({place({x + 8, 1, 2}, blocks::hopper(D::east)), level});
            c.push_back({place({x + 9, 1, 2}, blocks::chest()), level});
            c.push_back({insert({x + 8, 1, 2}, 0, items::stone, 64), level});
            c.push_back({place({x + 12, 0, 2}, blocks::stone()), level});
        }
        if (t == 3) {
            c.push_back({use_lever({x + 1, 0, 2}), level});
            c.push_back({place({x + 4, 0, 2}, blocks::redstone_block()), level});
        }
        if (t % 50 == 2) c.push_back({SpawnItem{at_block(x + 12.5, 4.0, 2.5), {}, {items::stone, 1}}, level});
    };
    if (t == 1) {
        c.push_back({ticket(1, {0, 0}, 0), overworld});
        c.push_back({AddTicket{2, {6, 0}, 1, TicketType::player_loading, 31}, overworld});
        c.push_back({AddTicket{10, {0, 0}, 0, TicketType::start, 31}, the_nether});
        c.push_back({ticket(5, {-12, 0}, 0), overworld});
    }
    for (const auto cx : {0, 1, 2, 6, -12}) probe(cx * 16, overworld);
    // Chunk (-12,0) drops at tick 4, with its lamp's tick pending, and comes
    // back at tick 30.
    if (t == 4) c.push_back({RemoveTicket{5}, overworld});
    if (t == 30) c.push_back({ticket(6, {-12, 0}, 0), overworld});
    probe(0, the_nether);
    if (t == 60) {
        c.push_back({AddTicket{3, {6, 0}, 0, TicketType::player_simulation, 29}, overworld});
        c.push_back({PlayerPresence{{1, 1}, at_block(104.5, 0.0, 8.5)}, overworld});
    }
    if (t == 80) c.push_back({RemoveTicket{3}, overworld});
    if (t == 90) c.push_back({AddTicket{4, {-6, 0}, 0, TicketType::post_teleport, 32, 5}, overworld});
    if (t == 400) c.push_back({PlayerPresence{{1, 1}, at_block(8.5, 2.0, 8.5)}, the_nether});
    if (t == 420) c.push_back({PlayerPresence{{1, 1}, {}, false, false}, the_nether});
    return c;
}
inline Scenario chunk_levels_scenario() {
    Scenario s{"chunk_levels", 61, 450, {}, 10, {}};
    s.inputs = [](Tick t) {
        std::vector<CommandBody> out;
        for (auto& [c, level] : chunk_level_commands(t)) out.push_back(std::move(c));
        return out;
    };
    s.levels = [](Tick t) {
        std::vector<DimensionId> out;
        for (const auto& [c, level] : chunk_level_commands(t)) out.push_back(level);
        return out;
    };
    return s;
}

// Wheat on farmland in a stone box lit by a glowstone in its middle: only
// the crops within light 9 grow (light follows the previous tick's blocks,
// so the crops come a tick after the box). A lamp switched on later lights
// more.
inline Scenario crop_light_scenario() {
    return {"crop_light", 71, 240, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        for (int x = -1; x <= 16; ++x)
            for (int z = -1; z <= 16; ++z) {
                const bool wall = x == -1 || x == 16 || z == -1 || z == 16;
                if (t == 2) {
                    c.push_back(place({x, -1, z}, wall ? blocks::stone() : registry_block("farmland")));
                    c.push_back(place({x, 1, z}, blocks::stone()));
                    if (wall) c.push_back(place({x, 0, z}, blocks::stone()));
                    if (x == 4 && z == 4) c.push_back(place({x, 0, z}, registry_block("glowstone")));
                    if (x == 12 && z == 12) c.push_back(place({x, 0, z}, blocks::lamp()));
                }
                if (t == 3 && !wall && !(x == 4 && z == 4) && !(x == 12 && z == 12) && !(x == 12 && z == 11))
                    c.push_back(place({x, 0, z}, blocks::wheat()));
            }
        // A lever beside the lamp, switched on at tick 120; random ticks ten
        // times faster from tick 3 (a game rule).
        if (t == 3) {
            c.push_back(place({12, 0, 11}, blocks::lever()));
            c.push_back(SetGameRule{"randomTickSpeed", 30});
        }
        // The day time: set, frozen for a while, then advancing again.
        if (t == 5) c.push_back(SetDayTime{13000});
        if (t == 40) c.push_back(SetGameRule{"doDaylightCycle", 0});
        if (t == 90) c.push_back(SetGameRule{"doDaylightCycle", 1});
        // Rain for 60 ticks, then clear for 40, which ends in a thunderstorm;
        // then a default-length storm.
        if (t == 10) c.push_back(SetWeather{WeatherKind::rain, 60});
        if (t == 100) c.push_back(SetWeather{WeatherKind::clear, 40});
        if (t == 200) c.push_back(SetWeather{WeatherKind::thunder, 0});
        if (t == 120) c.push_back(use_lever({12, 0, 11}));
        return c;
    }, 20};
}

// The scenario with every position raised by `dy` blocks, so that levels
// whose world starts at y = 0 (the Nether, the End) hold it too.
inline Scenario raised(Scenario s, std::int32_t dy) {
    const auto lift = [dy](CommandBody& b) {
        const auto up = [dy](BlockPos& p) { p.y += dy; };
        const auto up_fixed = [dy](Vec3& v) { v.y += dy; };
        std::visit([&](auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SetBlock> || std::is_same_v<T, UseBlock> || std::is_same_v<T, InsertItem>) up(c.pos);
            else if constexpr (std::is_same_v<T, SpawnItem> || std::is_same_v<T, SummonEntity>) up_fixed(c.position);
            else if constexpr (std::is_same_v<T, PlayerUse> || std::is_same_v<T, PlayerBreak>) up(c.pos);
            else if constexpr (std::is_same_v<T, DropItem> || std::is_same_v<T, PlayerDeath>) up_fixed(c.position);
            else if constexpr (std::is_same_v<T, ContainerClick>) {
                if (c.container) up(*c.container);
                up_fixed(c.drop_position);
            } else if constexpr (std::is_same_v<T, CloseContainer>) up_fixed(c.drop_position);
        }, b);
    };
    auto inner = s.inputs;
    s.inputs = [inner, lift](Tick t) {
        auto out = inner(t);
        for (auto& b : out) lift(b);
        return out;
    };
    s.name += std::format("+{}", dy);
    return s;
}

// Two scenarios at once, `a`'s commands in level `la` and then `b`'s in `lb`
// each tick. `b`'s ticket ids are raised by 1000: ticket ids are unique over
// all levels.
inline Scenario in_levels(const Scenario& a, DimensionId la, const Scenario& b, DimensionId lb) {
    const auto offset = [](CommandBody& c) {
        if (auto* add = std::get_if<AddTicket>(&c)) add->ticket += 1000;
        if (auto* remove = std::get_if<RemoveTicket>(&c)) remove->ticket += 1000;
    };
    Scenario s{std::format("{}@{}+{}@{}", a.name, dimension_name(la), b.name, dimension_name(lb)), a.seed,
               std::max(a.ticks, b.ticks), {}, std::min(a.dump_every, b.dump_every), {}};
    const auto first = [a](Tick t) { return t <= a.ticks ? a.inputs(t) : std::vector<CommandBody>{}; };
    const auto second = [b, offset](Tick t) {
        auto out = t <= b.ticks ? b.inputs(t) : std::vector<CommandBody>{};
        for (auto& c : out) offset(c);
        return out;
    };
    s.inputs = [first, second](Tick t) {
        auto out = first(t);
        for (auto& c : second(t)) out.push_back(std::move(c));
        return out;
    };
    s.levels = [first, second, la, lb](Tick t) {
        std::vector<DimensionId> out(first(t).size(), la);
        out.resize(out.size() + second(t).size(), lb);
        return out;
    };
    return s;
}

// Sand, gravel and concrete powder falling across the x=16 chunk edge: columns
// whose support goes, blocks dropped onto slabs (their items then merge),
// into water (powder becomes concrete) and onto a hopper.
inline Scenario falling_scenario() {
    return {"falling_blocks", 83, 160, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        const auto sand = registry_block("sand"), gravel = registry_block("gravel");
        const auto powder = registry_block("white_concrete_powder");
        const auto slab = registry_block("stone_slab", {{"type", "bottom"}, {"waterlogged", "false"}});
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = 0; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({14, 0, 4}, slab));
            c.push_back(place({17, 0, 4}, slab));
            c.push_back(place({12, 0, 6}, blocks::water_source()));
            c.push_back(place({20, 0, 6}, blocks::hopper(Direction::down)));
        }
        const auto phase = t % 40;
        if (phase == 5) {
            // A column on a support at each side of the edge.
            for (const int x : {15, 16}) {
                c.push_back(place({x, 0, 1}, blocks::stone()));
                for (int y = 1; y <= 4; ++y) c.push_back(place({x, y, 1}, y % 2 ? sand : gravel));
            }
        }
        if (phase == 15) {
            c.push_back(place({15, 0, 1}, blocks::air()));
            c.push_back(place({16, 0, 1}, blocks::air()));
        }
        if (phase == 20) {
            c.push_back(place({14, 6, 4}, sand));
            c.push_back(place({17, 7, 4}, sand));
            c.push_back(place({12, 5, 6}, powder));
            c.push_back(place({20, 6, 6}, gravel));
        }
        if (phase == 35) {
            // Clear what landed, so that the next round falls again.
            for (const int x : {15, 16})
                for (int y = 0; y <= 4; ++y) c.push_back(place({x, y, 1}, blocks::air()));
            c.push_back(place({12, 0, 6}, blocks::water_source()));
            c.push_back(place({20, 1, 6}, blocks::air()));
        }
        return c;
    }, 5};
}

// TNT primed on both sides of the x=16 chunk edge: by one dust line (both
// ends in the same tick), by a piston pushing TNT under a redstone block
// across the edge, by a repeater's block tick, and above water it falls into.
// The fuses run out and the TNT explodes, breaking what is around it.
inline Scenario tnt_scenario() {
    return {"primed_tnt", 97, 260, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        const auto tnt = registry_block("tnt");
        const auto dust = blocks::wire();
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -3; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({10, 0, 3}, blocks::water_source()));
            c.push_back(place({18, 0, 5}, blocks::repeater(Direction::east)));
        }
        const auto phase = t % 100;
        if (phase == 5) {
            c.push_back(place({14, 0, 0}, tnt));
            c.push_back(place({15, 0, 0}, dust));
            c.push_back(place({16, 0, 0}, dust));
            c.push_back(place({17, 0, 0}, tnt));
            c.push_back(place({14, 0, -2}, blocks::piston(Direction::east)));
            c.push_back(place({15, 0, -2}, tnt));
            c.push_back(place({16, 1, -2}, blocks::redstone_block()));
            c.push_back(place({12, 2, 3}, tnt));
            c.push_back(place({19, 0, 5}, tnt));
        }
        if (phase == 10) {
            // Above the dust: it powers the line without changing its shape.
            c.push_back(place({15, 1, 0}, blocks::redstone_block()));
            c.push_back(place({13, 0, -2}, blocks::redstone_block()));
            c.push_back(place({12, 3, 3}, blocks::redstone_block()));
            c.push_back(place({17, 0, 5}, blocks::redstone_block()));
        }
        if (phase == 95) {
            // Clear the sources and the piston for the next round.
            for (const BlockPos p : {BlockPos{15, 1, 0}, BlockPos{13, 0, -2}, BlockPos{12, 3, 3}, BlockPos{17, 0, 5},
                                     BlockPos{14, 0, -2}, BlockPos{16, 1, -2}})
                c.push_back(place(p, blocks::air()));
        }
        return c;
    }, 5};
}

// Arrows and snowballs flying across the x=16 chunk edge into walls on both
// sides; burning arrows prime TNT in the other chunk; a wall goes and its
// arrows fall.
inline Scenario projectile_scenario() {
    return {"projectiles", 101, 150, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        const auto tnt = registry_block("tnt");
        if (t == 2) {
            for (int x = 6; x <= 26; ++x)
                for (int z = -4; z <= 4; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            for (int y = 0; y <= 4; ++y)
                for (int z = -4; z <= 4; ++z) {
                    c.push_back(place({7, y, z}, blocks::obsidian()));
                    c.push_back(place({25, y, z}, blocks::obsidian()));
                }
        }
        const auto phase = t % 50;
        if (phase == 5) {
            c.push_back(place({24, 0, 2}, tnt));
            c.push_back(place({8, 0, -2}, tnt));
            for (int i = 0; i < 4; ++i) {
                const double z = -3.5 + 2.0 * i;
                c.push_back(SummonEntity{"minecraft:arrow", {12.5, 1.5, z}, {1.1, 0.2 + 0.05 * i, 0.01 * i}, 80, 0});
                c.push_back(SummonEntity{"minecraft:arrow", {20.5, 2.5, z + 0.5}, {-1.0, 0.1, -0.02 * i}, 80, 0});
            }
            c.push_back(SummonEntity{"minecraft:arrow", {19.5, 0.5, 2.5}, {1.0, 0.05, 0.0}, 80, 300});
            c.push_back(SummonEntity{"minecraft:arrow", {13.5, 0.5, -1.5}, {-1.0, 0.05, 0.0}, 80, 300});
            c.push_back(SummonEntity{"minecraft:snowball", {15.5, 3.5, 0.5}, {0.9, 0.2, 0.0}, 80, 0});
        }
        if (phase == 30) {
            // The east wall goes; its arrows fall. It comes back later.
            for (int y = 0; y <= 4; ++y)
                for (int z = -4; z <= 4; ++z) c.push_back(place({25, y, z}, blocks::air()));
        }
        if (phase == 45)
            for (int y = 0; y <= 4; ++y)
                for (int z = -4; z <= 4; ++z) c.push_back(place({25, y, z}, blocks::obsidian()));
        return c;
    }, 5};
}

// Boats drifting down a channel of flowing water across the x=16 chunk edge
// and pushing each other; minecarts sliding off rails across it.
inline Scenario vehicle_scenario() {
    return {"vehicles", 103, 160, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 8; x <= 26; ++x)
                for (int z = -5; z <= 5; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            for (int x = 9; x <= 25; ++x) {
                c.push_back(place({x, 0, -4}, blocks::stone()));
                c.push_back(place({x, 0, 0}, blocks::stone()));
            }
            for (int z = -3; z <= -1; ++z) {
                c.push_back(place({9, 0, z}, blocks::stone()));
                c.push_back(place({10, 0, z}, blocks::water_source()));
            }
        }
        const auto phase = t % 60;
        if (phase == 30) {
            c.push_back(SummonEntity{"minecraft:oak_boat", {11.5, 0.5, -2.0}, {0.0, 0.0, 0.0}, 80, 0});
            c.push_back(SummonEntity{"minecraft:birch_boat", {12.8, 2.5, -2.0}, {0.05, 0.0, 0.0}, 80, 0});
            c.push_back(SummonEntity{"minecraft:minecart", {12.5, 0.0, 2.5}, {0.8, 0.0, 0.02}, 80, 0});
            c.push_back(SummonEntity{"minecraft:minecart", {20.5, 3.0, 3.5}, {-0.6, 0.2, 0.0}, 80, 0});
        }
        return c;
    }, 5};
}

// Redstone torches and buttons across the x=16 chunk edge: a torch that
// turns itself off through dust and a repeater in the other chunk burns out
// and relights; a lever turns a torch off and on; a stone button powers a
// block in the other chunk; arrows press an oak button and hold it until
// its block goes.
inline Scenario torch_button_scenario() {
    return {"torches_buttons", 107, 300, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        const auto oak = blocks::button(blocks::ButtonKind::oak, false, Direction::east);
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -4; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            // The burnout loop: torch, dust, repeater into the torch's block.
            c.push_back(place({15, 0, 0}, blocks::stone()));
            c.push_back(place({15, 0, 1}, blocks::repeater(Direction::north)));
            for (const BlockPos p : {BlockPos{16, 0, 1}, BlockPos{16, 0, 2}, BlockPos{15, 0, 2}}) c.push_back(place(p, blocks::wire()));
            c.push_back(place({12, 0, 4}, blocks::stone()));
            c.push_back(place({12, 1, 4}, blocks::lever(false, Direction::north, blocks::LeverFace::floor)));
            c.push_back(place({14, 0, 4}, blocks::lamp()));
            c.push_back(place({15, 0, 5}, blocks::stone()));
            c.push_back(place({14, 0, 5}, blocks::lamp()));
            c.push_back(place({16, 0, 5}, blocks::button(blocks::ButtonKind::stone, false, Direction::east)));
            c.push_back(place({15, 1, -2}, blocks::stone()));
            c.push_back(place({15, 2, -2}, blocks::lamp()));
        }
        if (t == 5) {
            c.push_back(place({16, 0, 0}, blocks::wall_torch(Direction::east)));
            c.push_back(place({13, 0, 4}, blocks::wall_torch(Direction::east)));
        }
        if (t >= 10 && t < 60 && t % 3 == 0) c.push_back(UseBlock{{12, 1, 4}});
        const auto phase = t % 100;
        if (phase == 20 || phase == 45) c.push_back(UseBlock{{16, 0, 5}});
        if (phase == 30) {
            c.push_back(place({16, 1, -2}, oak));
            c.push_back(SummonEntity{"minecraft:arrow", {20.5, 1.5, -1.5}, {-1.0, 0.04, 0.0}, 80, 0});
        }
        if (phase == 80) c.push_back(place({15, 1, -2}, blocks::air()));
        if (phase == 90) c.push_back(place({15, 1, -2}, blocks::stone()));
        return c;
    }, 5};
}

// Pressure plates and target blocks across the x=16 chunk edge: items slide
// from one chunk over a wooden plate in the other; items whose positions lie
// in the other chunk reach into weighted plates; arrows and snowballs fly
// across the edge into target blocks.
inline Scenario plate_target_scenario() {
    return {"plates_targets", 109, 240, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -4; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({16, 0, 0}, blocks::pressure_plate(blocks::ButtonKind::oak)));
            c.push_back(place({16, 0, 1}, blocks::lamp()));
            c.push_back(place({15, 0, 3}, blocks::weighted_plate(false)));
            c.push_back(place({14, 0, 3}, blocks::wire()));
            c.push_back(place({17, 0, 3}, blocks::weighted_plate(true)));
            c.push_back(place({15, 0, -3}, blocks::target()));
            c.push_back(place({14, 0, -3}, blocks::wire()));
            c.push_back(place({17, 0, -1}, blocks::target()));
            c.push_back(place({18, 0, -1}, blocks::wire()));
        }
        const auto phase = t % 60;
        if (phase == 5) {
            c.push_back(SpawnItem{at_block(14.5, 0.0, 0.5), {1.5, 0.0, 0.0}, {items::stone, 1}});
            for (int i = 0; i < 3; ++i) c.push_back(SpawnItem{at_block(16.05, 0.2 + 0.3 * i, 3.3 + 0.2 * i), {}, {items::dirt, 1}});
            for (int i = 0; i < 4; ++i) c.push_back(SpawnItem{at_block(17.2 + 0.2 * i, 0.3, 3.5), {}, {items::sand, 1}});
            c.push_back(SummonEntity{"minecraft:arrow", {20.5, 0.73, -2.5}, {-1.0, 0.04, 0.0}, 80, 0});
            c.push_back(SummonEntity{"minecraft:snowball", {12.5, 0.6, -0.7}, {1.0, 0.1, 0.0}, 80, 0});
        }
        return c;
    }, 5};
}

// Doors, trapdoors and fence gates across the x=16 chunk edge: a lever in one
// chunk opens a door in the other, dust lines carry redstone blocks' power to
// a trapdoor and a fence gate across the edge, and items on the trapdoor
// fall through it when it opens.
inline Scenario door_scenario() {
    using blocks::DoorKind;
    return {"doors", 111, 200, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -4; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({16, 0, 0}, blocks::door(DoorKind::oak, Direction::east, false)));
            c.push_back(place({16, 1, 0}, blocks::door(DoorKind::oak, Direction::east, true)));
            c.push_back(place({15, 0, 0}, blocks::lever(false)));
            for (int x = 12; x <= 15; ++x) {
                c.push_back(place({x, 0, 3}, blocks::wire()));
                c.push_back(place({x, 0, 5}, blocks::wire()));
            }
            c.push_back(place({16, 0, 3}, blocks::trapdoor(DoorKind::spruce, Direction::north)));
            c.push_back(place({16, 0, 5}, blocks::fence_gate(DoorKind::birch, Direction::north)));
            c.push_back(place({16, -1, 3}, blocks::air()));
        }
        if (t >= 5 && t % 20 == 5) c.push_back(UseBlock{{15, 0, 0}});
        for (const int z : {3, 5}) {
            if (t % 40 == 10) c.push_back(place({11, 0, z}, blocks::redstone_block()));
            if (t % 40 == 30) c.push_back(place({11, 0, z}, blocks::air()));
        }
        if (t % 40 == 20) c.push_back(SpawnItem{at_block(16.5, 0.5, 3.5), {}, {items::stone, 1}});
        return c;
    }, 5};
}

// Honey across the x=16 chunk edge: a sticky piston moves a honey block with
// an item on it between the chunks, a pushed slime block leaves the honey
// beside it, and items and primed TNT in one chunk slide down a honey wall
// in the other.
inline Scenario honey_scenario() {
    return {"honey", 113, 200, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -4; z <= 8; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({14, 0, 0}, blocks::piston(Direction::east, true)));
            c.push_back(place({15, 0, 0}, blocks::honey()));
            c.push_back(place({13, 0, 0}, blocks::lever(false)));
            c.push_back(place({14, 0, -3}, blocks::piston(Direction::east)));
            c.push_back(place({15, 0, -3}, blocks::slime()));
            c.push_back(place({15, 0, -2}, blocks::honey()));
            c.push_back(place({15, 0, -4}, blocks::stone()));
            c.push_back(place({13, 0, -3}, blocks::lever(false)));
            for (int y = 0; y <= 4; ++y)
                for (int z = 3; z <= 7; ++z) c.push_back(place({16, y, z}, blocks::honey()));
        }
        if (t == 4) c.push_back(SpawnItem{at_block(15.5, 0.9375, 0.5), {}, {items::stone, 1}});
        if (t >= 10 && t % 20 == 10) c.push_back(UseBlock{{13, 0, 0}});
        if (t == 30) c.push_back(UseBlock{{13, 0, -3}});
        if (t % 50 == 5) {
            c.push_back(SpawnItem{at_block(15.93, 7.5, 3.5), {}, {items::dirt, 1}});
            c.push_back(SpawnItem{at_block(15.93, 8.5, 5.5), {0.0, 0.0, 0.1}, {items::sand, 1}});
            c.push_back(SummonEntity{"minecraft:tnt", {15.53, 7.5, 6.5}, {}, 400, 0});
        }
        return c;
    }, 5};
}

// Pistons moving blocks without rules of their own across the x=16 chunk
// edge: a sticky piston pushes a line of dirt, cobblestone and glazed
// terracotta over it and pulls the dirt back, another destroys a dandelion,
// a third is held by an enchanting table.
inline Scenario push_class_scenario() {
    return {"push_classes", 115, 160, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -4; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({12, 0, 0}, blocks::piston(Direction::east, true)));
            c.push_back(place({13, 0, 0}, registry_block("dirt")));
            c.push_back(place({14, 0, 0}, registry_block("cobblestone")));
            c.push_back(place({15, 0, 0}, registry_block("white_glazed_terracotta", {{"facing", "north"}})));
            c.push_back(place({11, 0, 0}, blocks::lever(false)));
            c.push_back(place({14, 0, 3}, blocks::piston(Direction::east)));
            c.push_back(place({15, 0, 3}, registry_block("cobblestone")));
            c.push_back(place({16, 0, 3}, registry_block("dandelion")));
            c.push_back(place({13, 0, 3}, blocks::lever(false)));
            c.push_back(place({15, 0, 5}, blocks::piston(Direction::east)));
            c.push_back(place({16, 0, 5}, registry_block("enchanting_table")));
            c.push_back(place({14, 0, 5}, blocks::lever(false)));
        }
        if (t >= 10 && t % 20 == 10)
            for (const BlockPos p : {BlockPos{11, 0, 0}, BlockPos{13, 0, 3}, BlockPos{14, 0, 5}}) c.push_back(UseBlock{p});
        return c;
    }, 5};
}

// Daylight detectors across the x=16 chunk edge through an evening, a night
// and rain: a normal one with dust and an inverted one with a lamp on either
// side, and a roofed one used at night.
inline Scenario daylight_scenario() {
    return {"daylight", 116, 200, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 10; x <= 22; ++x)
                for (int z = -3; z <= 4; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({15, 0, 0}, blocks::daylight_detector()));
            c.push_back(place({14, 0, 0}, blocks::wire()));
            c.push_back(place({16, 0, 0}, blocks::daylight_detector(true)));
            c.push_back(place({17, 0, 0}, blocks::lamp()));
            c.push_back(place({15, 0, 2}, blocks::daylight_detector()));
            c.push_back(place({15, 1, 2}, blocks::stone()));
            c.push_back(SetDayTime{12500});
        }
        if (t == 60) c.push_back(SetDayTime{18000});
        if (t == 70) c.push_back(UseBlock{{15, 0, 2}});
        if (t == 110) c.push_back(SetDayTime{6000});
        if (t == 120) c.push_back(SetWeather{WeatherKind::thunder, 100000});
        return c;
    }, 10};
}

// Note blocks across the x=16 chunk edge: one lever playing a note block on
// either side, uses, a piston pushing a note block over the edge onto gold,
// and a head set above one.
inline Scenario note_scenario() {
    return {"note_blocks", 117, 120, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 10; x <= 22; ++x)
                for (int z = -3; z <= 4; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({15, 0, 0}, blocks::note_block()));
            c.push_back(place({17, 0, 0}, blocks::note_block()));
            c.push_back(place({16, 0, 0}, blocks::lever(false)));
            c.push_back(place({14, 0, 2}, blocks::piston(Direction::east)));
            c.push_back(place({15, 0, 2}, blocks::note_block()));
            c.push_back(place({16, -1, 2}, registry_block("gold_block")));
            c.push_back(place({13, 0, 2}, blocks::lever(false)));
        }
        if (t >= 10 && t % 10 == 0) c.push_back(UseBlock{{16, 0, 0}});
        if (t >= 10 && t % 7 == 3) c.push_back(UseBlock{{17, 0, 0}});
        if (t == 40) c.push_back(UseBlock{{13, 0, 2}});
        if (t == 60) c.push_back(place({15, 1, 0}, registry_block("zombie_head", {{"powered", "false"}, {"rotation", "0"}})));
        return c;
    }, 10};
}

// Rails across the x=16 chunk edge: a line laid a rail a tick over the edge
// and turning south, powered rails on both sides of the edge on one lever, a
// junction at the edge switched by another, and a piston pushing a rail over
// the edge to the end of a line.
inline Scenario rail_scenario() {
    using blocks::RailKind;
    using blocks::RailShape;
    return {"rails", 118, 100, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2)
            for (int x = 8; x <= 24; ++x)
                for (int z = -4; z <= 8; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
        if (t >= 4 && t <= 12) c.push_back(place({static_cast<std::int32_t>(8 + t), 0, 0}, blocks::rail()));
        if (t == 13) c.push_back(place({20, 0, 1}, blocks::rail()));
        if (t == 14) {
            for (int x = 12; x <= 20; ++x) c.push_back(place({x, 0, 3}, blocks::rail(RailKind::powered, RailShape::east_west)));
            c.push_back(place({11, 0, 3}, blocks::lever(false)));
            c.push_back(place({15, 0, 5}, blocks::rail(RailKind::rail, RailShape::east_west)));
            c.push_back(place({17, 0, 5}, blocks::rail(RailKind::rail, RailShape::east_west)));
            c.push_back(place({16, 0, 6}, blocks::rail()));
            c.push_back(place({14, 0, -2}, blocks::piston(Direction::east)));
            c.push_back(place({15, 0, -2}, blocks::rail()));
            c.push_back(place({17, 0, -2}, blocks::rail(RailKind::rail, RailShape::east_west)));
            c.push_back(place({18, 0, -2}, blocks::rail(RailKind::rail, RailShape::east_west)));
            c.push_back(place({13, 0, -2}, blocks::lever(false)));
        }
        if (t == 15) c.push_back(place({16, 0, 5}, blocks::rail()));
        if (t == 16) c.push_back(place({16, 0, 4}, blocks::lever(false)));
        if (t >= 20 && t % 20 == 0) c.push_back(UseBlock{{11, 0, 3}});
        if (t >= 30 && t % 20 == 10) c.push_back(UseBlock{{16, 0, 4}});
        if (t == 25) c.push_back(UseBlock{{13, 0, -2}});
        return c;
    }, 10};
}

// Minecarts riding rails over the x=16 chunk edge: one fast along a line
// with a slope on the far side, one through a curve at the edge, one boosted
// by powered rails on both sides of it.
inline Scenario minecart_rail_scenario() {
    using blocks::RailKind;
    using blocks::RailShape;
    return {"minecart_rails", 119, 120, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 6; x <= 26; ++x)
                for (int z = -6; z <= 8; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            // A line from x=8 rising at x=20..21 onto stone.
            for (int x = 20; x <= 24; ++x)
                for (int y = 0; y < std::min(x - 19, 2); ++y) c.push_back(place({x, y, 0}, blocks::stone()));
            for (int x = 8; x <= 24; ++x) c.push_back(place({x, x < 20 ? 0 : std::min(x - 19, 2), 0}, blocks::rail()));
            // A line east to x=16 turning south.
            for (int x = 8; x <= 16; ++x) c.push_back(place({x, 0, 4}, blocks::rail()));
            for (int z = 5; z <= 8; ++z) c.push_back(place({16, 0, z}, blocks::rail()));
            // Powered rails at x=14..18 on redstone blocks.
            for (int x = 8; x <= 24; ++x) {
                const bool powered = x >= 14 && x <= 18;
                if (powered) c.push_back(place({x, -1, -4}, blocks::redstone_block()));
                c.push_back(place({x, 0, -4}, blocks::rail(powered ? RailKind::powered : RailKind::rail, RailShape::north_south)));
            }
        }
        if (t == 4) {
            c.push_back(SummonEntity{"minecraft:minecart", {9.5, 0.0, 0.5}, {1.5, 0.0, 0.0}, 0, 0});
            c.push_back(SummonEntity{"minecraft:minecart", {9.5, 0.0, 4.5}, {1.5, 0.0, 0.0}, 0, 0});
            c.push_back(SummonEntity{"minecraft:minecart", {9.5, 0.0, -3.5}, {0.2, 0.0, 0.0}, 0, 0});
        }
        return c;
    }, 10};
}

// Minecarts meeting at the x=16 chunk edge: two head-on on one line, one
// running into a cart at rest in the other chunk.
inline Scenario minecart_meeting_scenario() {
    using blocks::RailKind;
    using blocks::RailShape;
    return {"minecart_meeting", 120, 80, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 6; x <= 26; ++x)
                for (int z = -2; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            for (int x = 7; x <= 25; ++x) {
                c.push_back(place({x, 0, 0}, blocks::rail(RailKind::rail, RailShape::east_west)));
                c.push_back(place({x, 0, 4}, blocks::rail(RailKind::rail, RailShape::east_west)));
            }
        }
        if (t == 4) {
            c.push_back(SummonEntity{"minecraft:minecart", {10.5, 0.0, 0.5}, {0.6, 0.0, 0.0}, 0, 0});
            c.push_back(SummonEntity{"minecraft:minecart", {22.5, 0.0, 0.5}, {-0.6, 0.0, 0.0}, 0, 0});
            c.push_back(SummonEntity{"minecraft:minecart", {12.5, 0.0, 4.5}, {0.5, 0.0, 0.0}, 0, 0});
            c.push_back(SummonEntity{"minecraft:minecart", {17.5, 0.0, 4.5}, {}, 0, 0});
        }
        return c;
    }, 10};
}

// Detector rails at the x=16 chunk edge: a cart crossing the edge over one
// on each side, a lamp beside the one past the edge, and one pressed by a
// cart at rest powering powered rails over the edge.
inline Scenario detector_rail_scenario() {
    using blocks::RailKind;
    using blocks::RailShape;
    return {"detector_rails", 121, 100, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 6; x <= 26; ++x)
                for (int z = -2; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            for (int x = 7; x <= 25; ++x) {
                const auto kind = x == 15 || x == 17 ? RailKind::detector : RailKind::rail;
                c.push_back(place({x, 0, 0}, blocks::rail(kind, RailShape::east_west)));
            }
            c.push_back(place({17, 0, 1}, blocks::lamp()));
            for (int x = 12; x <= 20; ++x) {
                const auto kind = x == 15 ? RailKind::detector : x > 15 ? RailKind::powered : RailKind::rail;
                c.push_back(place({x, 0, 4}, blocks::rail(kind, RailShape::east_west)));
            }
            c.push_back(place({14, 0, 4}, blocks::stone()));
        }
        if (t == 4) {
            c.push_back(SummonEntity{"minecraft:minecart", {9.5, 0.0, 0.5}, {0.5, 0.0, 0.0}, 0, 0});
            c.push_back(SummonEntity{"minecraft:minecart", {15.5, 0.0, 4.5}, {}, 0, 0});
        }
        return c;
    }, 10};
}

// Coral fans at the x=16 chunk edge: living fans dying dry on either side,
// one kept alive by water in the other chunk until the water goes, and a
// piston breaking a living fan over the edge.
inline Scenario coral_scenario() {
    using blocks::CoralKind;
    return {"coral_fans", 122, 240, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -2; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({15, 0, 0}, blocks::coral_fan(CoralKind::brain)));
            c.push_back(place({16, 0, 0}, blocks::coral_fan(CoralKind::bubble)));
            c.push_back(place({16, 0, 3}, blocks::water_source()));
            for (const BlockPos p : {BlockPos{17, 0, 3}, BlockPos{16, 0, 2}, BlockPos{16, 0, 4}, BlockPos{16, 1, 3}}) c.push_back(place(p, blocks::glass()));
            c.push_back(place({15, 0, 3}, blocks::coral_fan(CoralKind::horn)));
            c.push_back(place({14, 0, 5}, blocks::piston(Direction::east)));
            c.push_back(place({15, 0, 5}, blocks::coral_fan(CoralKind::fire, false, true)));
            c.push_back(place({13, 0, 5}, blocks::lever(false)));
        }
        if (t == 50) c.push_back(place({16, 0, 3}, blocks::glass()));
        if (t == 60) c.push_back(UseBlock{{13, 0, 5}});
        return c;
    }, 10};
}

// Dispensers and droppers at the x=16 chunk edge, fired by levers: a dropper
// into a hopper over the edge that feeds a chest, a dispenser ejecting stone
// over the edge read by a comparator, one pouring water and taking it back
// over the edge, and one setting a minecart on a rail over the edge.
inline Scenario dispenser_scenario() {
    using blocks::RailKind;
    using blocks::RailShape;
    return {"dispensers", 123, 120, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -2; z <= 10; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            c.push_back(place({15, 0, 0}, blocks::dispenser(Direction::east, true)));
            c.push_back(place({16, 0, 0}, blocks::hopper(Direction::south)));
            c.push_back(place({16, 0, 1}, blocks::chest()));
            c.push_back(place({15, 0, 3}, blocks::dispenser(Direction::east)));
            c.push_back(place({14, 0, 3}, blocks::comparator(Direction::west)));
            c.push_back(place({13, 0, 3}, blocks::wire()));
            c.push_back(place({15, 0, 6}, blocks::dispenser(Direction::east)));
            for (const BlockPos p : {BlockPos{17, 0, 6}, BlockPos{16, 0, 5}, BlockPos{16, 0, 7}}) c.push_back(place(p, blocks::stone()));
            c.push_back(place({17, 0, 9}, blocks::dispenser(Direction::west)));
            c.push_back(place({16, 0, 9}, blocks::rail(RailKind::rail, RailShape::east_west)));
            c.push_back(place({15, 0, 9}, blocks::stone()));
            for (const int z : {0, 6}) c.push_back(place({14, 0, z}, blocks::lever(false)));
            c.push_back(place({15, 0, 4}, blocks::lever(false)));
            c.push_back(place({18, 0, 9}, blocks::lever(false)));
        }
        if (t == 3) {
            c.push_back(insert({15, 0, 0}, 0, items::stone, 3));
            c.push_back(insert({15, 0, 3}, 0, items::stone, 2));
            c.push_back(insert({15, 0, 3}, 4, items::glass, 2));
            c.push_back(insert({15, 0, 6}, 0, items::water_bucket, 1));
            c.push_back(insert({17, 0, 9}, 0, items::minecart, 1));
        }
        if (t % 10 == 0 && t >= 10 && t <= 60)
            for (const BlockPos p : {BlockPos{14, 0, 0}, BlockPos{15, 0, 4}, BlockPos{14, 0, 6}, BlockPos{18, 0, 9}}) c.push_back(UseBlock{p});
        return c;
    }, 10};
}

// The Mytteri push duper (TNT duplication; minecraft.wiki "Tutorial:Block and
// item duplication") at the x=16 chunk edge, pushed west over it six times by
// a piston set behind it each time: every push after the first primes the
// TNT while its block moves on, and the primed TNT falls out of the world.
inline Scenario tnt_duper_scenario() {
    using blocks::CoralKind;
    using blocks::RailKind;
    using blocks::RailShape;
    return {"tnt_duper", 124, 220, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        constexpr int x0 = 19, z = 2; // the top slime's column
        if (t == 2) {
            c.push_back(place({x0 - 2, 1, z}, blocks::stone()));
            c.push_back(place({x0 - 1, 1, z}, blocks::slime()));
            c.push_back(place({x0, 1, z}, blocks::slime()));
            c.push_back(place({x0 - 1, 2, z}, blocks::rail(RailKind::detector, RailShape::east_west)));
            c.push_back(place({x0, 2, z}, blocks::slime()));
            c.push_back(place({x0 - 2, 0, z}, registry_block("tnt")));
            c.push_back(place({x0 - 2, 1, z - 1}, blocks::slime()));
            c.push_back(place({x0 - 3, 0, z - 1}, blocks::slime()));
            c.push_back(place({x0 - 2, 0, z - 1}, blocks::slime()));
            c.push_back(place({x0 - 3, 0, z}, blocks::coral_wall_fan(CoralKind::tube, Direction::south, true)));
        }
        if (t == 3) c.push_back(SummonEntity{"minecraft:minecart", {x0 - 0.5, 2.0625, z + 0.5}, {}, 0, 0});
        for (int k = 0; k < 6; ++k) {
            const Tick at = 10 + 20 * static_cast<Tick>(k);
            const int x = x0 + 1 - k;
            if (t == at - 1) c.push_back(place({x, 3, z}, blocks::redstone_block()));
            if (t == at) c.push_back(place({x, 2, z}, blocks::piston(Direction::west)));
            if (t == at + 9) c.push_back(place({x, 3, z}, blocks::air()));
        }
        return c;
    }, 10};
}

// Two-way flying engine A (minecraft.wiki "Tutorial:Flying machines/Java
// Engine Gallery") flying east over the x=16 chunk edge into an obsidian wall:
// slime blocks with observers on top that power them, and two sticky pistons
// facing each other's slime block.
inline Scenario flying_engine_scenario() {
    return {"flying_engine", 125, 220, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            c.push_back(place({10, 0, 2}, blocks::slime()));
            c.push_back(place({11, 0, 3}, blocks::slime()));
            c.push_back(place({11, 0, 2}, blocks::piston(Direction::west, true)));
            c.push_back(place({10, 0, 3}, blocks::piston(Direction::east, true)));
            c.push_back(place({10, 1, 2}, blocks::observer(Direction::up)));
            c.push_back(place({11, 1, 3}, blocks::observer(Direction::up)));
            for (int z = 1; z <= 4; ++z)
                for (int y = 0; y <= 1; ++y) c.push_back(place({24, y, z}, blocks::obsidian()));
        }
        // A block set above the first observer starts it eastward.
        if (t == 10) c.push_back(place({10, 2, 2}, blocks::stone()));
        return c;
    }, 10};
}

// Two slices of the basic hopper item sorter (minecraft.wiki "Tutorial:Hopper")
// on either side of the z=16 chunk edge, sorting diamonds and iron from a chest
// through a hopper line that ends in an overflow chest.
inline Scenario item_sorter_scenario() {
    return {"item_sorter", 126, 420, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        constexpr std::array<int, 2> zs{15, 16};
        if (t == 2) {
            for (const int z : zs) {
                // Columns x = 4..8 as the wiki's side view, rows y = 1..4.
                c.push_back(place({5, 1, z}, blocks::stone()));
                c.push_back(place({4, 2, z}, blocks::stone()));
                c.push_back(place({6, 2, z}, blocks::stone()));
                c.push_back(place({5, 3, z}, blocks::stone()));
                c.push_back(place({6, 3, z}, blocks::stone()));
                c.push_back(place({7, 3, z}, blocks::stone()));
                c.push_back(place({5, 2, z}, blocks::repeater(Direction::east)));
                c.push_back(place({7, 2, z}, blocks::wall_torch(Direction::east)));
                c.push_back(place({4, 3, z}, blocks::wire()));
                c.push_back(place({5, 4, z}, blocks::wire()));
                c.push_back(place({6, 4, z}, blocks::wire()));
                c.push_back(place({7, 4, z}, blocks::comparator(Direction::west)));
                c.push_back(place({8, 3, z}, blocks::hopper(Direction::east)));
                c.push_back(place({9, 3, z}, blocks::chest()));
                c.push_back(place({8, 4, z}, blocks::hopper(Direction::west)));
                c.push_back(place({8, 5, z}, blocks::hopper(Direction::south)));
            }
            c.push_back(place({8, 6, zs[0]}, blocks::chest()));
            c.push_back(place({8, 5, zs[1] + 1}, blocks::chest()));
        }
        if (t == 3) {
            c.push_back(insert({8, 4, zs[0]}, 0, items::diamond, 41));
            c.push_back(insert({8, 4, zs[1]}, 0, items::iron_ingot, 41));
            for (std::uint8_t s = 1; s < 5; ++s)
                for (const int z : zs) c.push_back(insert({8, 4, z}, s, items::stick, 1));
        }
        if (t == 5) {
            c.push_back(insert({8, 6, zs[0]}, 0, items::diamond, 6));
            c.push_back(insert({8, 6, zs[0]}, 1, items::cobblestone, 4));
            c.push_back(insert({8, 6, zs[0]}, 2, items::iron_ingot, 6));
        }
        return c;
    }, 10};
}

// Lava spreading across a chunk border, meeting water and receding.
inline Scenario lava_scenario() {
    return {"lava_water", 41, 400, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2) {
            std::vector<CommandBody> b;
            for (int x = 8; x < 24; ++x)
                for (int z = 8; z < 24; ++z) b.push_back(place({x, 0, z}, blocks::stone()));
            return b;
        }
        if (t == 3) return {place({15, 1, 15}, registry_block("lava", {{"level", "0"}})), place({19, 1, 12}, blocks::water_source())};
        if (t == 30) return {place({17, 1, 19}, registry_block("lava", {{"level", "0"}}))};
        if (t == 150) return {place({15, 1, 15}, blocks::air())};
        return {};
    }};
}

// Waterlogged blocks giving their water to their open sides after shape
// updates, which come again just before each quarter of the run.
inline Scenario waterlogged_scenario() {
    return {"waterlogged", 43, 120, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 2)};
        if (t == 2) {
            std::vector<CommandBody> b;
            for (int x = 8; x < 24; ++x)
                for (int z = 8; z < 24; ++z) b.push_back(place({x, 0, z}, blocks::stone()));
            return b;
        }
        if (t == 3)
            return {place({12, 1, 12}, registry_block("oak_slab", {{"type", "bottom"}, {"waterlogged", "true"}})),
                    place({19, 1, 19}, registry_block("oak_stairs", {{"facing", "north"}, {"waterlogged", "true"}})),
                    place({16, 1, 20}, blocks::trapdoor(blocks::DoorKind::oak, D::north, false, false).with_level(2))};
        if (t % 30 == 28) return {place({12, 1, 13}, blocks::glass()), place({19, 1, 20}, blocks::glass()), place({16, 1, 21}, blocks::glass())};
        if (t % 30 == 29) return {place({12, 1, 13}, blocks::air()), place({19, 1, 20}, blocks::air()), place({16, 1, 21}, blocks::air())};
        return {};
    }};
}

// Fire across the x=16 chunk edge on a hard world: planks, wool and TNT
// burning while a player stands near, nothing but rescheduling once the
// player leaves, and burning again with allowFireTicksAwayFromPlayer.
inline Scenario fire_scenario() {
    return {"fire", 61, 300, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1), SetDifficulty{3}, PlayerPresence{{1, 1}, at_block(16.5, 0.0, 2.5)}};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 8; x <= 24; ++x)
                for (int z = -3; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            for (int x = 10; x <= 22; ++x)
                for (int z = 0; z <= 1; ++z) c.push_back(place({x, 0, z}, registry_block("oak_planks")));
            for (int x = 14; x <= 18; ++x) c.push_back(place({x, 0, 3}, registry_block("white_wool")));
            c.push_back(place({16, 0, 4}, registry_block("tnt")));
            c.push_back(place({15, 1, 1}, registry_block("oak_fence")));
        }
        if (t == 5) {
            c.push_back(place({12, 0, 2}, registry_block("fire")));
            c.push_back(place({20, 0, 2}, registry_block("fire")));
            c.push_back(place({17, 1, 3}, registry_block("fire")));
            c.push_back(place({22, 3, 5}, registry_block("fire"))); // in the air: gone at once
        }
        // Items and an arrow into the fires and a lava pool on the chunk edge.
        if (t == 6) {
            for (const BlockPos p : {BlockPos{15, 0, 6}, BlockPos{17, 0, 6}, BlockPos{16, 0, 5}, BlockPos{16, 0, 7}}) c.push_back(place(p, blocks::stone()));
            c.push_back(place({16, 0, 6}, registry_block("lava", {{"level", "0"}})));
            c.push_back(SpawnItem{at_block(12.5, 2.0, 2.5), {}, {items::redstone, 1}});
            c.push_back(SpawnItem{at_block(16.5, 1.5, 6.5), {}, {items::stone, 1}});
            c.push_back(SpawnItem{at_block(19.4, 0.0, 2.5), {0.2, 0.0, 0.0}, {items::glass, 1}});
            c.push_back(SummonEntity{"minecraft:arrow", at_block(10.5, 0.5, 2.5), {0.6, 0.0, 0.0}, 0, 0});
        }
        if (t == 150) c.push_back(PlayerPresence{{1, 1}, {}, false, false});
        if (t == 200) c.push_back(SetGameRule{"allowFireTicksAwayFromPlayer", 1});
        return c;
    }, 10};
}

// Frosted ice across the x=16 chunk edge aging and melting in its ticks and
// by its neighbors, half of it under a roof; snow layers losing their
// support and grass under snow.
inline Scenario snow_scenario() {
    return {"snow", 67, 320, [](Tick t) -> std::vector<CommandBody> {
        if (t == 1) return {ticket(1, {0, 0}, 1)};
        std::vector<CommandBody> c;
        if (t == 2) {
            for (int x = 10; x <= 22; ++x)
                for (int z = -2; z <= 6; ++z) c.push_back(place({x, -1, z}, blocks::stone()));
            for (int x = 12; x <= 20; ++x)
                for (int z = 0; z <= 2; ++z) c.push_back(place({x, 0, z}, registry_block("frosted_ice", {{"age", "0"}})));
            for (int x = 16; x <= 20; ++x)
                for (int z = -1; z <= 3; ++z) c.push_back(place({x, 2, z}, blocks::stone()));
            for (int x = 12; x <= 20; x += 2) c.push_back(place({x, -1, 5}, registry_block(x % 4 == 0 ? "ice" : "grass_block", {})));
        }
        if (t == 3)
            for (int x = 12; x <= 20; x += 2) c.push_back(place({x, 0, 5}, registry_block("snow", {{"layers", "2"}})));
        if (t == 40)
            for (int x = 13; x <= 21; x += 2) c.push_back(place({x, 0, 5}, blocks::stone()));
        if (t == 150) c.push_back(place({16, 0, 1}, blocks::stone()));
        return c;
    }, 10};
}

inline std::vector<Scenario> all_scenarios() {
    return {slice_scenario(), clock_scenario(), piston_scenario(), hopper_scenario(), water_scenario(),
            wheat_scenario(), many_machines_scenario(), ticket_scenario(), piston_chain_scenario(),
            comparator_chest_scenario(), piston_pulse_scenario(), drops_scenario(), observed_piston_scenario(),
            transient_head_scenario(), registry_block_scenario(), chunk_levels_scenario(), crop_light_scenario(), falling_scenario(),
            tnt_scenario(), projectile_scenario(), vehicle_scenario(), torch_button_scenario(),
            plate_target_scenario(), door_scenario(), honey_scenario(), push_class_scenario(), daylight_scenario(), note_scenario(), rail_scenario(), minecart_rail_scenario(), minecart_meeting_scenario(), detector_rail_scenario(), coral_scenario(),
            dispenser_scenario(), tnt_duper_scenario(), flying_engine_scenario(), item_sorter_scenario(), lava_scenario(), waterlogged_scenario(), fire_scenario(), snow_scenario()};
}

} // namespace circuit::test
