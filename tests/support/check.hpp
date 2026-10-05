// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <format>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace circuit::test {

inline void require(bool ok, std::string_view message,
                    std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error(std::format("{}:{}: {}", where.file_name(), where.line(), message));
}

// Runs named cases, reports each failure, and returns a process exit code.
class Suite {
public:
    explicit Suite(std::string_view name) : name_(name) {}
    template <class F>
    void run(std::string_view case_name, F&& body) {
        try {
            body();
            ++passed_;
        } catch (const std::exception& e) {
            ++failed_;
            std::cerr << "FAIL " << name_ << "/" << case_name << ": " << e.what() << '\n';
        }
    }
    int finish() const {
        std::cout << name_ << ": " << passed_ << " passed, " << failed_ << " failed\n";
        return failed_ == 0 ? 0 : 1;
    }

private:
    std::string name_;
    int passed_{}, failed_{};
};

} // namespace circuit::test
