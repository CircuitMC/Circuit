// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/replay.hpp>
#include <circuit/contracts/version.hpp>
#include <algorithm>

namespace circuit::contracts {
namespace {
constexpr std::uint8_t magic[4]{'C', 'R', 'P', 'L'};
// 10: weather (tag 21); 9: day time (tag 20); 8: game rules (tag 19); 7: typed tickets (tag 16), player presences and levels started; 6: each
// command names its level; 5: stacks carry their data components; 4: the
// block registry id; 3: stacks carry damage, player commands carry their
// player. Commands of versions before 6 are the overworld's. Tickets of
// versions before 7 (tags 5 and 9) read as forced squares.
constexpr std::uint16_t format_version = 10;
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
    void vec(FixedVec v) { put(v.x); put(v.y); put(v.z); }
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
    bool vec(FixedVec& v) { return get(v.x) && get(v.y) && get(v.z); }
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
    bool levels_{};
};

// Record tags: 0 SetBlock, 1 UseBlock, 2 InsertItem, 3 SpawnItem,
// 4 RemoveEntity, 5 radius ticket without a load radius (read only),
// 6 RemoveTicket, 7 PlayerUse, 8 PlayerBreak, 9 radius ticket (read only),
// 10 SetPlayerSlot, 11 PickUp, 12 DropItem, 13 ContainerClick,
// 14 CloseContainer, 15 PlayerDeath, 16 AddTicket, 17 PlayerPresence,
// 18 LevelsStarted, 19 SetGameRule, 20 SetDayTime, 21 SetWeather.
std::uint8_t tag_of(const CommandBody& body) {
    if (std::holds_alternative<SetGameRule>(body)) return 19;
    if (std::holds_alternative<SetDayTime>(body)) return 20;
    if (std::holds_alternative<SetWeather>(body)) return 21;
    if (std::holds_alternative<AddTicket>(body)) return 16;
    if (std::holds_alternative<PlayerPresence>(body)) return 17;
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
        } else if constexpr (std::is_same_v<T, RemoveEntity>) {
            w.put(body.id.value);
        } else if constexpr (std::is_same_v<T, SetGameRule>) {
            w.put(static_cast<std::uint8_t>(body.name.size()));
            for (const char ch : body.name) w.put(static_cast<std::uint8_t>(ch));
            w.put(body.value);
        } else if constexpr (std::is_same_v<T, SetDayTime>) {
            w.put(body.time);
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
        } else if constexpr (std::is_same_v<T, PlayerDeath>) {
            w.player(body.player);
            w.vec(body.position);
        } else if constexpr (std::is_same_v<T, PlayerPresence>) {
            w.player(body.player);
            w.vec(body.position);
            w.put(static_cast<std::uint8_t>(body.spectator));
            w.put(static_cast<std::uint8_t>(body.present));
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
    case 7: {
        PlayerUse b;
        std::uint8_t face{}, horizontal{}, look{}, sneaking{}, survival{};
        if (!r.player(b.player) || !r.get(b.slot) || !r.pos(b.pos) || !r.get(face) || !r.get(horizontal) || !r.get(look)
            || !r.get(sneaking) || !r.get(survival) || !r.get(b.reach) || !r.get(b.free))
            return false;
        if (face > 5 || horizontal > 5 || look > 5 || sneaking > 1 || survival > 1 || b.reach > 3 || b.free > 3) return false;
        b.face = static_cast<Direction>(face);
        b.horizontal = static_cast<Direction>(horizontal);
        b.look = static_cast<Direction>(look);
        if (!is_horizontal(b.horizontal)) return false;
        b.sneaking = sneaking == 1;
        b.survival = survival == 1;
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
    case 14: {
        CloseContainer b;
        if (!r.player(b.player) || !r.vec(b.drop_position) || !r.vec(b.drop_velocity)) return false;
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
    case 17: {
        PlayerPresence b;
        std::uint8_t spectator{}, present{};
        if (!r.player(b.player) || !r.vec(b.position) || !r.get(spectator) || !r.get(present) || spectator > 1 || present > 1) return false;
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

Result<TickInput> decode_tick_input(std::span<const std::byte> bytes, const ReplayLimits& limits, bool components, bool levels) {
    Reader r(bytes, components, levels);
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
