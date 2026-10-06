// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/auth/json.hpp>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace circuit::auth {

const Json* Json::find(std::string_view key) const noexcept {
    for (const auto& [k, v] : members_)
        if (k == key) return &v;
    return nullptr;
}

const std::string* Json::string(std::string_view key) const noexcept {
    const auto* v = find(key);
    return v && v->kind_ == Kind::string ? &v->string_ : nullptr;
}

struct JsonParser {
    std::string_view text;
    std::size_t at{};
    std::size_t max_depth{};

    void skip() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
    }
    std::unexpected<std::string> error(std::string_view what) const {
        return std::unexpected(std::format("JSON: {} at offset {}", what, at));
    }
    bool literal(std::string_view word) {
        if (text.substr(at, word.size()) != word) return false;
        at += word.size();
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::expected<std::uint32_t, std::string> hex4() {
        if (at + 4 > text.size()) return error("short escape");
        std::uint32_t v = 0;
        const auto [end, ec] = std::from_chars(text.data() + at, text.data() + at + 4, v, 16);
        if (ec != std::errc{} || end != text.data() + at + 4) return error("bad escape");
        at += 4;
        return v;
    }

    std::expected<std::string, std::string> string() {
        if (at >= text.size() || text[at] != '"') return error("expected a string");
        ++at;
        std::string out;
        while (at < text.size()) {
            const char c = text[at++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return error("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size()) break;
            switch (text[at++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto cp = hex4();
                if (!cp) return std::unexpected(cp.error());
                if (*cp >= 0xD800 && *cp <= 0xDBFF && text.substr(at, 2) == "\\u") {
                    at += 2;
                    auto low = hex4();
                    if (!low) return std::unexpected(low.error());
                    if (*low < 0xDC00 || *low > 0xDFFF) return error("unpaired surrogate");
                    *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
                } else if (*cp >= 0xD800 && *cp <= 0xDFFF) {
                    return error("unpaired surrogate");
                }
                append_utf8(out, *cp);
                break;
            }
            default: return error("unknown escape");
            }
        }
        return error("unterminated string");
    }

    std::expected<Json, std::string> value(std::size_t depth) {
        if (depth > max_depth) return error("nesting too deep");
        skip();
        if (at >= text.size()) return error("unexpected end");
        Json j;
        const char c = text[at];
        if (c == '{') {
            ++at;
            j.kind_ = Json::Kind::object;
            skip();
            if (at < text.size() && text[at] == '}') {
                ++at;
                return j;
            }
            for (;;) {
                skip();
                auto key = string();
                if (!key) return std::unexpected(key.error());
                skip();
                if (at >= text.size() || text[at] != ':') return error("expected ':'");
                ++at;
                auto v = value(depth + 1);
                if (!v) return std::unexpected(v.error());
                j.members_.emplace_back(std::move(*key), std::move(*v));
                skip();
                if (at < text.size() && text[at] == ',') { ++at; continue; }
                if (at < text.size() && text[at] == '}') { ++at; return j; }
                return error("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++at;
            j.kind_ = Json::Kind::array;
            skip();
            if (at < text.size() && text[at] == ']') {
                ++at;
                return j;
            }
            for (;;) {
                auto v = value(depth + 1);
                if (!v) return std::unexpected(v.error());
                j.items_.push_back(std::move(*v));
                skip();
                if (at < text.size() && text[at] == ',') { ++at; continue; }
                if (at < text.size() && text[at] == ']') { ++at; return j; }
                return error("expected ',' or ']'");
            }
        }
        if (c == '"') {
            auto s = string();
            if (!s) return std::unexpected(s.error());
            j.kind_ = Json::Kind::string;
            j.string_ = std::move(*s);
            return j;
        }
        if (literal("true")) { j.kind_ = Json::Kind::boolean; j.boolean_ = true; return j; }
        if (literal("false")) { j.kind_ = Json::Kind::boolean; return j; }
        if (literal("null")) return j;
        // Numbers: JSON syntax, parsed as double.
        const auto start = at;
        if (at < text.size() && text[at] == '-') ++at;
        while (at < text.size() && (std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '.' || text[at] == 'e'
                                    || text[at] == 'E' || text[at] == '+' || text[at] == '-'))
            ++at;
        if (at == start) return error("unexpected character");
        double v{};
        const auto [end, ec] = std::from_chars(text.data() + start, text.data() + at, v);
        if (ec != std::errc{} || end != text.data() + at || !std::isfinite(v)) return error("bad number");
        j.kind_ = Json::Kind::number;
        j.number_ = v;
        return j;
    }
};

std::expected<Json, std::string> Json::parse(std::string_view text, std::size_t max_depth) {
    JsonParser p{text, 0, max_depth};
    auto v = p.value(0);
    if (!v) return v;
    p.skip();
    if (p.at != text.size()) return p.error("trailing data");
    return v;
}

} // namespace circuit::auth
