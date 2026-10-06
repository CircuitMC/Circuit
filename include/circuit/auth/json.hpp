// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small bounded JSON reader for service responses.
namespace circuit::auth {

class Json {
public:
    enum class Kind { null, boolean, number, string, array, object };

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool as_bool() const noexcept { return boolean_; }
    [[nodiscard]] double as_number() const noexcept { return number_; }
    [[nodiscard]] const std::string& as_string() const noexcept { return string_; }
    [[nodiscard]] const std::vector<Json>& items() const noexcept { return items_; }
    [[nodiscard]] const std::vector<std::pair<std::string, Json>>& members() const noexcept { return members_; }
    // Object member, or nullptr.
    [[nodiscard]] const Json* find(std::string_view key) const noexcept;
    // String member, or nullptr when absent or not a string.
    [[nodiscard]] const std::string* string(std::string_view key) const noexcept;

    static std::expected<Json, std::string> parse(std::string_view text, std::size_t max_depth = 64);

private:
    friend struct JsonParser;
    Kind kind_{Kind::null};
    bool boolean_{};
    double number_{};
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

} // namespace circuit::auth
