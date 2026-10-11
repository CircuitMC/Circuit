// SPDX-License-Identifier: AGPL-3.0-only
#include "command_reader.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace circuit::server {

namespace nbt = protocol::nbt;

nbt::Tag string_arguments(std::vector<std::string> values) {
    auto list = nbt::Tag::list(nbt::TagType::string);
    for (auto& v : values) list.push(nbt::Tag::string(std::move(v)));
    return list;
}

nbt::Tag int_arguments(std::vector<std::int32_t> values) {
    auto list = nbt::Tag::list(nbt::TagType::int_);
    for (const auto v : values) list.push(nbt::Tag::int_(v));
    return list;
}

std::string_view CommandReader::word() const noexcept {
    return input_.substr(at_, std::min(input_.find(' ', at_), input_.size()) - at_);
}

CommandError CommandReader::error(std::string_view key, nbt::Tag with) const { return {at_, std::string(key), std::move(with)}; }

std::optional<CommandError> CommandReader::child() {
    if (input_[at_] != ' ') return error("command.expected.separator");
    // A space is read only when something follows it.
    if (input_.size() - at_ < 2) return error("command.unknown.argument");
    ++at_;
    return std::nullopt;
}

std::optional<CommandError> CommandReader::end() {
    if (done()) return std::nullopt;
    if (auto e = child()) return e;
    return error("command.unknown.argument");
}

std::string_view CommandReader::unquoted() {
    const auto start = at_;
    const auto allowed = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.' || ch == '+';
    };
    while (at_ < input_.size() && allowed(input_[at_])) ++at_;
    return input_.substr(start, at_ - start);
}

std::expected<std::string, CommandError> CommandReader::string() {
    if (done() || (input_[at_] != '"' && input_[at_] != '\'')) return std::string(unquoted());
    const char quote = input_[at_++];
    std::string out;
    bool escaped = false;
    for (; at_ < input_.size(); ++at_) {
        const char ch = input_[at_];
        if (escaped) {
            if (ch != quote && ch != '\\') return std::unexpected(error("parsing.quote.escape", string_arguments({std::string(1, ch)})));
            out += ch;
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == quote) {
            ++at_;
            return out;
        } else {
            out += ch;
        }
    }
    return std::unexpected(error("parsing.quote.expected.end"));
}

std::expected<std::int32_t, CommandError> CommandReader::integer(std::int32_t min, std::int32_t max) {
    const auto start = at_;
    while (at_ < input_.size() && (std::isdigit(static_cast<unsigned char>(input_[at_])) || input_[at_] == '.' || input_[at_] == '-')) ++at_;
    const auto number = input_.substr(start, at_ - start);
    if (number.empty()) return std::unexpected(error("parsing.int.expected"));
    std::int32_t value{};
    const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
    at_ = start; // bounds and parse errors point at the number
    if (ec != std::errc{} || end != number.data() + number.size())
        return std::unexpected(error("parsing.int.invalid", string_arguments({std::string(number)})));
    if (value < min) return std::unexpected(error("argument.integer.low", int_arguments({min, value})));
    if (value > max) return std::unexpected(error("argument.integer.big", int_arguments({max, value})));
    at_ += number.size();
    return value;
}

std::expected<float, CommandError> CommandReader::real() {
    const auto start = at_;
    while (at_ < input_.size() && (std::isdigit(static_cast<unsigned char>(input_[at_])) || input_[at_] == '.' || input_[at_] == '-')) ++at_;
    const auto number = input_.substr(start, at_ - start);
    if (number.empty()) return std::unexpected(error("parsing.float.expected"));
    float value{};
    auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (ec == std::errc::result_out_of_range && end == number.data() + number.size()) {
        // Too large or too small for a float: infinite or (nearly) zero, as
        // the official parser reads it.
        const std::string text(number);
        value = static_cast<float>(std::strtod(text.c_str(), nullptr));
        ec = std::errc{};
    }
    if (ec != std::errc{} || end != number.data() + number.size()) {
        at_ = start;
        return std::unexpected(error("parsing.float.invalid", string_arguments({std::string(number)})));
    }
    return value;
}

std::expected<bool, CommandError> CommandReader::boolean() {
    const auto start = at_;
    const auto text = string();
    if (!text) return std::unexpected(text.error());
    if (text->empty()) return std::unexpected(error("parsing.bool.expected"));
    if (*text != "true" && *text != "false") {
        at_ = start;
        return std::unexpected(error("parsing.bool.invalid", string_arguments({*text})));
    }
    return *text == "true";
}

std::expected<std::int32_t, CommandError> CommandReader::time(std::int32_t min) {
    const auto value = real();
    if (!value) return std::unexpected(value.error());
    const auto unit = unquoted();
    const int factor = unit.empty() || unit == "t" ? 1 : unit == "s" ? 20 : unit == "d" ? 24000 : 0;
    if (factor == 0) return std::unexpected(error("argument.time.invalid_unit"));
    // The product of floats, rounded half up to an int that saturates.
    const float product = *value * static_cast<float>(factor);
    std::int32_t ticks = 0;
    if (!std::isnan(product)) {
        const double rounded = std::floor(static_cast<double>(product) + 0.5);
        ticks = rounded >= static_cast<double>(std::numeric_limits<std::int32_t>::max()) ? std::numeric_limits<std::int32_t>::max()
              : rounded <= static_cast<double>(std::numeric_limits<std::int32_t>::min()) ? std::numeric_limits<std::int32_t>::min()
                                                                                         : static_cast<std::int32_t>(rounded);
    }
    if (ticks < min) return std::unexpected(error("argument.time.tick_count_too_low", int_arguments({min, ticks})));
    return ticks;
}

} // namespace circuit::server
