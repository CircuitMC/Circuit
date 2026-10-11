// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/block.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// Block light and sky light, as both official servers compute them
// (minecraft.wiki "Light"; checked with light probes on both).
//
// Every state has an emission, a dampening (0, 1 or 15) and possibly an
// occlusion shape (block_registry.hpp). Light passes from a cell to a
// neighbor unless the faces of their occlusion shapes between them together
// cover the face; it arrives one level lower, or lower by the neighbor's
// dampening when that is more. A cell's block light is the most of its own
// emission and what its neighbors give it. Sky light comes from above the
// world: level 15 passes straight down undiminished into cells that dampen
// nothing, and otherwise spreads like block light.
//
// The light is a function of the blocks: a field holds it for a level's
// loaded columns and follows changes incrementally (columns loaded and
// unloaded, cells changed), always ending at the same values.
namespace circuit::gameplay {

struct LightInfo {
    std::uint8_t emission{};
    std::uint8_t dampening{};
    std::uint16_t shape{}; // light occlusion shape (registry::light_shape), 0: none
    bool operator==(const LightInfo&) const = default;
};
LightInfo light_info(BlockState s) noexcept;

enum class LightKind : std::uint8_t { block, sky };

// The level light gives a neighbor on `side` of a cell lit at `level`.
std::uint8_t light_passed(LightKind kind, std::uint8_t level, const LightInfo& from, const LightInfo& to,
                          contracts::Direction side) noexcept;

// What a light field reads: the states of loaded columns.
class LightWorld {
public:
    virtual ~LightWorld() = default;
    [[nodiscard]] virtual BlockState block(contracts::BlockPos pos) const = 0;
    [[nodiscard]] virtual bool loaded(contracts::ChunkPos chunk) const = 0;
    // A hint: whether a section (from the lowest, 0) of a loaded column holds only air.
    [[nodiscard]] virtual bool empty(contracts::ChunkPos, std::int32_t) const { return false; }
};

class LightField {
public:
    LightField(std::int32_t min_y, std::int32_t height);
    LightField(LightField&&) noexcept;
    LightField& operator=(LightField&&) noexcept;
    ~LightField();

    // Light at a position; 0 outside the field's columns and the world height.
    // Reads (get, has, section) may run on several threads at once while the
    // field does not change.
    [[nodiscard]] std::uint8_t get(LightKind kind, contracts::BlockPos pos) const noexcept;
    [[nodiscard]] bool has(contracts::ChunkPos chunk) const noexcept;
    [[nodiscard]] std::vector<contracts::ChunkPos> columns() const;

    // One section's light (from the lowest, 0) as the protocol packs it: two
    // cells per byte, the even index in the low half, cells in y, z, x
    // order. False when the column is not in the field.
    bool section(LightKind kind, contracts::ChunkPos chunk, std::int32_t index, std::array<std::uint8_t, 2048>& out) const;
    // The sections whose light changed since the last call, in no order.
    std::vector<std::pair<contracts::ChunkPos, std::int32_t>> take_changed();

    // A column joins the field: `world` reports it loaded.
    void load(contracts::ChunkPos chunk, const LightWorld& world);
    // A column leaves the field: `world` no longer reports it loaded.
    void unload(contracts::ChunkPos chunk, const LightWorld& world);
    // A cell of a column in the field was `before` and is now what `world`
    // reports; every other cell is as `world` reports.
    void change(contracts::BlockPos pos, BlockState before, const LightWorld& world);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace circuit::gameplay
