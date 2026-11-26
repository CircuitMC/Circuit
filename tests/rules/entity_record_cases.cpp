// SPDX-License-Identifier: AGPL-3.0-only
// The transitional entity record: a header every entity has, and the fields
// of each kind, which entity_payload_fault checks.
#include "../support/check.hpp"
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <format>
#include <optional>
#include <string_view>

namespace {
using namespace circuit::gameplay;
using circuit::test::require;

Entity of(EntityKind kind) {
    Entity e;
    e.id = circuit::contracts::EntityId{1};
    e.kind = kind;
    switch (kind) {
    case EntityKind::item: e.stack = {items::stone, 3}; break;
    case EntityKind::falling_block: e.carried = blocks::stone(); break;
    case EntityKind::primed_tnt: e.fuse = 80; break;
    case EntityKind::experience_orb: e.experience = 7; break;
    case EntityKind::boat: e.variant = 2; e.status = 1; break;
    case EntityKind::minecart: e.yaw = 90.0f; break;
    case EntityKind::arrow: e.life = 20; e.fire = 100; break;
    default: break;
    }
    return e;
}

void expect(const Entity& e, std::optional<std::string_view> fault, std::string_view label) {
    const auto got = entity_payload_fault(e);
    require(got == fault, std::format("{}: {} instead of {}", label, got.value_or("none"), fault.value_or("none")));
}
} // namespace

CIRCUIT_TEST("the entity header holds what every kind has") {
    Entity e = of(EntityKind::arrow);
    e.position = {1.5, 2.0, 3.5};
    const EntityHeader& header = e;
    require(header.id == e.id && header.kind == EntityKind::arrow && header.position == e.position, "the header is the entity's");
    Entity other = e;
    other.life = 21;
    require(static_cast<const EntityHeader&>(other) == header && !(other == e), "kind fields are outside the header and still compared");
}

CIRCUIT_TEST("each kind's own fields are checked and the others must keep their defaults") {
    for (const auto kind : {EntityKind::item, EntityKind::falling_block, EntityKind::primed_tnt, EntityKind::arrow, EntityKind::snowball,
                            EntityKind::minecart, EntityKind::boat, EntityKind::experience_orb})
        expect(of(kind), std::nullopt, entity_kind_name(kind));
    auto e = of(EntityKind::item);
    e.stack.count = 0;
    expect(e, "entity stack", "an empty item");
    e = of(EntityKind::item);
    e.fuse = 5;
    expect(e, "entity state of another kind", "an item with a fuse");
    e = of(EntityKind::primed_tnt);
    e.fuse = 40000;
    expect(e, "primed TNT fuse", "a fuse beyond a short");
    e = of(EntityKind::falling_block);
    e.carried = blocks::air();
    expect(e, "falling block state", "a falling block of air");
    e = of(EntityKind::boat);
    e.variant = 99;
    expect(e, "boat state", "an unknown boat");
    e = of(EntityKind::minecart);
    e.yaw = 400.0f;
    expect(e, "minecart yaw", "a yaw beyond a turn");
    e = of(EntityKind::experience_orb);
    e.orbs = 0;
    expect(e, "experience orb state", "no orbs");
    e = of(EntityKind::snowball);
    e.stack = {items::stone, 1};
    expect(e, "entity state of another kind", "a snowball with a stack");
}
