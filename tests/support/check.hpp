// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Test cases, their registration and the runner of a test program
// (tests/README.md describes the layout and the environment variables).
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace circuit::test {

inline void require(bool ok, std::string_view message,
                    std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error(std::format("{}:{}: {}", where.file_name(), where.line(), message));
}

namespace detail {
struct Case {
    std::string name;
    std::function<void()> body;
};
// Cases registered by CIRCUIT_TEST in any source file of the program.
inline std::vector<Case>& registered_cases() {
    static std::vector<Case> cases;
    return cases;
}
// Set when the running case asked for an official jar that was not given.
inline std::atomic<bool>& jar_missing() {
    static std::atomic<bool> missing{false};
    return missing;
}
// An environment variable that is set, not empty and not "0".
inline bool enabled(const char* variable) {
    const char* value = std::getenv(variable);
    return value && *value && std::string_view(value) != "0";
}
} // namespace detail

// CIRCUIT_REQUIRE_JARS=1: the checks that need an official server jar must run.
inline bool jars_required() { return detail::enabled("CIRCUIT_REQUIRE_JARS"); }

// The path of the official server jar an environment variable names
// (CIRCUIT_VANILLA_JAR_1_21_8, CIRCUIT_VANILLA_JAR_1_21_4), or nullptr when
// the variable is unset or empty. A case that gets nullptr leaves out the
// checks that need the jar and is counted as skipped for a missing jar; with
// CIRCUIT_REQUIRE_JARS=1 it fails instead. Tests read the jar variables only
// through this function, so that no check is left out silently.
inline const char* official_jar(const char* variable) {
    if (const char* jar = std::getenv(variable); jar && *jar) return jar;
    if (jars_required())
        throw std::runtime_error(std::format("{} is not set, and CIRCUIT_REQUIRE_JARS requires the official server jars", variable));
    detail::jar_missing().store(true);
    return nullptr;
}

// Registers a case before main() runs; see CIRCUIT_TEST.
struct Registration {
    Registration(std::string_view name, void (*body)()) { detail::registered_cases().push_back({std::string(name), body}); }
};

#define CIRCUIT_TEST_JOIN_(a, b) a##b
#define CIRCUIT_TEST_JOIN(a, b) CIRCUIT_TEST_JOIN_(a, b)
#define CIRCUIT_TEST_AT_(name, id)                                                                \
    static void CIRCUIT_TEST_JOIN(circuit_test_body_, id)();                                      \
    static const ::circuit::test::Registration CIRCUIT_TEST_JOIN(circuit_test_registration_, id){ \
        name, &CIRCUIT_TEST_JOIN(circuit_test_body_, id)};                                        \
    static void CIRCUIT_TEST_JOIN(circuit_test_body_, id)()

// Defines a case at namespace scope in any source file of a test program:
//
//     CIRCUIT_TEST("levers power the block they are on") {
//         require(...);
//     }
//
// The program's Suite::finish() runs it with every other case of the program.
// Names are unique within a program.
#define CIRCUIT_TEST(name) CIRCUIT_TEST_AT_(name, __COUNTER__)

// The cases of one test program. Cases come from CIRCUIT_TEST in any source
// file and from run() in the file that holds main(); finish() runs them all,
// sorted by name, reports each failure, and returns the process exit code:
// 0 when every case passed, 1 when a case failed, 2 when the selection or the
// set of names is invalid.
//
// Environment:
// - CIRCUIT_TEST_FILTER=text  only the cases whose names contain the text;
// - CIRCUIT_TEST_SHARD=i/n    of those, sorted by name, only the cases whose
//                             index modulo n is i (0 <= i < n), so that n
//                             processes run every case exactly once;
// - CIRCUIT_TEST_LIST=1       print the selected names, one per line, and run
//                             nothing;
// - CIRCUIT_TEST_TIMES        print each passed case's duration;
// - CIRCUIT_REQUIRE_JARS=1    see official_jar().
//
// The last line is "<program>: N passed, M failed, K skipped for a missing
// jar". The K cases are among the N: they passed without the checks that need
// an official jar.
class Suite {
public:
    explicit Suite(std::string_view name) : name_(name) {}

    // Adds a case. The body runs in finish(), not here: what it captures by
    // reference must live until finish() returns, and the statements between
    // run() calls execute before any case does.
    template <class F>
    void run(std::string_view case_name, F&& body) {
        cases_.push_back({std::string(case_name), std::function<void()>(std::forward<F>(body))});
    }

    int finish() {
        std::vector<detail::Case> cases = std::move(cases_);
        for (const auto& c : detail::registered_cases()) cases.push_back(c);
        std::ranges::sort(cases, {}, &detail::Case::name);
        if (const auto twice = std::ranges::adjacent_find(cases, {}, &detail::Case::name); twice != cases.end()) {
            std::cerr << name_ << ": two cases are named \"" << twice->name << "\"\n";
            return 2;
        }
        if (const char* filter = std::getenv("CIRCUIT_TEST_FILTER"))
            std::erase_if(cases, [&](const detail::Case& c) { return c.name.find(filter) == std::string::npos; });
        if (const char* shard = std::getenv("CIRCUIT_TEST_SHARD")) {
            const std::string_view text(shard);
            const auto slash = text.find('/');
            const auto number = [](std::string_view digits, std::size_t& out) {
                const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), out);
                return !digits.empty() && error == std::errc{} && end == digits.data() + digits.size();
            };
            std::size_t index = 0, count = 0;
            if (slash == std::string_view::npos || !number(text.substr(0, slash), index) || !number(text.substr(slash + 1), count) ||
                index >= count) {
                std::cerr << name_ << ": CIRCUIT_TEST_SHARD is \"" << text << "\"; expected i/n with 0 <= i < n\n";
                return 2;
            }
            std::vector<detail::Case> mine;
            for (std::size_t i = index; i < cases.size(); i += count) mine.push_back(std::move(cases[i]));
            cases = std::move(mine);
        }
        if (detail::enabled("CIRCUIT_TEST_LIST")) {
            for (const auto& c : cases) std::cout << c.name << '\n';
            return 0;
        }
        const bool times = std::getenv("CIRCUIT_TEST_TIMES") != nullptr;
        int passed = 0, failed = 0, skipped = 0;
        for (const auto& c : cases) {
            detail::jar_missing().store(false);
            const auto start = std::chrono::steady_clock::now();
            try {
                c.body();
                ++passed;
                if (detail::jar_missing().load()) ++skipped;
                if (times)
                    std::cout << std::format("  [{:.1f} s] {}\n",
                                             std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), c.name);
            } catch (const std::exception& e) {
                ++failed;
                std::cerr << "FAIL " << name_ << "/" << c.name << ": " << e.what() << '\n';
            } catch (...) {
                ++failed;
                std::cerr << "FAIL " << name_ << "/" << c.name << ": an exception that is not a std::exception\n";
            }
        }
        std::cout << name_ << ": " << passed << " passed, " << failed << " failed, " << skipped << " skipped for a missing jar" << std::endl;
        return failed == 0 ? 0 : 1;
    }

private:
    std::string name_;
    std::vector<detail::Case> cases_;
};

} // namespace circuit::test
