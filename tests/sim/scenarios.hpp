// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../support/scenario.hpp"
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

inline void append(std::vector<CommandBody>& to, std::vector<CommandBody> from) {
    for (auto& c : from) to.push_back(std::move(c));
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
                    place({15, 1, 11}, blocks::comparator(D::south)), place({15, 1, 12}, blocks::wire()),
                    place({15, 1, 13}, blocks::wire()), place({15, 1, 14}, blocks::lamp()),
                    place({16, 1, 11}, blocks::comparator(D::south, true)), place({16, 1, 12}, blocks::lamp()),
                    place({17, 1, 11}, blocks::wire()), place({18, 1, 11}, blocks::lever()),
                    place({17, 0, 11}, blocks::comparator(D::south)), place({17, 0, 12}, blocks::lamp())};
        std::vector<CommandBody> c;
        if (t % 13 == 5) c.push_back(use_lever({18, 1, 11}));
        if (t == 50) c.push_back(use_block({16, 1, 11}));
        if (t == 120) c.push_back(place({15, 1, 10}, blocks::air()));
        if (t == 140) c.push_back(place({15, 1, 10}, blocks::chest(D::north, ChestType::left)));
        return c;
    }};
}

inline std::vector<Scenario> all_scenarios() {
    return {slice_scenario(), clock_scenario(), piston_scenario(), hopper_scenario(), water_scenario(),
            wheat_scenario(), many_machines_scenario(), ticket_scenario(), piston_chain_scenario(),
            comparator_chest_scenario()};
}

} // namespace circuit::test
