// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <cstdint>

// Random sequences (minecraft.wiki "Random sequence"): the Xoroshiro128++
// generators both official servers keep per namespaced id for loot tables.
// A sequence starts when first used: the salt, XORed with the world seed,
// expands to 128 bits (low half the value XOR 0x6a09e667f3bcc909, high half
// the low half plus 0x9e3779b97f4a7c15), XORs the MD5 of the id (its first
// eight bytes, big-endian, into the low half, the last eight into the high
// half) and passes each half through mixStafford13. Floats take the top 24
// bits of the next value, bounded integers the low 32 bits by Lemire's
// method; both fitted to every roll of the loot_many skeleton on both
// versions, and the states they leave equal the official saved ones.
namespace circuit::gameplay {

// The MD5 of a sequence's id as two big-endian halves; the data layer
// computes it.
using SequenceIdHash = std::array<std::uint64_t, 2>;

// The world's sequence settings (minecraft.wiki "Random sequence").
struct SequenceSettings {
    std::int32_t salt{};
    bool include_world_seed{true};
    bool include_sequence_id{true};
    bool operator==(const SequenceSettings&) const = default;
};

class RandomSequence {
public:
    constexpr RandomSequence() noexcept = default;
    constexpr RandomSequence(std::uint64_t low, std::uint64_t high) noexcept : low_(low), high_(high) {
        // An all-zero state would stay zero.
        if (low_ == 0 && high_ == 0) {
            low_ = 0x9e3779b97f4a7c15ULL;
            high_ = 0x6a09e667f3bcc909ULL;
        }
    }
    static constexpr RandomSequence create(std::int64_t world_seed, const SequenceIdHash& id, const SequenceSettings& settings) noexcept {
        std::uint64_t value = static_cast<std::uint64_t>(static_cast<std::int64_t>(settings.salt));
        if (settings.include_world_seed) value ^= static_cast<std::uint64_t>(world_seed);
        std::uint64_t low = value ^ 0x6a09e667f3bcc909ULL;
        std::uint64_t high = low + 0x9e3779b97f4a7c15ULL;
        if (settings.include_sequence_id) {
            low ^= id[0];
            high ^= id[1];
        }
        return {mix(low), mix(high)};
    }

    [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }
    [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }

    constexpr std::uint64_t next_long() noexcept {
        const std::uint64_t l = low_;
        std::uint64_t h = high_;
        const std::uint64_t result = rotl(l + h, 17) + l;
        h ^= l;
        low_ = rotl(l, 49) ^ h ^ (h << 21);
        high_ = rotl(h, 28);
        return result;
    }
    constexpr float next_float() noexcept { return static_cast<float>(next_long() >> 40) * 0x1.0p-24f; }
    // Uniform in [0, bound); bound > 0.
    constexpr std::int32_t next_int(std::int32_t bound) noexcept {
        const auto b = static_cast<std::uint64_t>(bound);
        std::uint64_t m = (next_long() & 0xFFFFFFFFULL) * b;
        if ((m & 0xFFFFFFFFULL) < b) {
            const std::uint64_t threshold = ((std::uint64_t{1} << 32) - b) % b;
            while ((m & 0xFFFFFFFFULL) < threshold) m = (next_long() & 0xFFFFFFFFULL) * b;
        }
        return static_cast<std::int32_t>(m >> 32);
    }

    constexpr bool operator==(const RandomSequence&) const noexcept = default;

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    static constexpr std::uint64_t mix(std::uint64_t z) noexcept {
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    std::uint64_t low_{0x9e3779b97f4a7c15ULL};
    std::uint64_t high_{0x6a09e667f3bcc909ULL};
};

} // namespace circuit::gameplay
