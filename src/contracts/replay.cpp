// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/replay.hpp>
#include <circuit/contracts/version.hpp>
#include <algorithm>
#include <bit>

namespace circuit::contracts {
namespace {
constexpr std::uint8_t magic[4]{'C', 'R', 'P', 'L'};
// 24: closed windows that name their block (tag 35);
// 23: window buttons of creative players (tag 34);
// 22: names typed into anvils (tag 32) and experience changes (tag 33);
// 21: summoned experience orbs carry their value, count and age (tag 31);
// 20: slots enabled and disabled (tag 30);
// 19: window buttons (tag 29);
// 18: closed windows whose items drop (tag 28);
// 17: the world's difficulty (tag 27);
// 16: players use items along their view (tag 26);
// 15: player uses carry the player's yaw (tag 25; tags 7 and 24 read it from the horizontal facing);
// 14: player uses carry where they clicked (tag 24; tag 7 reads as the middle of the block);
// 13: summoned entities (tag 23);
// 12: spawned items may carry a pickup delay and an age (tag 22);
// 11: positions and velocities are doubles (before: 1/4096 block units);
// 10: weather (tag 21); 9: day time (tag 20); 8: game rules (tag 19); 7: typed tickets (tag 16), player presences and levels started; 6: each
// command names its level; 5: stacks carry their data components; 4: the
// block registry id; 3: stacks carry damage, player commands carry their
// player. Commands of versions before 6 are the overworld's. Tickets of
// versions before 7 (tags 5 and 9) read as forced squares.
constexpr std::uint16_t format_version = 24;
constexpr std::uint16_t oldest_version = 3;
// The registry of version 3 records, which store only states that predate it.
constexpr std::string_view version_3_registry = "circuit-blocks-1";

class Writer {
public:
    template <class T>
    void put(T value) {
        using U = std::make_unsigned_t<T>;
        auto bits = static_cast<U>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            out.push_back(static_cast<std::byte>(bits & 0xFF));
            if constexpr (sizeof(T) > 1) bits = static_cast<U>(bits >> 8);
        }
    }
    void pos(BlockPos p) { put(p.x); put(p.y); put(p.z); }
    void vec(Vec3 v) {
        put(std::bit_cast<std::uint64_t>(v.x));
        put(std::bit_cast<std::uint64_t>(v.y));
        put(std::bit_cast<std::uint64_t>(v.z));
    }
    void stack(const ItemStack& s) {
        put(s.item);
        put(s.count);
        put(s.damage);
        write_patch(out, s.components);
    }
    void player(PlayerKey k) { put(k.hi); put(k.lo); }
    std::vector<std::byte> out;
};

class Reader {
public:
    // `components`: whether stacks carry data components (format 5 and later);
    // `levels`: whether commands name their level (format 6 and later).
    explicit Reader(std::span<const std::byte> in, bool components = true, bool levels = true)
        : in_(in), components_(components), levels_(levels) {}
    template <class T>
    bool get(T& value) {
        if (in_.size() - at_ < sizeof(T)) return false;
        using U = std::make_unsigned_t<T>;
        U bits{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bits = static_cast<U>(bits | (static_cast<U>(std::to_integer<std::uint8_t>(in_[at_ + i])) << (8 * i)));
        at_ += sizeof(T);
        value = static_cast<T>(bits);
        return true;
    }
    bool pos(BlockPos& p) { return get(p.x) && get(p.y) && get(p.z); }
    // Doubles (format 11 and later), or 1/4096 block units, converted exactly.
    bool vec(Vec3& v) {
        if (doubles_) {
            std::uint64_t x{}, y{}, z{};
            if (!get(x) || !get(y) || !get(z)) return false;
            v = {std::bit_cast<double>(x), std::bit_cast<double>(y), std::bit_cast<double>(z)};
            return finite(v);
        }
        std::int64_t x{}, y{}, z{};
        if (!get(x) || !get(y) || !get(z)) return false;
        v = {static_cast<double>(x) / 4096.0, static_cast<double>(y) / 4096.0, static_cast<double>(z) / 4096.0};
        return true;
    }
    void doubles(bool on) noexcept { doubles_ = on; }
    bool stack(ItemStack& s) {
        if (!get(s.item) || !get(s.count) || !get(s.damage)) return false;
        if (!components_) return true;
        auto rest = in_.subspan(at_);
        const auto size = rest.size();
        auto patch = read_patch(rest);
        if (!patch) return false;
        at_ += size - rest.size();
        s.components = std::move(*patch);
        return true;
    }
    void components(bool on) { components_ = on; }
    void levels(bool on) { levels_ = on; }
    [[nodiscard]] bool levels() const { return levels_; }
    bool player(PlayerKey& k) { return get(k.hi) && get(k.lo); }
    bool bytes(std::size_t n, std::string& s) {
        if (in_.size() - at_ < n) return false;
        s.assign(reinterpret_cast<const char*>(in_.data() + at_), n);
        at_ += n;
        return true;
    }
    [[nodiscard]] bool finished() const { return at_ == in_.size(); }

private:
    std::span<const std::byte> in_;
    std::size_t at_{};
    bool components_{};
    bool doubles_{true};
    bool levels_{};
};

// Record tags: 0 SetBlock, 1 UseBlock, 2 InsertItem, 3 SpawnItem,
// 4 RemoveEntity, 5 radius ticket without a load radius (read only),
// 6 RemoveTicket, 7 PlayerUse, 8 PlayerBreak, 9 radius ticket (read only),
// 10 SetPlayerSlot, 11 PickUp, 12 DropItem, 13 ContainerClick,
// 14 CloseContainer, 15 PlayerDeath, 16 AddTicket, 17 PlayerPresence,
// 18 LevelsStarted, 19 SetGameRule, 20 SetDayTime, 21 SetWeather,
// 22 SpawnItem with a pickup delay or an age (tag 3 has neither),
// 23 SummonEntity, 24 PlayerUse with where it clicked (tag 7 has none),
// 25 PlayerUse with where it clicked and the player's yaw, 26 PlayerUseItem,
// 27 SetDifficulty, 28 CloseContainer whose items drop (tag 14 returns them),
// 29 ContainerButton, 30 SetSlotEnabled, 31 SummonEntity with an experience
// orb's value, count and age (tag 23 has none), 32 RenameItem,
// 33 ChangeExperience, 34 ContainerButton of a creative player (tag 29's
// players are not), 35 CloseContainer naming its window's block (tags 14 and
// 28 name none), 36 PlayerPresence with the hotbar slot in the main hand
// (tag 17's is the first), 37 PlaceRecipe, 38 KnowRecipes, 39 EditBook,
// 40 SwapHands, 41 SetBeacon, 42 ConsumeItem, 43 WearArmor, 44 WearHeld,
// 45 LoseExperience.
std::uint8_t tag_of(const CommandBody& body) {
    if (std::holds_alternative<LoseExperience>(body)) return 45;
    if (std::holds_alternative<WearHeld>(body)) return 44;
    if (std::holds_alternative<WearArmor>(body)) return 43;
    if (std::holds_alternative<ConsumeItem>(body)) return 42;
    if (std::holds_alternative<SetBeacon>(body)) return 41;
    if (std::holds_alternative<SwapHands>(body)) return 40;
    if (std::holds_alternative<EditBook>(body)) return 39;
    if (std::holds_alternative<KnowRecipes>(body)) return 38;
    if (std::holds_alternative<PlaceRecipe>(body)) return 37;
    if (const auto* summon = std::get_if<SummonEntity>(&body); summon && (summon->value != 0 || summon->count != 1 || summon->age != 0))
        return 31;
    if (std::holds_alternative<ChangeExperience>(body)) return 33;
    if (std::holds_alternative<RenameItem>(body)) return 32;
    if (std::holds_alternative<SetSlotEnabled>(body)) return 30;
    if (const auto* button = std::get_if<ContainerButton>(&body)) return button->creative ? 34 : 29;
    if (const auto* close = std::get_if<CloseContainer>(&body); close && close->container) return 35;
    if (const auto* close = std::get_if<CloseContainer>(&body); close && close->drop) return 28;
    if (std::holds_alternative<SummonEntity>(body)) return 23;
    if (std::holds_alternative<PlayerUse>(body)) return 25;
    if (std::holds_alternative<PlayerUseItem>(body)) return 26;
    if (std::holds_alternative<SetDifficulty>(body)) return 27;
    if (const auto* spawn = std::get_if<SpawnItem>(&body); spawn && (spawn->pickup_delay != 0 || spawn->age != 0)) return 22;
    if (std::holds_alternative<SetGameRule>(body)) return 19;
    if (std::holds_alternative<SetDayTime>(body)) return 20;
    if (std::holds_alternative<SetWeather>(body)) return 21;
    if (std::holds_alternative<AddTicket>(body)) return 16;
    if (const auto* presence = std::get_if<PlayerPresence>(&body)) return presence->held != 0 ? 36 : 17;
    if (std::holds_alternative<LevelsStarted>(body)) return 18;
    if (std::holds_alternative<SetPlayerSlot>(body)) return 10;
    if (std::holds_alternative<PickUp>(body)) return 11;
    if (std::holds_alternative<DropItem>(body)) return 12;
    if (std::holds_alternative<ContainerClick>(body)) return 13;
    if (std::holds_alternative<CloseContainer>(body)) return 14;
    if (std::holds_alternative<PlayerDeath>(body)) return 15;
    return static_cast<std::uint8_t>(body.index());
}

void encode_command(Writer& w, const Command& c) {
    w.put(c.admission);
    w.put(tag_of(c.body));
    w.put(static_cast<std::uint8_t>(c.dimension));
    std::visit([&](const auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, SetBlock>) {
            w.pos(body.pos);
            w.put(body.state);
            w.put(static_cast<std::uint8_t>(body.expected_generation.has_value()));
            if (body.expected_generation) w.put(body.expected_generation->value);
        } else if constexpr (std::is_same_v<T, UseBlock>) {
            w.pos(body.pos);
        } else if constexpr (std::is_same_v<T, InsertItem>) {
            w.pos(body.pos);
            w.put(body.slot);
            w.stack(body.stack);
        } else if constexpr (std::is_same_v<T, SpawnItem>) {
            w.vec(body.position);
            w.vec(body.velocity);
            w.stack(body.stack);
            if (body.pickup_delay != 0 || body.age != 0) {
                w.put(body.pickup_delay);
                w.put(body.age);
            }
        } else if constexpr (std::is_same_v<T, RemoveEntity>) {
            w.put(body.id.value);
        } else if constexpr (std::is_same_v<T, SetGameRule>) {
            w.put(static_cast<std::uint8_t>(body.name.size()));
            for (const char ch : body.name) w.put(static_cast<std::uint8_t>(ch));
            w.put(body.value);
        } else if constexpr (std::is_same_v<T, SummonEntity>) {
            w.put(static_cast<std::uint8_t>(body.type.size()));
            for (const char ch : body.type) w.put(static_cast<std::uint8_t>(ch));
            w.vec(body.position);
            w.vec(body.velocity);
            w.put(body.fuse);
            w.put(body.fire);
            if (tag_of(CommandBody{body}) == 31) {
                w.put(body.value);
                w.put(body.count);
                w.put(body.age);
            }
        } else if constexpr (std::is_same_v<T, SetDayTime>) {
            w.put(body.time);
        } else if constexpr (std::is_same_v<T, SetDifficulty>) {
            w.put(body.difficulty);
        } else if constexpr (std::is_same_v<T, SetWeather>) {
            w.put(static_cast<std::uint8_t>(body.kind));
            w.put(body.duration);
        } else if constexpr (std::is_same_v<T, AddTicket>) {
            w.put(body.ticket);
            w.put(body.center.x);
            w.put(body.center.z);
            w.put(body.radius);
            w.put(static_cast<std::uint8_t>(body.type));
            w.put(body.level);
            w.put(body.lifetime);
        } else if constexpr (std::is_same_v<T, RemoveTicket>) {
            w.put(body.ticket);
        } else if constexpr (std::is_same_v<T, PlayerUse>) {
            w.player(body.player);
            w.put(body.slot);
            w.pos(body.pos);
            w.put(static_cast<std::uint8_t>(body.face));
            w.put(static_cast<std::uint8_t>(body.horizontal));
            w.put(static_cast<std::uint8_t>(body.look));
            w.put(static_cast<std::uint8_t>(body.sneaking));
            w.put(static_cast<std::uint8_t>(body.survival));
            w.put(body.reach);
            w.put(body.free);
            w.put(std::bit_cast<std::uint32_t>(body.cursor_x));
            w.put(std::bit_cast<std::uint32_t>(body.cursor_y));
            w.put(std::bit_cast<std::uint32_t>(body.cursor_z));
            w.put(std::bit_cast<std::uint32_t>(body.yaw));
        } else if constexpr (std::is_same_v<T, PlayerUseItem>) {
            w.player(body.player);
            w.put(body.slot);
            w.vec(body.eye);
            w.put(std::bit_cast<std::uint32_t>(body.yaw));
            w.put(std::bit_cast<std::uint32_t>(body.pitch));
            w.put(std::bit_cast<std::uint64_t>(body.reach));
            w.put(static_cast<std::uint8_t>(body.survival));
            w.put(static_cast<std::uint8_t>(body.hit.has_value()));
            if (body.hit) {
                w.pos(body.hit->pos);
                w.put(static_cast<std::uint8_t>(body.hit->face));
                w.put(std::bit_cast<std::uint32_t>(body.hit->cursor_x));
                w.put(std::bit_cast<std::uint32_t>(body.hit->cursor_y));
                w.put(std::bit_cast<std::uint32_t>(body.hit->cursor_z));
            }
        } else if constexpr (std::is_same_v<T, PlayerBreak>) {
            w.player(body.player);
            w.pos(body.pos);
            w.put(static_cast<std::uint8_t>(body.survival));
            w.put(body.slot);
        } else if constexpr (std::is_same_v<T, SetPlayerSlot>) {
            w.player(body.player);
            w.put(body.slot);
            w.stack(body.stack);
        } else if constexpr (std::is_same_v<T, PickUp>) {
            w.player(body.player);
            w.put(body.entity.value);
            w.put(body.selected);
            w.put(static_cast<std::uint8_t>(body.creative));
        } else if constexpr (std::is_same_v<T, DropItem>) {
            w.player(body.player);
            w.put(body.slot);
            w.put(static_cast<std::uint8_t>(body.whole_stack));
            w.vec(body.position);
            w.vec(body.velocity);
        } else if constexpr (std::is_same_v<T, ContainerClick>) {
            w.player(body.player);
            w.put(static_cast<std::uint8_t>(body.container.has_value()));
            if (body.container) w.pos(*body.container);
            w.put(body.slot);
            w.put(body.button);
            w.put(body.mode);
            w.put(static_cast<std::uint8_t>(body.creative));
            w.vec(body.drop_position);
            w.vec(body.drop_velocity);
        } else if constexpr (std::is_same_v<T, CloseContainer>) {
            w.player(body.player);
            w.vec(body.drop_position);
            w.vec(body.drop_velocity);
            if (body.container) {
                w.put(static_cast<std::uint8_t>(body.drop));
                w.pos(*body.container);
            }
        } else if constexpr (std::is_same_v<T, ContainerButton>) {
            w.player(body.player);
            w.put(static_cast<std::uint8_t>(body.container.has_value()));
            if (body.container) w.pos(*body.container);
            w.put(body.button);
        } else if constexpr (std::is_same_v<T, SetSlotEnabled>) {
            w.player(body.player);
            w.put(static_cast<std::uint8_t>(body.container.has_value()));
            if (body.container) w.pos(*body.container);
            w.put(body.slot);
            w.put(static_cast<std::uint8_t>(body.enabled));
        } else if constexpr (std::is_same_v<T, RenameItem>) {
            w.player(body.player);
            w.put(static_cast<std::uint16_t>(body.name.size()));
            for (const char ch : body.name) w.put(static_cast<std::uint8_t>(ch));
            w.put(static_cast<std::uint8_t>(body.creative));
        } else if constexpr (std::is_same_v<T, SwapHands>) {
            w.player(body.player);
            w.put(body.slot);
        } else if constexpr (std::is_same_v<T, ConsumeItem>) {
            w.player(body.player);
            w.put(body.slot);
            w.stack(body.stack);
            w.put(static_cast<std::uint8_t>(body.creative));
            w.vec(body.eye);
        } else if constexpr (std::is_same_v<T, WearArmor>) {
            w.player(body.player);
            w.put(body.amount);
            w.put(static_cast<std::uint8_t>(body.fire));
        } else if constexpr (std::is_same_v<T, WearHeld>) {
            w.player(body.player);
            w.put(body.slot);
            w.put(body.item);
            w.put(body.amount);
        } else if constexpr (std::is_same_v<T, LoseExperience>) {
            w.player(body.player);
            w.vec(body.at);
            w.put(static_cast<std::uint8_t>(body.drops));
        } else if constexpr (std::is_same_v<T, SetBeacon>) {
            w.player(body.player);
            w.put(static_cast<std::uint8_t>(body.container.has_value()));
            if (body.container) w.pos(*body.container);
            // Each power as an effect id plus one, 0 for none.
            w.put(body.primary ? *body.primary + 1 : 0);
            w.put(body.secondary ? *body.secondary + 1 : 0);
        } else if constexpr (std::is_same_v<T, EditBook>) {
            const auto text = [&](const std::string& s) {
                w.put(static_cast<std::uint16_t>(s.size()));
                for (const char ch : s) w.put(static_cast<std::uint8_t>(ch));
            };
            w.player(body.player);
            w.put(body.slot);
            w.put(static_cast<std::uint8_t>(body.pages.size()));
            for (const auto& p : body.pages) text(p);
            w.put(static_cast<std::uint8_t>(body.title.has_value()));
            if (body.title) text(*body.title);
            text(body.author);
        } else if constexpr (std::is_same_v<T, KnowRecipes>) {
            w.player(body.player);
            w.put(static_cast<std::uint32_t>(body.recipes.size()));
            for (const auto r : body.recipes) w.put(r);
            w.put(static_cast<std::uint8_t>(body.known | body.replace << 1));
        } else if constexpr (std::is_same_v<T, PlaceRecipe>) {
            w.player(body.player);
            w.put(static_cast<std::uint8_t>(body.container.has_value()));
            if (body.container) w.pos(*body.container);
            w.put(body.kind);
            w.put(body.recipe);
            w.put(static_cast<std::uint8_t>(body.all));
            w.put(body.selected);
        } else if constexpr (std::is_same_v<T, ChangeExperience>) {
            w.player(body.player);
            w.put(body.amount);
            w.put(static_cast<std::uint8_t>(body.levels));
            w.put(static_cast<std::uint8_t>(body.set));
        } else if constexpr (std::is_same_v<T, PlayerDeath>) {
            w.player(body.player);
            w.vec(body.position);
        } else if constexpr (std::is_same_v<T, PlayerPresence>) {
            w.player(body.player);
            w.vec(body.position);
            w.put(static_cast<std::uint8_t>(body.spectator));
            w.put(static_cast<std::uint8_t>(body.present));
            if (body.held != 0) w.put(body.held);
        }
    }, c.body);
}

bool decode_command(Reader& r, Command& c) {
    std::uint8_t tag{};
    if (!r.get(c.admission) || !r.get(tag)) return false;
    c.dimension = DimensionId::overworld;
    if (r.levels()) {
        std::uint8_t level{};
        if (!r.get(level) || !valid_dimension(level)) return false;
        c.dimension = static_cast<DimensionId>(level);
    }
    switch (tag) {
    case 0: {
        SetBlock b;
        std::uint8_t has{};
        if (!r.pos(b.pos) || !r.get(b.state) || !r.get(has) || has > 1) return false;
        if (has) {
            Generation g;
            if (!r.get(g.value)) return false;
            b.expected_generation = g;
        }
        c.body = b;
        return true;
    }
    case 1: { UseBlock b; if (!r.pos(b.pos)) return false; c.body = b; return true; }
    case 2: {
        InsertItem b;
        if (!r.pos(b.pos) || !r.get(b.slot) || !r.stack(b.stack)) return false;
        c.body = b;
        return true;
    }
    case 3: {
        SpawnItem b;
        if (!r.vec(b.position) || !r.vec(b.velocity) || !r.stack(b.stack)) return false;
        c.body = b;
        return true;
    }
    case 4: { RemoveEntity b; if (!r.get(b.id.value)) return false; c.body = b; return true; }
    case 5: {
        AddTicket b; // a forced square
        if (!r.get(b.ticket) || !r.get(b.center.x) || !r.get(b.center.z) || !r.get(b.radius)) return false;
        c.body = b;
        return true;
    }
    case 6: { RemoveTicket b; if (!r.get(b.ticket)) return false; c.body = b; return true; }
    case 7: case 24: case 25: {
        PlayerUse b;
        std::uint8_t face{}, horizontal{}, look{}, sneaking{}, survival{};
        if (!r.player(b.player) || !r.get(b.slot) || !r.pos(b.pos) || !r.get(face) || !r.get(horizontal) || !r.get(look)
            || !r.get(sneaking) || !r.get(survival) || !r.get(b.reach) || !r.get(b.free))
            return false;
        if (face > 5 || horizontal > 5 || look > 5 || sneaking > 1 || survival > 1 || b.reach > 3 || b.free > 3) return false;
        if (tag >= 24) {
            std::uint32_t x{}, y{}, z{};
            if (!r.get(x) || !r.get(y) || !r.get(z)) return false;
            b.cursor_x = std::bit_cast<float>(x);
            b.cursor_y = std::bit_cast<float>(y);
            b.cursor_z = std::bit_cast<float>(z);
            for (const float v : {b.cursor_x, b.cursor_y, b.cursor_z})
                if (!(v >= 0.0f && v <= 1.0f)) return false;
        }
        if (tag == 25) {
            std::uint32_t yaw{};
            if (!r.get(yaw)) return false;
            b.yaw = std::bit_cast<float>(yaw);
            if (!(b.yaw >= -360.0f && b.yaw <= 360.0f)) return false;
        }
        b.face = static_cast<Direction>(face);
        b.horizontal = static_cast<Direction>(horizontal);
        b.look = static_cast<Direction>(look);
        if (!is_horizontal(b.horizontal)) return false;
        // Earlier uses have the yaw of their horizontal facing.
        if (tag != 25)
            b.yaw = b.horizontal == Direction::south ? 0.0f : b.horizontal == Direction::west ? 90.0f : b.horizontal == Direction::north ? 180.0f : 270.0f;
        b.sneaking = sneaking == 1;
        b.survival = survival == 1;
        c.body = b;
        return true;
    }
    case 26: {
        PlayerUseItem b;
        std::uint32_t yaw{}, pitch{};
        std::uint64_t reach{};
        std::uint8_t survival{}, hit{};
        if (!r.player(b.player) || !r.get(b.slot) || !r.vec(b.eye) || !r.get(yaw) || !r.get(pitch) || !r.get(reach) || !r.get(survival)
            || !r.get(hit))
            return false;
        b.yaw = std::bit_cast<float>(yaw);
        b.pitch = std::bit_cast<float>(pitch);
        b.reach = std::bit_cast<double>(reach);
        if (!(b.yaw >= -360.0f && b.yaw <= 360.0f) || !(b.pitch >= -90.0f && b.pitch <= 90.0f) || !(b.reach >= 0.0 && b.reach <= 64.0)
            || survival > 1 || hit > 1)
            return false;
        b.survival = survival == 1;
        if (hit == 1) {
            BlockHit h;
            std::uint8_t face{};
            std::uint32_t x{}, y{}, z{};
            if (!r.pos(h.pos) || !r.get(face) || face > 5 || !r.get(x) || !r.get(y) || !r.get(z)) return false;
            h.face = static_cast<Direction>(face);
            h.cursor_x = std::bit_cast<float>(x);
            h.cursor_y = std::bit_cast<float>(y);
            h.cursor_z = std::bit_cast<float>(z);
            for (const float v : {h.cursor_x, h.cursor_y, h.cursor_z})
                if (!(v >= 0.0f && v <= 1.0f)) return false;
            b.hit = h;
        }
        c.body = b;
        return true;
    }
    case 8: {
        PlayerBreak b;
        std::uint8_t survival{};
        if (!r.player(b.player) || !r.pos(b.pos) || !r.get(survival) || survival > 1 || !r.get(b.slot)) return false;
        b.survival = survival == 1;
        c.body = b;
        return true;
    }
    case 9: {
        AddTicket b; // a forced square; the load radius is not kept
        std::uint8_t load_radius{};
        if (!r.get(b.ticket) || !r.get(b.center.x) || !r.get(b.center.z) || !r.get(b.radius) || !r.get(load_radius)) return false;
        c.body = b;
        return true;
    }
    case 10: {
        SetPlayerSlot b;
        if (!r.player(b.player) || !r.get(b.slot) || !r.stack(b.stack)) return false;
        c.body = b;
        return true;
    }
    case 11: {
        PickUp b;
        std::uint8_t creative{};
        if (!r.player(b.player) || !r.get(b.entity.value) || !r.get(b.selected) || !r.get(creative) || creative > 1) return false;
        b.creative = creative == 1;
        c.body = b;
        return true;
    }
    case 12: {
        DropItem b;
        std::uint8_t whole{};
        if (!r.player(b.player) || !r.get(b.slot) || !r.get(whole) || whole > 1 || !r.vec(b.position) || !r.vec(b.velocity)) return false;
        b.whole_stack = whole == 1;
        c.body = b;
        return true;
    }
    case 13: {
        ContainerClick b;
        std::uint8_t has{}, creative{};
        if (!r.player(b.player) || !r.get(has) || has > 1) return false;
        if (has) {
            BlockPos p;
            if (!r.pos(p)) return false;
            b.container = p;
        }
        if (!r.get(b.slot) || !r.get(b.button) || !r.get(b.mode) || b.mode > 6 || !r.get(creative) || creative > 1
            || !r.vec(b.drop_position) || !r.vec(b.drop_velocity))
            return false;
        b.creative = creative == 1;
        c.body = b;
        return true;
    }
    case 14:
    case 28:
    case 35: {
        CloseContainer b;
        if (!r.player(b.player) || !r.vec(b.drop_position) || !r.vec(b.drop_velocity)) return false;
        b.drop = tag == 28;
        if (tag == 35) {
            std::uint8_t drop{};
            BlockPos p;
            if (!r.get(drop) || drop > 1 || !r.pos(p)) return false;
            b.drop = drop == 1;
            b.container = p;
        }
        c.body = b;
        return true;
    }
    case 15: {
        PlayerDeath b;
        if (!r.player(b.player) || !r.vec(b.position)) return false;
        c.body = b;
        return true;
    }
    case 16: {
        AddTicket b;
        std::uint8_t type{};
        if (!r.get(b.ticket) || !r.get(b.center.x) || !r.get(b.center.z) || !r.get(b.radius) || !r.get(type) || !r.get(b.level)
            || !r.get(b.lifetime) || type >= ticket_type_count)
            return false;
        b.type = static_cast<TicketType>(type);
        c.body = b;
        return true;
    }
    case 17:
    case 36: {
        PlayerPresence b;
        std::uint8_t spectator{}, present{};
        if (!r.player(b.player) || !r.vec(b.position) || !r.get(spectator) || !r.get(present) || spectator > 1 || present > 1) return false;
        if (tag == 36 && (!r.get(b.held) || b.held == 0 || b.held > 8)) return false;
        b.spectator = spectator == 1;
        b.present = present == 1;
        c.body = b;
        return true;
    }
    case 18: c.body = LevelsStarted{}; return true;
    case 19: {
        SetGameRule b;
        std::uint8_t length{};
        if (!r.get(length) || length == 0 || length > max_game_rule_name || !r.bytes(length, b.name) || !r.get(b.value)) return false;
        if (!std::ranges::all_of(b.name, [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); })) return false;
        c.body = b;
        return true;
    }
    case 20: {
        SetDayTime b;
        if (!r.get(b.time) || b.time < 0) return false;
        c.body = b;
        return true;
    }
    case 27: {
        SetDifficulty b;
        if (!r.get(b.difficulty) || b.difficulty > max_difficulty) return false;
        c.body = b;
        return true;
    }
    case 29:
    case 34: {
        ContainerButton b;
        b.creative = tag == 34;
        std::uint8_t has{};
        if (!r.player(b.player) || !r.get(has) || has > 1) return false;
        if (has) {
            BlockPos p;
            if (!r.pos(p)) return false;
            b.container = p;
        }
        if (!r.get(b.button)) return false;
        c.body = b;
        return true;
    }
    case 32: {
        RenameItem b;
        std::uint16_t length{};
        std::uint8_t creative{};
        if (!r.player(b.player) || !r.get(length) || length > 4 * max_item_name || !r.bytes(length, b.name) || !r.get(creative) || creative > 1)
            return false;
        b.creative = creative == 1;
        c.body = b;
        return true;
    }
    case 33: {
        ChangeExperience b;
        std::uint8_t levels{}, set{};
        if (!r.player(b.player) || !r.get(b.amount) || !r.get(levels) || levels > 1 || !r.get(set) || set > 1) return false;
        b.levels = levels == 1;
        b.set = set == 1;
        c.body = b;
        return true;
    }
    case 41: {
        SetBeacon b;
        std::uint8_t has{};
        std::int32_t primary{}, secondary{};
        if (!r.player(b.player) || !r.get(has) || has > 1) return false;
        if (has) {
            BlockPos p;
            if (!r.pos(p)) return false;
            b.container = p;
        }
        if (!r.get(primary) || !r.get(secondary) || primary < 0 || secondary < 0 || primary > max_beacon_effect + 1
            || secondary > max_beacon_effect + 1)
            return false;
        if (primary > 0) b.primary = primary - 1;
        if (secondary > 0) b.secondary = secondary - 1;
        c.body = b;
        return true;
    }
    case 45: {
        LoseExperience b;
        std::uint8_t drops{};
        if (!r.player(b.player) || !r.vec(b.at) || !r.get(drops) || drops > 1) return false;
        b.drops = drops == 1;
        c.body = b;
        return true;
    }
    case 44: {
        WearHeld b;
        if (!r.player(b.player) || !r.get(b.slot) || ((b.slot < hotbar_first || b.slot >= hotbar_first + 9) && b.slot != offhand_slot) || !r.get(b.item)
            || !r.get(b.amount)
            || b.amount < 1 || b.amount > max_held_wear)
            return false;
        c.body = b;
        return true;
    }
    case 43: {
        WearArmor b;
        std::uint8_t fire{};
        if (!r.player(b.player) || !r.get(b.amount) || b.amount < 1 || b.amount > max_armor_wear || !r.get(fire) || fire > 1) return false;
        b.fire = fire == 1;
        c.body = b;
        return true;
    }
    case 42: {
        ConsumeItem b;
        std::uint8_t creative{};
        if (!r.player(b.player) || !r.get(b.slot) || !(b.slot == offhand_slot || (b.slot >= hotbar_first && b.slot < hotbar_first + 9)))
            return false;
        if (!r.stack(b.stack) || !r.get(creative) || creative > 1 || !r.vec(b.eye)) return false;
        b.creative = creative == 1;
        c.body = b;
        return true;
    }
    case 40: {
        SwapHands b;
        if (!r.player(b.player) || !r.get(b.slot) || b.slot < hotbar_first || b.slot >= hotbar_first + 9) return false;
        c.body = b;
        return true;
    }
    case 39: {
        EditBook b;
        const auto text = [&](std::string& s, std::size_t max) {
            std::uint16_t n{};
            return r.get(n) && n <= max && r.bytes(n, s);
        };
        std::uint8_t pages{}, titled{};
        if (!r.player(b.player) || !r.get(b.slot) || !(b.slot == offhand_slot || (b.slot >= hotbar_first && b.slot < hotbar_first + 9))) return false;
        if (!r.get(pages) || pages > max_book_pages) return false;
        b.pages.resize(pages);
        for (auto& p : b.pages)
            if (!text(p, 3 * max_page_units)) return false;
        if (!r.get(titled) || titled > 1) return false;
        if (titled) {
            std::string title;
            if (!text(title, 3 * max_title_units)) return false;
            b.title = std::move(title);
        }
        if (!text(b.author, max_author_bytes)) return false;
        c.body = std::move(b);
        return true;
    }
    case 38: {
        KnowRecipes b;
        std::uint32_t n{};
        std::uint8_t flags{};
        if (!r.player(b.player) || !r.get(n) || n > max_known_recipes) return false;
        b.recipes.resize(n);
        for (auto& recipe : b.recipes)
            if (!r.get(recipe)) return false;
        if (!r.get(flags) || flags > 3) return false;
        b.known = (flags & 1) != 0;
        b.replace = (flags & 2) != 0;
        c.body = std::move(b);
        return true;
    }
    case 37: {
        PlaceRecipe b;
        std::uint8_t has{}, all{};
        if (!r.player(b.player) || !r.get(has) || has > 1) return false;
        if (has) {
            BlockPos p;
            if (!r.pos(p)) return false;
            b.container = p;
        }
        if (!r.get(b.kind) || b.kind > 3 || !r.get(b.recipe) || !r.get(all) || all > 1 || !r.get(b.selected)) return false;
        if (b.selected < hotbar_first || b.selected >= hotbar_first + 9) return false;
        b.all = all == 1;
        c.body = b;
        return true;
    }
    case 30: {
        SetSlotEnabled b;
        std::uint8_t has{}, enabled{};
        if (!r.player(b.player) || !r.get(has) || has > 1) return false;
        if (has) {
            BlockPos p;
            if (!r.pos(p)) return false;
            b.container = p;
        }
        if (!r.get(b.slot) || !r.get(enabled) || enabled > 1) return false;
        b.enabled = enabled == 1;
        c.body = b;
        return true;
    }
    case 22: {
        SpawnItem b;
        if (!r.vec(b.position) || !r.vec(b.velocity) || !r.stack(b.stack) || !r.get(b.pickup_delay) || !r.get(b.age)) return false;
        if (b.pickup_delay == 0 && b.age == 0) return false; // written as tag 3
        c.body = b;
        return true;
    }
    case 23: case 31: {
        SummonEntity b;
        std::uint8_t length{};
        if (!r.get(length) || length == 0 || length > max_entity_type || !r.bytes(length, b.type) || !r.vec(b.position)
            || !r.vec(b.velocity) || !r.get(b.fuse) || !r.get(b.fire))
            return false;
        if (tag == 31 && (!r.get(b.value) || !r.get(b.count) || !r.get(b.age) || (b.value == 0 && b.count == 1 && b.age == 0))) return false;
        if (!valid_entity_type(b.type)) return false;
        c.body = b;
        return true;
    }
    case 21: {
        SetWeather b;
        std::uint8_t kind{};
        if (!r.get(kind) || kind >= weather_kind_count || !r.get(b.duration) || b.duration < 0) return false;
        b.kind = static_cast<WeatherKind>(kind);
        c.body = b;
        return true;
    }
    default: return false;
    }
}

Result<void> check_structure(const ReplayLog& log, const ReplayLimits& limits) {
    if (log.bundle_id.empty() || log.bundle_id.size() > limits.max_bundle_id) return std::unexpected(ErrorCode::malformed_record);
    if (log.rules_id.empty() || log.rules_id.size() > limits.max_bundle_id) return std::unexpected(ErrorCode::malformed_record);
    if (log.block_registry_id.empty() || log.block_registry_id.size() > limits.max_bundle_id)
        return std::unexpected(ErrorCode::malformed_record);
    if (log.ticks.size() > limits.max_ticks) return std::unexpected(ErrorCode::record_too_large);
    std::optional<Tick> last_tick;
    std::optional<std::uint64_t> last_admission;
    for (const auto& t : log.ticks) {
        if (last_tick && t.tick <= *last_tick) return std::unexpected(ErrorCode::malformed_record);
        last_tick = t.tick;
        if (t.commands.size() > limits.max_commands_per_tick) return std::unexpected(ErrorCode::record_too_large);
        for (const auto& c : t.commands) {
            if (last_admission && c.admission <= *last_admission) return std::unexpected(ErrorCode::malformed_record);
            last_admission = c.admission;
        }
    }
    return {};
}
} // namespace

Result<std::vector<std::byte>> encode_replay(const ReplayLog& log, const ReplayLimits& limits) {
    if (auto ok = check_structure(log, limits); !ok) return std::unexpected(ok.error());
    Writer w;
    for (auto b : magic) w.put(b);
    w.put(format_version);
    for (const auto* id : {&log.bundle_id, &log.rules_id, &log.block_registry_id}) {
        w.put(static_cast<std::uint16_t>(id->size()));
        for (char ch : *id) w.put(static_cast<std::uint8_t>(ch));
    }
    w.put(log.seed);
    w.put(static_cast<std::uint32_t>(log.ticks.size()));
    for (const auto& t : log.ticks) {
        w.put(t.tick);
        w.put(static_cast<std::uint32_t>(t.commands.size()));
        for (const auto& c : t.commands) encode_command(w, c);
        if (w.out.size() > limits.max_bytes) return std::unexpected(ErrorCode::record_too_large);
    }
    if (w.out.size() > limits.max_bytes) return std::unexpected(ErrorCode::record_too_large);
    return std::move(w.out);
}

Result<ReplayLog> decode_replay(std::span<const std::byte> bytes, const ReplayLimits& limits) {
    if (bytes.size() > limits.max_bytes) return std::unexpected(ErrorCode::record_too_large);
    Reader r(bytes);
    for (auto expected : magic) {
        std::uint8_t b{};
        if (!r.get(b) || b != expected) return std::unexpected(ErrorCode::malformed_record);
    }
    std::uint16_t version{}, id_length{};
    if (!r.get(version) || version < oldest_version || version > format_version) return std::unexpected(ErrorCode::malformed_record);
    r.components(version >= 5);
    r.levels(version >= 6);
    r.doubles(version >= 11);
    ReplayLog log;
    std::vector<std::string*> ids{&log.bundle_id, &log.rules_id};
    if (version >= 4) ids.push_back(&log.block_registry_id);
    else log.block_registry_id = version_3_registry;
    for (auto* id : ids) {
        if (!r.get(id_length) || id_length == 0 || id_length > limits.max_bundle_id || !r.bytes(id_length, *id))
            return std::unexpected(ErrorCode::malformed_record);
    }
    std::uint32_t tick_count{};
    if (!r.get(log.seed) || !r.get(tick_count)) return std::unexpected(ErrorCode::malformed_record);
    if (tick_count > limits.max_ticks) return std::unexpected(ErrorCode::record_too_large);
    log.ticks.reserve(tick_count);
    for (std::uint32_t i = 0; i < tick_count; ++i) {
        TickInput t;
        std::uint32_t count{};
        if (!r.get(t.tick) || !r.get(count)) return std::unexpected(ErrorCode::malformed_record);
        if (count > limits.max_commands_per_tick) return std::unexpected(ErrorCode::record_too_large);
        t.commands.resize(count);
        for (auto& c : t.commands)
            if (!decode_command(r, c)) return std::unexpected(ErrorCode::malformed_record);
        log.ticks.push_back(std::move(t));
    }
    if (!r.finished()) return std::unexpected(ErrorCode::malformed_record);
    if (auto ok = check_structure(log, limits); !ok) return std::unexpected(ok.error());
    return log;
}

std::vector<std::byte> encode_tick_input(const TickInput& input) {
    Writer w;
    w.put(input.tick);
    w.put(static_cast<std::uint32_t>(input.commands.size()));
    for (const auto& c : input.commands) encode_command(w, c);
    return std::move(w.out);
}

Result<TickInput> decode_tick_input(std::span<const std::byte> bytes, const ReplayLimits& limits, bool components, bool levels,
                                    bool doubles) {
    Reader r(bytes, components, levels);
    r.doubles(doubles);
    TickInput t;
    std::uint32_t count{};
    if (!r.get(t.tick) || !r.get(count)) return std::unexpected(ErrorCode::malformed_record);
    if (count > limits.max_commands_per_tick) return std::unexpected(ErrorCode::record_too_large);
    t.commands.resize(count);
    for (auto& c : t.commands)
        if (!decode_command(r, c)) return std::unexpected(ErrorCode::malformed_record);
    if (!r.finished()) return std::unexpected(ErrorCode::malformed_record);
    return t;
}

Result<void> check_replay_rules(const ReplayLog& log, const VersionBundle& bundle) noexcept {
    if (!bundle.rules) return std::unexpected(ErrorCode::unsupported_version);
    if (log.bundle_id != bundle.id || log.rules_id != bundle.rules->id || log.block_registry_id != bundle.rules->block_registry)
        return std::unexpected(ErrorCode::rules_mismatch);
    return {};
}

} // namespace circuit::contracts
