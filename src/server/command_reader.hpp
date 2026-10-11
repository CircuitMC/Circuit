// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/nbt.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Reads a command's input as the target versions' command parser does, as
// observed on both official servers with protocol clients (minecraft.wiki
// "Commands" for the argument types). A command is a tree of literals and
// arguments separated by single spaces:
// - A space is read only when at least one character follows it; a lone
//   trailing space or text after a node without children is an unknown
//   argument, text right after an argument a missing separator.
// - A word matching a literal selects it (case-sensitive); otherwise an
//   argument is read there, or the word is an unknown argument.
// - A node that does not complete a command but ends the input is an unknown
//   command.
// A refused input yields the official translation key, its arguments and the
// position reading stopped at, from which the server builds the error and
// its context line.
namespace circuit::server {

struct CommandError {
    std::size_t cursor{};
    std::string key;
    protocol::nbt::Tag with = protocol::nbt::Tag::list(protocol::nbt::TagType::string);
};

// Translation arguments: plain strings or integers.
protocol::nbt::Tag string_arguments(std::vector<std::string> values);
protocol::nbt::Tag int_arguments(std::vector<std::int32_t> values);

class CommandReader {
public:
    // Reads `input` (without the slash) from `at`, after the node read so far.
    CommandReader(std::string_view input, std::size_t at) : input_(input), at_(at) {}

    [[nodiscard]] bool done() const noexcept { return at_ == input_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return at_; }
    // The text up to the next space: what a literal would match.
    [[nodiscard]] std::string_view word() const noexcept;
    void skip(std::size_t n) noexcept { at_ += n; }
    [[nodiscard]] CommandError error(std::string_view key,
                                     protocol::nbt::Tag with = protocol::nbt::Tag::list(protocol::nbt::TagType::string)) const;

    // Moves from the end of a node to its child, over the separating space.
    std::optional<CommandError> child();
    // Ends a node that completes the command: nothing may follow it.
    std::optional<CommandError> end();
    // A node without a command that ended the input.
    [[nodiscard]] CommandError incomplete() const { return error("command.unknown.command"); }

    // Arguments. Integers and floats read the characters 0-9, '.' and '-'.
    std::expected<std::int32_t, CommandError> integer(std::int32_t min, std::int32_t max);
    std::expected<float, CommandError> real();
    // true or false, bare or quoted with " or ' (with \ escapes).
    std::expected<bool, CommandError> boolean();
    // A time: a float, then the unit d (24000 ticks), s (20) or t (1, the
    // default); rounded half up to whole ticks (saturating), at least `min`.
    std::expected<std::int32_t, CommandError> time(std::int32_t min = 0);

private:
    std::string_view unquoted();
    std::expected<std::string, CommandError> string();

    std::string_view input_;
    std::size_t at_;
};

} // namespace circuit::server
