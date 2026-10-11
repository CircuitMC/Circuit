// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/trace.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/light.hpp>
#include <algorithm>
#include <deque>
#include <stdexcept>
#include <unordered_map>

namespace circuit::gameplay {
using contracts::BlockPos;
using contracts::ChunkPos;
using contracts::Direction;

namespace {
LightInfo registry_light(std::uint32_t state) noexcept {
    return {registry::light_emission(state), registry::light_dampening(state), registry::light_shape(state)};
}

// Modeled states through the registry states they stand for. Their light
// depends only on the low 16 bits (type, facing, level, flag): wire sides and
// diode delays do not change it, which building the table checks.
struct ModeledLight {
    std::vector<LightInfo> by_bits = std::vector<LightInfo>(1u << 16, LightInfo{0, 15, 0});
};
const ModeledLight& modeled_light() {
    static const auto table = [] {
        ModeledLight out;
        std::vector<bool> seen(1u << 16);
        for_each_modeled_state([&](BlockState s) {
            const auto state = registry_equivalent(s);
            const auto info = state ? registry_light(*state) : LightInfo{0, 15, 0};
            const auto bits = s.raw() & 0xFFFF;
            if (seen[bits] && !(out.by_bits[bits] == info)) throw std::logic_error("light: modeled states differ beyond their low bits");
            seen[bits] = true;
            out.by_bits[bits] = info;
        });
        return out;
    }();
    return table;
}

constexpr std::array<Direction, 6> directions{Direction::down, Direction::up, Direction::north,
                                              Direction::south, Direction::west, Direction::east};
} // namespace

LightInfo light_info(BlockState s) noexcept {
    if (s.is(BlockType::other)) return registry_light(s.registry_state());
    if (s.is_unloaded()) return {0, 15, 0};
    return modeled_light().by_bits[s.raw() & 0xFFFF];
}

std::uint8_t light_passed(LightKind kind, std::uint8_t level, const LightInfo& from, const LightInfo& to, Direction side) noexcept {
    if (level == 0) return 0;
    if ((from.shape != 0 || to.shape != 0) && registry::light_occluded(from.shape, to.shape, side)) return 0;
    if (kind == LightKind::sky && side == Direction::down && level == 15 && to.dampening == 0) return 15;
    const int cost = std::max<int>(1, to.dampening);
    return static_cast<std::uint8_t>(std::max(0, int{level} - cost));
}

struct LightField::Impl {
    struct Section {
        std::uint8_t uniform[2]{0, 0};
        std::unique_ptr<std::array<std::uint8_t, 4096>> cells[2];
        [[nodiscard]] std::uint8_t get(LightKind k, std::size_t i) const noexcept {
            const auto n = static_cast<std::size_t>(k);
            return cells[n] ? (*cells[n])[i] : uniform[n];
        }
        void set(LightKind k, std::size_t i, std::uint8_t v) {
            const auto n = static_cast<std::size_t>(k);
            if (!cells[n]) {
                if (v == uniform[n]) return;
                cells[n] = std::make_unique<std::array<std::uint8_t, 4096>>();
                cells[n]->fill(uniform[n]);
            }
            (*cells[n])[i] = v;
        }
    };
    struct Column {
        std::vector<Section> sections;
    };

    std::int32_t min_y;
    std::int32_t height;
    std::unordered_map<std::uint64_t, Column> columns;

    static std::uint64_t key(ChunkPos c) noexcept { return contracts::pack_chunk(c); }
    [[nodiscard]] std::int32_t top() const noexcept { return min_y + height - 1; }
    [[nodiscard]] bool in_height(std::int32_t y) const noexcept { return y >= min_y && y <= top(); }
    [[nodiscard]] std::size_t index(BlockPos p) const noexcept {
        return static_cast<std::size_t>(((p.y - min_y) & 15) << 8 | (p.z & 15) << 4 | (p.x & 15));
    }

    // The last column looked up (columns are not added or removed while it is used).
    mutable std::uint64_t cached_key{};
    mutable Column* cached{};

    Column* column(ChunkPos c) const noexcept {
        const auto k = key(c);
        if (cached && cached_key == k) return cached;
        const auto it = const_cast<Impl*>(this)->columns.find(k);
        if (it == columns.end()) return nullptr;
        cached_key = k;
        cached = &it->second;
        return cached;
    }
    Section* section(BlockPos p) noexcept {
        if (!in_height(p.y)) return nullptr;
        auto* c = column(contracts::chunk_of(p));
        return c ? &c->sections[static_cast<std::size_t>((p.y - min_y) >> 4)] : nullptr;
    }
    const Section* section(BlockPos p) const noexcept { return const_cast<Impl*>(this)->section(p); }

    [[nodiscard]] std::uint8_t get(LightKind k, BlockPos p) const noexcept {
        const auto* s = section(p);
        return s ? s->get(k, index(p)) : 0;
    }
    void set(LightKind k, BlockPos p, std::uint8_t v) {
        if (auto* s = section(p)) s->set(k, index(p), v);
    }
    // Whether the section of `chunk` at height `y` and the same sections of
    // its four neighbors hold one level each (or are absent), each within
    // one level of the column's: nothing passes between them.
    bool border_uniform(ChunkPos chunk, std::int32_t y) const {
        const auto s = static_cast<std::size_t>((y - min_y) >> 4);
        const auto mine = columns.find(key(chunk));
        if (mine == columns.end()) return false;
        const auto& own = mine->second.sections[s];
        if (own.cells[0] || own.cells[1]) return false;
        for (const auto& [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            const auto it = columns.find(key({chunk.x + dx, chunk.z + dz}));
            if (it == columns.end()) continue;
            const auto& other = it->second.sections[s];
            if (other.cells[0] || other.cells[1]) return false;
            for (std::size_t n = 0; n < 2; ++n)
                if (std::abs(int{own.uniform[n]} - int{other.uniform[n]}) > 1) return false;
        }
        return true;
    }

    // A cell whose light the field follows: inside the height, in a column of
    // the field (whose columns the world always reports loaded: they leave
    // the field before anything else is followed).
    bool live(BlockPos p, const LightWorld&) const {
        return in_height(p.y) && column(contracts::chunk_of(p)) != nullptr;
    }

    // The level a cell takes from what is not a neighbor: its emission (block
    // light) or the sky above the world.
    std::uint8_t own(LightKind k, BlockPos p, const LightInfo& info) const {
        if (k == LightKind::block) return info.emission;
        if (p.y == top()) return light_passed(k, 15, LightInfo{}, info, Direction::down);
        return 0;
    }

    // Work lists, kept between calls.
    std::array<std::vector<BlockPos>, 16> buckets;
    std::deque<std::pair<BlockPos, std::uint8_t>> queue;

    void increase(LightKind k, const std::vector<BlockPos>& seeds, const LightWorld& world) {
        for (const auto p : seeds) {
            if (!live(p, world)) continue;
            const auto info = light_info(world.block(p));
            const auto mine = own(k, p, info);
            auto level = get(k, p);
            if (mine > level) set(k, p, level = mine);
            if (level > 0) buckets[level].push_back(p);
        }
        for (int level = 15; level > 0; --level) {
            auto& bucket = buckets[static_cast<std::size_t>(level)];
            while (!bucket.empty()) {
                const auto q = bucket.back();
                bucket.pop_back();
                if (get(k, q) != level) continue; // raised since
                const auto from = light_info(world.block(q));
                for (const auto d : directions) {
                    const auto n = q.relative(d);
                    if (!live(n, world)) continue;
                    const auto give = light_passed(k, static_cast<std::uint8_t>(level), from, light_info(world.block(n)), d);
                    if (give > get(k, n)) {
                        set(k, n, give);
                        buckets[give].push_back(n);
                    }
                }
            }
        }
    }

    // Clears the light that may have come from the starting cells (at their
    // given levels) and returns the cells to light again from.
    std::vector<BlockPos> decrease(LightKind k, const std::vector<std::pair<BlockPos, std::uint8_t>>& starts, const LightWorld& world) {
        queue.clear();
        std::vector<BlockPos> reseed;
        for (const auto& [p, level] : starts) {
            if (!live(p, world)) continue;
            set(k, p, 0);
            queue.emplace_back(p, level);
        }
        while (!queue.empty()) {
            const auto [q, level] = queue.front();
            queue.pop_front();
            const auto from = light_info(world.block(q));
            if (own(k, q, from) > 0) reseed.push_back(q);
            for (const auto d : directions) {
                const auto n = q.relative(d);
                if (!live(n, world)) continue;
                const auto current = get(k, n);
                if (current == 0) continue;
                const auto give = light_passed(k, level, from, light_info(world.block(n)), d);
                if (give != 0 && current == give) {
                    set(k, n, 0);
                    queue.emplace_back(n, current);
                } else {
                    reseed.push_back(n);
                }
            }
        }
        return reseed;
    }
};

LightField::LightField(std::int32_t min_y, std::int32_t height) : impl_(std::make_unique<Impl>()) {
    impl_->min_y = min_y;
    impl_->height = height;
}
LightField::LightField(LightField&&) noexcept = default;
LightField& LightField::operator=(LightField&&) noexcept = default;
LightField::~LightField() = default;

std::uint8_t LightField::get(LightKind kind, BlockPos pos) const noexcept { return impl_->get(kind, pos); }
bool LightField::has(ChunkPos chunk) const noexcept { return impl_->columns.contains(Impl::key(chunk)); }
std::vector<ChunkPos> LightField::columns() const {
    std::vector<ChunkPos> out;
    for (const auto& [k, c] : impl_->columns)
        out.push_back({static_cast<std::int32_t>(static_cast<std::uint32_t>(k >> 32)), static_cast<std::int32_t>(static_cast<std::uint32_t>(k))});
    std::ranges::sort(out);
    return out;
}

void LightField::load(ChunkPos chunk, const LightWorld& world) {
    auto& m = *impl_;
    if (m.columns.contains(Impl::key(chunk))) return;
    m.cached = nullptr;
    auto& column = m.columns[Impl::key(chunk)];
    column.sections.resize(static_cast<std::size_t>(m.height / 16));
    const auto x0 = chunk.x * 16, z0 = chunk.z * 16;
    std::vector<BlockPos> block_seeds, sky_seeds;
    // Sky light straight down from above the world, section by section, and
    // the lowest cell each column of cells keeps it to.
    std::array<std::int32_t, 256> lowest;
    lowest.fill(m.top() + 1);
    std::array<LightInfo, 256> above{};
    std::array<bool, 256> open;
    open.fill(true);
    for (auto s = static_cast<std::int32_t>(column.sections.size()) - 1; s >= 0; --s) {
        const auto base = m.min_y + s * 16;
        const bool all_open = std::ranges::all_of(open, [](bool o) { return o; })
                           && std::ranges::all_of(above, [](const LightInfo& i) { return i.shape == 0; });
        if (all_open && world.empty(chunk, s)) {
            column.sections[static_cast<std::size_t>(s)].uniform[static_cast<std::size_t>(LightKind::sky)] = 15;
            lowest.fill(base);
            above.fill(LightInfo{});
            continue;
        }
        for (auto y = base + 15; y >= base; --y)
            for (std::size_t i = 0; i < 256; ++i) {
                if (!open[i]) continue;
                const BlockPos p{x0 + static_cast<std::int32_t>(i & 15), y, z0 + static_cast<std::int32_t>(i >> 4)};
                const auto info = light_info(world.block(p));
                if (light_passed(LightKind::sky, 15, above[i], info, Direction::down) != 15) {
                    open[i] = false;
                    continue;
                }
                m.set(LightKind::sky, p, 15);
                above[i] = info;
                lowest[i] = y;
            }
    }
    for (std::size_t i = 0; i < 256; ++i)
        if (lowest[i] <= m.top()) sky_seeds.push_back({x0 + static_cast<std::int32_t>(i & 15), lowest[i], z0 + static_cast<std::int32_t>(i >> 4)});
    // Sky light spreads sideways from where one column of cells keeps it lower
    // than its neighbor.
    for (int i = 0; i < 256; ++i) {
        const int x = i & 15, z = i >> 4;
        auto reach = lowest[static_cast<std::size_t>(i)];
        for (const auto& [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            if (x + dx < 0 || x + dx > 15 || z + dz < 0 || z + dz > 15) continue;
            reach = std::max(reach, lowest[static_cast<std::size_t>((z + dz) * 16 + x + dx)]);
        }
        for (auto y = lowest[static_cast<std::size_t>(i)]; y < reach; ++y) sky_seeds.push_back({x0 + x, y, z0 + z});
    }
    // Emitters.
    for (std::int32_t s = 0; s < static_cast<std::int32_t>(column.sections.size()); ++s) {
        if (world.empty(chunk, s)) continue;
        for (auto y = m.min_y + s * 16; y < m.min_y + s * 16 + 16; ++y)
            for (int i = 0; i < 256; ++i) {
                const BlockPos p{x0 + (i & 15), y, z0 + (i >> 4)};
                if (light_info(world.block(p)).emission > 0) block_seeds.push_back(p);
            }
    }
    // Both sides of the column's borders exchange light, except where both
    // sides' sections hold one level each, within one of the other.
    for (std::int32_t y = m.min_y; y <= m.top(); ++y)
        for (int k = 0; k < 16; ++k) {
            if (k == 0 && (y - m.min_y) % 16 == 0 && m.border_uniform(chunk, y)) {
                y += 15;
                break;
            }
            for (const auto& [inside, outside] : {std::pair<BlockPos, BlockPos>{{x0, y, z0 + k}, {x0 - 1, y, z0 + k}},
                                                  {{x0 + 15, y, z0 + k}, {x0 + 16, y, z0 + k}},
                                                  {{x0 + k, y, z0}, {x0 + k, y, z0 - 1}},
                                                  {{x0 + k, y, z0 + 15}, {x0 + k, y, z0 + 16}}}) {
                for (const auto kind : {LightKind::block, LightKind::sky}) {
                    // Only a side that could raise the other one by passing one level less.
                    auto& seeds = kind == LightKind::block ? block_seeds : sky_seeds;
                    const auto in = m.get(kind, inside), out = m.get(kind, outside);
                    if (in > out + 1) seeds.push_back(inside);
                    if (out > in + 1) seeds.push_back(outside);
                }
            }
        }
    m.increase(LightKind::block, block_seeds, world);
    m.increase(LightKind::sky, sky_seeds, world);
    // Sections whose cells all have one level keep only that level.
    for (auto& section : column.sections)
        for (std::size_t n = 0; n < 2; ++n)
            if (section.cells[n] && std::ranges::all_of(*section.cells[n], [&](std::uint8_t v) { return v == (*section.cells[n])[0]; })) {
                section.uniform[n] = (*section.cells[n])[0];
                section.cells[n].reset();
            }
}

void LightField::unload(ChunkPos chunk, const LightWorld& world) {
    auto& m = *impl_;
    const auto it = m.columns.find(Impl::key(chunk));
    if (it == m.columns.end()) return;
    // Light the column gave its neighbors (at most one level less than its
    // own border cells) may have come from it; it is cleared and lit again.
    const auto x0 = chunk.x * 16, z0 = chunk.z * 16;
    std::vector<std::pair<BlockPos, std::uint8_t>> starts[2];
    // Per section of the column and side: a neighbor section that holds one
    // level at least as high as the column's single level took nothing from it.
    const auto took_nothing = [&](std::size_t s, int dx, int dz) {
        const auto& own = it->second.sections[s];
        const auto other = m.columns.find(Impl::key({chunk.x + dx, chunk.z + dz}));
        if (other == m.columns.end()) return true;
        const auto& theirs = other->second.sections[s];
        if (own.cells[0] || own.cells[1] || theirs.cells[0] || theirs.cells[1]) return false;
        return theirs.uniform[0] >= own.uniform[0] && theirs.uniform[1] >= own.uniform[1];
    };
    for (std::int32_t y = m.min_y; y <= m.top(); ++y) {
        const auto s = static_cast<std::size_t>((y - m.min_y) >> 4);
        const bool skip[4]{took_nothing(s, -1, 0), took_nothing(s, 1, 0), took_nothing(s, 0, -1), took_nothing(s, 0, 1)};
        for (int k = 0; k < 16; ++k) {
            const std::pair<BlockPos, BlockPos> sides[4]{{{x0, y, z0 + k}, {x0 - 1, y, z0 + k}},
                                                         {{x0 + 15, y, z0 + k}, {x0 + 16, y, z0 + k}},
                                                         {{x0 + k, y, z0}, {x0 + k, y, z0 - 1}},
                                                         {{x0 + k, y, z0 + 15}, {x0 + k, y, z0 + 16}}};
            for (int side = 0; side < 4; ++side) {
                if (skip[side]) continue;
                const auto& [inside, outside] = sides[side];
                for (const auto kind : {LightKind::block, LightKind::sky}) {
                    const auto mine = m.get(kind, inside), theirs = m.get(kind, outside);
                    if (theirs > 0 && theirs < mine) starts[static_cast<std::size_t>(kind)].emplace_back(outside, theirs);
                }
            }
        }
    }
    m.cached = nullptr;
    m.columns.erase(it);
    for (const auto kind : {LightKind::block, LightKind::sky}) {
        const auto reseed = m.decrease(kind, starts[static_cast<std::size_t>(kind)], world);
        m.increase(kind, reseed, world);
    }
}

void LightField::change(BlockPos pos, BlockState before, const LightWorld& world) {
    auto& m = *impl_;
    if (!m.live(pos, world)) return;
    const auto after = world.block(pos);
    if (light_info(before) == light_info(after)) return;
    // The world as it was, with the cell before the change.
    struct Before final : LightWorld {
        const LightWorld& base;
        BlockPos pos;
        BlockState state;
        Before(const LightWorld& b, BlockPos p, BlockState s) : base(b), pos(p), state(s) {}
        BlockState block(BlockPos p) const override { return p == pos ? state : base.block(p); }
        bool loaded(ChunkPos c) const override { return base.loaded(c); }
    } old(world, pos, before);
    const auto was = light_info(before), now = light_info(after);
    for (const auto kind : {LightKind::block, LightKind::sky}) {
        // Sky light does not depend on emission; a cell that only emits more
        // only raises light.
        const bool passes_same = was.dampening == now.dampening && was.shape == now.shape;
        if (kind == LightKind::sky && passes_same) continue;
        if (kind == LightKind::block && passes_same && now.emission > was.emission) {
            m.increase(kind, {pos}, world);
            continue;
        }
        auto reseed = m.decrease(kind, {{pos, m.get(kind, pos)}}, old);
        reseed.push_back(pos);
        for (const auto d : directions) reseed.push_back(pos.relative(d));
        m.increase(kind, reseed, world);
    }
}

} // namespace circuit::gameplay
