// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/auth.hpp>
#include <circuit/protocol.hpp>
#include <circuit/storage.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
} // namespace

int main() {
    for (const auto id : {"java-1.21.8", "java-1.20.4"}) {
        const auto selected = circuit::protocol::builtin_registry::select(id);
        check(selected.has_value(), "declared candidate is selectable");
        if (selected) {
            const auto& descriptor = selected->descriptor();
            check(descriptor.id == id, "selection returns requested version");
            check(!descriptor.supports_play, "unimplemented adapter never claims Play support");
            check(!descriptor.protocol_number, "unverified protocol numbers stay unset");
        }
    }
    const auto unknown = circuit::protocol::builtin_registry::select("java-unknown");
    check(!unknown && unknown.error() == circuit::protocol::selection_error::unknown_version,
          "unknown version rejected rather than silently falling back");

    circuit::auth::deny_authenticator provider;
    circuit::auth::login_request request{"ClaimedPlayer", "unverified", {}};
    const auto identity = provider.authenticate(request,
        std::chrono::steady_clock::now() + std::chrono::seconds{1});
    check(!identity && identity.error() == circuit::auth::error::unavailable,
          "unconfigured authentication fails closed for a claimed identity");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "interface tests passed\n";
    return EXIT_SUCCESS;
}
