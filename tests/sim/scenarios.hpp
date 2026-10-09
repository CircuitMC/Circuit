// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../support/scenario.hpp"
#include <array>
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
};

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
        if (t < 60 && t % 7 == 3) c.push_back(SpawnItem{at_block(15.0, 1.0, 31.5), {4000, 0, 0}, {items::redstone, 3}});
        if (t < 80 && t % 9 == 4) c.push_back(SpawnItem{at_block(0.5, 6.0, 40.5), {0, 200, 0}, {items::wheat, 1}});
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
            for (int x = 10; x <= 21; ++x) {
                b.push_back(place({x, 0, 5}, blocks::wheat()));
                b.push_back(place({x, 0, 7}, blocks::lamp()));
                b.push_back(place({x, 0, 6}, blocks::observer(D::north)));
            }
            for (int z = 12; z < 20; ++z)
                for (int x = 12; x < 20; ++x) b.push_back(place({x, 0, z}, blocks::wheat()));
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

inline std::vector<Scenario> all_scenarios() {
    return {slice_scenario(), clock_scenario(), piston_scenario(), hopper_scenario(), water_scenario(),
            wheat_scenario(), many_machines_scenario(), ticket_scenario(), piston_chain_scenario(),
            comparator_chest_scenario(), piston_pulse_scenario(), drops_scenario(), observed_piston_scenario(),
            transient_head_scenario(), registry_block_scenario()};
}

} // namespace circuit::test
