// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/evidence/trace_format.hpp>
#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <set>

namespace circuit::evidence {
using contracts::EvidenceSource;

namespace {
bool id_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'; }
bool key_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

bool valid_id(std::string_view id) {
    const auto colon = id.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == id.size()) return false;
    for (std::size_t i = 0; i < id.size(); ++i)
        if (i != colon && !id_char(id[i])) return false;
    return id.find(':', colon + 1) == std::string_view::npos;
}

std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const auto start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

template <class T>
bool number(std::string_view text, T& out) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

std::optional<EvidenceSource> source_from(std::string_view s) {
    if (s == "harness_trace") return EvidenceSource::harness_trace;
    if (s == "documented_behavior") return EvidenceSource::documented_behavior;
    if (s == "controlled_experiment") return EvidenceSource::controlled_experiment;
    if (s == "circuit_reference") return EvidenceSource::circuit_reference;
    return std::nullopt;
}

std::string pos_text(BlockPos p) { return std::format("{} {} {}", p.x, p.y, p.z); }

class Parser {
public:
    Parser(std::string_view text, const FormatLimits& limits) : text_(text), limits_(limits) {}

    std::expected<Fixture, FormatError> run() {
        if (text_.size() > limits_.max_bytes) return fail(0, "file exceeds the size limit");
        std::size_t start = 0;
        while (start <= text_.size()) {
            auto end = text_.find('\n', start);
            if (end == std::string_view::npos) end = text_.size();
            auto line = text_.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            ++line_;
            if (line_ > limits_.max_lines) return fail(line_, "too many lines");
            if (line.size() > limits_.max_line_length) return fail(line_, "line too long");
            if (auto ok = handle(line); !ok) return std::unexpected(ok.error());
            if (end == text_.size()) break;
            start = end + 1;
        }
        if (!saw_magic_) return fail(1, "missing `circuit-trace 1` header");
        if (!header_done_) {
            if (auto ok = finish_header(line_); !ok) return std::unexpected(ok.error());
        }
        return std::move(f_);
    }

private:
    using Status = std::expected<void, FormatError>;
    static std::unexpected<FormatError> fail(std::size_t line, std::string message) {
        return std::unexpected(FormatError{line, std::move(message)});
    }

    Status handle(std::string_view line) {
        const auto t = tokens(line);
        if (!saw_magic_) {
            if (t.size() != 2 || t[0] != "circuit-trace") return fail(line_, "first line must be `circuit-trace 1`");
            if (t[1] != "1") return fail(line_, "unsupported format version");
            saw_magic_ = true;
            return {};
        }
        if (t.empty() || t[0].front() == '#') return {};
        static const std::set<std::string_view> header_keys{"title", "game-version", "source", "harness", "mechanisms",
                                                            "area", "ticks", "random-tick-speed", "neighbor-update-limit",
                                                            "changes"};
        if (header_keys.contains(t[0])) {
            if (header_done_) return fail(line_, "header key after the body started");
            if (!seen_.insert(std::string(t[0])).second) return fail(line_, std::format("duplicate key `{}`", t[0]));
            return header(t, line);
        }
        if (!header_done_)
            if (auto ok = finish_header(line_); !ok) return ok;
        if (t[0] == "at") return action(t);
        if (t[0] == "change") return change(t);
        if (t[0] == "block") return block(t);
        if (t[0] == "slot") return slot(t);
        return fail(line_, std::format("unknown key `{}`", t[0]));
    }

    Status header(const std::vector<std::string_view>& t, std::string_view line) {
        const auto key = t[0];
        if (key == "title") {
            auto rest = line.substr(line.find("title") + 5);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
            while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.remove_suffix(1);
            if (rest.empty() || rest.size() > 200) return fail(line_, "title must have 1-200 characters");
            for (char c : rest)
                if (static_cast<unsigned char>(c) < 0x20) return fail(line_, "control character in title");
            f_.title = std::string(rest);
            return {};
        }
        if (key == "mechanisms") {
            if (t.size() < 2) return fail(line_, "at least one mechanism is required");
            for (std::size_t i = 1; i < t.size(); ++i) {
                for (char c : t[i])
                    if (!key_char(c)) return fail(line_, "invalid mechanism id");
                f_.mechanisms.emplace_back(t[i]);
            }
            return {};
        }
        if (key == "area") {
            if (t.size() != 7) return fail(line_, "area needs six coordinates");
            std::int32_t v[6];
            for (int i = 0; i < 6; ++i)
                if (!number(t[1 + i], v[i])) return fail(line_, "invalid area coordinate");
            f_.area_min = {v[0], v[1], v[2]};
            f_.area_max = {v[3], v[4], v[5]};
            if (v[0] > v[3] || v[1] > v[4] || v[2] > v[5]) return fail(line_, "area minimum exceeds maximum");
            if (static_cast<std::int64_t>(v[3]) - v[0] >= limits_.max_horizontal_extent
                || static_cast<std::int64_t>(v[5]) - v[2] >= limits_.max_horizontal_extent
                || static_cast<std::int64_t>(v[4]) - v[1] >= limits_.max_vertical_extent)
                return fail(line_, "area exceeds the extent limit");
            if (!contracts::within_border(f_.area_min) || !contracts::within_border(f_.area_max))
                return fail(line_, "area outside the world border");
            return {};
        }
        if (t.size() != 2) return fail(line_, std::format("`{}` takes one value", key));
        const auto value = t[1];
        if (key == "game-version") {
            for (char c : value)
                if (!id_char(c)) return fail(line_, "invalid game version");
            f_.game_version = std::string(value);
        } else if (key == "source") {
            const auto s = source_from(value);
            if (!s) return fail(line_, "unknown source");
            f_.source = *s;
        } else if (key == "harness") {
            for (char c : value)
                if (!id_char(c) && c != ':' && c != '+') return fail(line_, "invalid harness identity");
            f_.harness = std::string(value);
        } else if (key == "ticks") {
            if (!number(value, f_.ticks) || f_.ticks < 1 || f_.ticks > limits_.max_ticks) return fail(line_, "invalid tick count");
        } else if (key == "random-tick-speed") {
            if (!number(value, f_.random_tick_speed) || f_.random_tick_speed > 4096) return fail(line_, "invalid random tick speed");
        } else if (key == "neighbor-update-limit") {
            if (!number(value, f_.neighbor_update_limit) || f_.neighbor_update_limit < 1
                || f_.neighbor_update_limit > 1'000'000)
                return fail(line_, "invalid neighbor update limit");
        } else if (key == "changes") {
            if (value == "ordered") f_.changes = ChangeMode::ordered;
            else if (value == "unordered") f_.changes = ChangeMode::unordered;
            else if (value == "none") f_.changes = ChangeMode::none;
            else return fail(line_, "changes must be ordered, unordered or none");
        }
        return {};
    }

    Status finish_header(std::size_t line) {
        header_done_ = true;
        for (const auto* key : {"title", "game-version", "source", "harness", "mechanisms", "area", "ticks"})
            if (!seen_.contains(key)) return fail(line, std::format("missing required key `{}`", key));
        return {};
    }

    Status tick_of(std::string_view text, Tick lowest, Tick& out) {
        if (!number(text, out) || out < lowest || out > f_.ticks) return fail(line_, "tick outside the scenario");
        return {};
    }
    Status pos_of(const std::vector<std::string_view>& t, std::size_t i, BlockPos& out) {
        if (!number(t[i], out.x) || !number(t[i + 1], out.y) || !number(t[i + 2], out.z))
            return fail(line_, "invalid coordinate");
        if (!f_.contains(out)) return fail(line_, "position outside the area");
        return {};
    }
    Status state_of(std::string_view text, std::string& out) {
        if (auto s = parse_state(text); !s) return fail(line_, s.error());
        out = std::string(text);
        return {};
    }
    Status item_of(std::string_view text, bool allow_empty, std::string& out) {
        if (!(allow_empty && text == "empty") && !valid_id(text)) return fail(line_, "invalid item id");
        out = std::string(text);
        return {};
    }

    Status action(const std::vector<std::string_view>& t) {
        Action a;
        a.line = line_;
        if (t.size() < 6) return fail(line_, "incomplete action");
        if (auto ok = tick_of(t[1], 0, a.tick); !ok) return ok;
        if (!f_.actions.empty() && a.tick < f_.actions.back().tick) return fail(line_, "actions must be in tick order");
        if (auto ok = pos_of(t, 3, a.pos); !ok) return ok;
        if (t[2] == "set" && t.size() == 7) {
            a.kind = Action::Kind::set;
            if (auto ok = state_of(t[6], a.state); !ok) return ok;
        } else if (t[2] == "use" && t.size() == 6) {
            a.kind = Action::Kind::use;
        } else if (t[2] == "insert" && t.size() == 9) {
            a.kind = Action::Kind::insert;
            unsigned slot{}, count{};
            if (!number(t[6], slot) || slot > 255) return fail(line_, "invalid slot");
            if (auto ok = item_of(t[7], false, a.item); !ok) return ok;
            if (!number(t[8], count) || count < 1 || count > contracts::max_stack_size) return fail(line_, "invalid count");
            a.slot = static_cast<std::uint8_t>(slot);
            a.count = static_cast<std::uint8_t>(count);
        } else {
            return fail(line_, "unknown or malformed action");
        }
        f_.actions.push_back(std::move(a));
        return {};
    }

    Status change(const std::vector<std::string_view>& t) {
        if (f_.changes == ChangeMode::none) return fail(line_, "change lines require `changes ordered|unordered`");
        if (t.size() != 7) return fail(line_, "change needs tick, position, old and new state");
        Change c;
        c.line = line_;
        if (auto ok = tick_of(t[1], 1, c.tick); !ok) return ok;
        if (!f_.recorded_changes.empty() && c.tick < f_.recorded_changes.back().tick)
            return fail(line_, "changes must be in tick order");
        if (auto ok = pos_of(t, 2, c.pos); !ok) return ok;
        if (auto ok = state_of(t[5], c.before); !ok) return ok;
        if (auto ok = state_of(t[6], c.after); !ok) return ok;
        f_.recorded_changes.push_back(std::move(c));
        return {};
    }

    Status block(const std::vector<std::string_view>& t) {
        if (t.size() != 6) return fail(line_, "block probe needs tick, position and state");
        BlockProbe p;
        p.line = line_;
        if (auto ok = tick_of(t[1], 0, p.tick); !ok) return ok;
        if (auto ok = pos_of(t, 2, p.pos); !ok) return ok;
        if (t[5] != "?") {
            std::string s;
            if (auto ok = state_of(t[5], s); !ok) return ok;
            p.state.value = std::move(s);
        }
        f_.block_probes.push_back(std::move(p));
        return {};
    }

    Status slot(const std::vector<std::string_view>& t) {
        SlotProbe p;
        p.line = line_;
        if (t.size() != 7 && t.size() != 8) return fail(line_, "slot probe needs tick, position, slot, item and count");
        if (auto ok = tick_of(t[1], 0, p.tick); !ok) return ok;
        if (auto ok = pos_of(t, 2, p.pos); !ok) return ok;
        unsigned slot{};
        if (!number(t[5], slot) || slot > 255) return fail(line_, "invalid slot");
        p.slot = static_cast<std::uint8_t>(slot);
        if (t.size() == 7) {
            if (t[6] != "?") return fail(line_, "slot probe needs an item and a count, or `?`");
        } else {
            std::string item;
            if (auto ok = item_of(t[6], true, item); !ok) return ok;
            unsigned count{};
            if (!number(t[7], count) || count > contracts::max_stack_size) return fail(line_, "invalid count");
            if ((item == "empty") != (count == 0)) return fail(line_, "empty slots have count 0");
            p.item.value = std::move(item);
            p.count = static_cast<std::uint8_t>(count);
        }
        f_.slot_probes.push_back(std::move(p));
        return {};
    }

    std::string_view text_;
    const FormatLimits& limits_;
    std::size_t line_{};
    bool saw_magic_{};
    bool header_done_{};
    std::set<std::string> seen_;
    Fixture f_;
};
} // namespace

std::expected<StateSpec, std::string> parse_state(std::string_view text) {
    StateSpec spec;
    const auto bracket = text.find('[');
    const auto id = text.substr(0, bracket);
    if (!valid_id(id)) return std::unexpected(std::format("invalid block id `{}`", id));
    spec.id = std::string(id);
    if (bracket == std::string_view::npos) return spec;
    if (text.back() != ']') return std::unexpected("unterminated property list");
    auto body = text.substr(bracket + 1, text.size() - bracket - 2);
    if (body.empty()) return std::unexpected("empty property list");
    std::set<std::string> keys;
    while (true) {
        const auto comma = body.find(',');
        const auto pair = body.substr(0, comma);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1 == pair.size()) return std::unexpected("malformed property");
        const auto key = pair.substr(0, eq), value = pair.substr(eq + 1);
        for (char c : key)
            if (!key_char(c)) return std::unexpected("invalid property name");
        for (char c : value)
            if (!key_char(c)) return std::unexpected("invalid property value");
        if (!keys.insert(std::string(key)).second) return std::unexpected(std::format("duplicate property `{}`", key));
        spec.properties.emplace_back(std::string(key), std::string(value));
        if (comma == std::string_view::npos) break;
        body.remove_prefix(comma + 1);
    }
    return spec;
}

bool Fixture::is_skeleton() const noexcept {
    for (const auto& p : block_probes)
        if (!p.state.recorded()) return true;
    for (const auto& p : slot_probes)
        if (!p.item.recorded()) return true;
    return false;
}

std::string_view source_name(EvidenceSource source) noexcept {
    switch (source) {
    case EvidenceSource::none: return "none";
    case EvidenceSource::documented_behavior: return "documented_behavior";
    case EvidenceSource::harness_trace: return "harness_trace";
    case EvidenceSource::controlled_experiment: return "controlled_experiment";
    case EvidenceSource::circuit_reference: return "circuit_reference";
    }
    return "none";
}

std::expected<Fixture, FormatError> parse_fixture(std::string_view text, const FormatLimits& limits) {
    return Parser(text, limits).run();
}

std::string write_fixture(const Fixture& f) {
    std::string out = "circuit-trace 1\n";
    out += std::format("title {}\ngame-version {}\nsource {}\nharness {}\nmechanisms", f.title, f.game_version,
                       source_name(f.source), f.harness);
    for (const auto& m : f.mechanisms) out += " " + m;
    out += std::format("\narea {} {}\nticks {}\n", pos_text(f.area_min), pos_text(f.area_max), f.ticks);
    if (f.random_tick_speed != 0) out += std::format("random-tick-speed {}\n", f.random_tick_speed);
    if (f.neighbor_update_limit != 0) out += std::format("neighbor-update-limit {}\n", f.neighbor_update_limit);
    if (f.changes != ChangeMode::none) out += std::format("changes {}\n", f.changes == ChangeMode::ordered ? "ordered" : "unordered");
    std::map<Tick, std::vector<std::string>> body;
    for (const auto& a : f.actions) {
        switch (a.kind) {
        case Action::Kind::set: body[a.tick].push_back(std::format("at {} set {} {}", a.tick, pos_text(a.pos), a.state)); break;
        case Action::Kind::use: body[a.tick].push_back(std::format("at {} use {}", a.tick, pos_text(a.pos))); break;
        case Action::Kind::insert:
            body[a.tick].push_back(std::format("at {} insert {} {} {} {}", a.tick, pos_text(a.pos), a.slot, a.item, a.count));
            break;
        }
    }
    for (const auto& c : f.recorded_changes)
        body[c.tick].push_back(std::format("change {} {} {} {}", c.tick, pos_text(c.pos), c.before, c.after));
    auto blocks = f.block_probes;
    std::ranges::stable_sort(blocks, [](const BlockProbe& a, const BlockProbe& b) { return std::tie(a.tick, a.pos) < std::tie(b.tick, b.pos); });
    for (const auto& p : blocks)
        body[p.tick].push_back(std::format("block {} {} {}", p.tick, pos_text(p.pos), p.state.value.value_or("?")));
    auto slots = f.slot_probes;
    std::ranges::stable_sort(slots, [](const SlotProbe& a, const SlotProbe& b) {
        return std::tie(a.tick, a.pos, a.slot) < std::tie(b.tick, b.pos, b.slot);
    });
    for (const auto& p : slots)
        body[p.tick].push_back(p.item.recorded() ? std::format("slot {} {} {} {} {}", p.tick, pos_text(p.pos), p.slot, *p.item.value, p.count)
                                                 : std::format("slot {} {} {} ?", p.tick, pos_text(p.pos), p.slot));
    for (const auto& [tick, lines] : body) {
        out += "\n";
        for (const auto& l : lines) out += l + "\n";
    }
    return out;
}

} // namespace circuit::evidence
