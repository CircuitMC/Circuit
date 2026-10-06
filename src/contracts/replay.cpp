// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/replay.hpp>
#include <circuit/contracts/version.hpp>

namespace circuit::contracts {
namespace {
constexpr std::uint8_t magic[4]{'C', 'R', 'P', 'L'};
constexpr std::uint16_t format_version = 2;

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
    void stack(ItemStack s) { put(s.item); put(s.count); }
    std::vector<std::byte> out;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}
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
    bool stack(ItemStack& s) { return get(s.item) && get(s.count); }
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
};

void encode_command(Writer& w, const Command& c) {
    w.put(c.admission);
    w.put(static_cast<std::uint8_t>(c.body.index()));
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
        } else if constexpr (std::is_same_v<T, AddTicket>) {
            w.put(body.ticket);
            w.put(body.center.x);
            w.put(body.center.z);
            w.put(body.radius);
        } else if constexpr (std::is_same_v<T, RemoveTicket>) {
            w.put(body.ticket);
        }
    }, c.body);
}

bool decode_command(Reader& r, Command& c) {
    std::uint8_t tag{};
    if (!r.get(c.admission) || !r.get(tag)) return false;
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
        AddTicket b;
        if (!r.get(b.ticket) || !r.get(b.center.x) || !r.get(b.center.z) || !r.get(b.radius)) return false;
        c.body = b;
        return true;
    }
    case 6: { RemoveTicket b; if (!r.get(b.ticket)) return false; c.body = b; return true; }
    default: return false;
    }
}

Result<void> check_structure(const ReplayLog& log, const ReplayLimits& limits) {
    if (log.bundle_id.empty() || log.bundle_id.size() > limits.max_bundle_id) return std::unexpected(ErrorCode::malformed_record);
    if (log.rules_id.empty() || log.rules_id.size() > limits.max_bundle_id) return std::unexpected(ErrorCode::malformed_record);
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
    for (const auto* id : {&log.bundle_id, &log.rules_id}) {
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
    if (!r.get(version) || version != format_version) return std::unexpected(ErrorCode::malformed_record);
    ReplayLog log;
    for (auto* id : {&log.bundle_id, &log.rules_id}) {
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

Result<TickInput> decode_tick_input(std::span<const std::byte> bytes, const ReplayLimits& limits) {
    Reader r(bytes);
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
    if (log.bundle_id != bundle.id || log.rules_id != bundle.rules->id) return std::unexpected(ErrorCode::rules_mismatch);
    return {};
}

} // namespace circuit::contracts
