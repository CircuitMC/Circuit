// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace circuit::protocol {

enum class implementation_status { planned, experimental, supported };
enum class selection_error { unknown_version };
enum class decode_error { unsupported, incomplete, malformed, size_limit };

struct version_descriptor {
    std::string_view id;
    std::string_view edition;
    std::string_view game_version;
    // Filled only after checking the upstream protocol definition and fixtures.
    std::optional<std::int32_t> protocol_number;
    implementation_status status;
    bool supports_play;
};

// Selecting a version chooses ONE adapter for the server process. It does not
// claim wire compatibility: these candidates currently have no play codec.
class selected_version {
public:
    explicit constexpr selected_version(version_descriptor value) : value_(value) {}
    [[nodiscard]] constexpr const version_descriptor& descriptor() const noexcept {
        return value_;
    }

private:
    version_descriptor value_;
};

class builtin_registry {
public:
    inline static constexpr std::array candidates{
        version_descriptor{"java-1.21.8", "java", "1.21.8", std::nullopt,
                           implementation_status::planned, false},
        version_descriptor{"java-1.20.4", "java", "1.20.4", std::nullopt,
                           implementation_status::planned, false},
    };

    [[nodiscard]] static constexpr std::expected<selected_version, selection_error>
    select(std::string_view id) noexcept {
        for (const auto& candidate : candidates) {
            if (candidate.id == id) return selected_version{candidate};
        }
        return std::unexpected(selection_error::unknown_version);
    }
};

struct decoded_frame {
    std::int32_t packet_id;
    std::vector<std::byte> payload;
    std::size_t consumed_bytes;
};

// Connection state, size limits, compression, registries and semantic conversion
// belong to the concrete per-version adapter. These are boundaries, not codecs.
class version_adapter {
public:
    virtual ~version_adapter() = default;
    virtual const version_descriptor& descriptor() const noexcept = 0;
    virtual std::expected<decoded_frame, decode_error>
    decode(std::span<const std::byte> input, std::size_t maximum_frame_bytes) = 0;
};

} // namespace circuit::protocol
