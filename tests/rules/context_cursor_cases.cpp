// SPDX-License-Identifier: AGPL-3.0-only
// RuleContext's section cursor: block() reads the section the executor
// pointed it at without a virtual call, and every other read goes through
// load_block. A small executor over paletted sections checks the reads at
// several bit widths, the misses, writes in place and writes that grow the
// palette, a level change and sections at negative coordinates.
#include "../support/check.hpp"
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/context.hpp>
#include <circuit/gameplay/rules.hpp>
#include <format>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
using namespace circuit;
using namespace circuit::gameplay;
using circuit::test::require;
using contracts::BlockPos;

// Paletted sections keyed by level and section coordinates.
struct Section {
    std::vector<BlockState> palette;
    std::vector<std::uint64_t> words;
    std::uint8_t bits{};

    std::uint32_t value(std::uint32_t i) const {
        if (bits == 0) return 0;
        const auto per = 64 / bits;
        return static_cast<std::uint32_t>((words[i / per] >> ((i % per) * bits)) & ((std::uint64_t{1} << bits) - 1));
    }
    void set_value(std::uint32_t i, std::uint32_t v) {
        const auto per = 64 / bits;
        const auto shift = (i % per) * bits;
        const auto mask = ((std::uint64_t{1} << bits) - 1) << shift;
        words[i / per] = (words[i / per] & ~mask) | (static_cast<std::uint64_t>(v) << shift);
    }
    // Every value repacked at a new width.
    void repack(std::uint8_t to) {
        std::vector<std::uint32_t> values(4096);
        for (std::uint32_t i = 0; i < 4096; ++i) values[i] = value(i);
        bits = to;
        words.assign((4096 + 64 / to - 1) / (64 / to), 0);
        for (std::uint32_t i = 0; i < 4096; ++i) set_value(i, values[i]);
    }
    SectionView view() const { return SectionView::make(palette.data(), words.data(), bits); }
};

using SectionKey = std::tuple<int, std::int32_t, std::int32_t, std::int32_t>;

class CursorContext final : public RuleContext {
public:
    explicit CursorContext(const contracts::VersionBundle& bundle) : RuleContext(bundle) {}

    std::map<SectionKey, Section> sections;
    std::size_t misses{};

    SectionKey key_of(BlockPos p) const { return {static_cast<int>(dimension()), p.x >> 4, p.y >> 4, p.z >> 4}; }
    Section& section_at(BlockPos p) { return sections.at(key_of(p)); }
    void use_level(contracts::DimensionId level) { enter(level); }
    bool holds(BlockPos p) const { return cursor_holds(p); }

    BlockState load_block(BlockPos pos) override {
        ++misses;
        const auto it = sections.find(key_of(pos));
        if (it == sections.end()) return BlockState::unloaded_sentinel();
        point_cursor(pos, it->second.view());
        return it->second.palette[it->second.value(contracts::section_local_index(pos))];
    }
    // Writes in place when the state is in the palette; otherwise the
    // palette grows (it may move) and the values may be repacked wider, and
    // the cursor is dropped if it holds this section.
    void write_block(BlockPos pos, BlockState state) override {
        auto& s = section_at(pos);
        const auto index = contracts::section_local_index(pos);
        for (std::uint32_t v = 0; v < s.palette.size(); ++v)
            if (s.palette[v] == state) {
                if (s.bits == 0) break; // one-state section: grows below
                s.set_value(index, v);
                return;
            }
        if (cursor_holds(pos)) drop_cursor();
        s.palette.push_back(state);
        std::uint8_t need = 1;
        while ((std::size_t{1} << need) < s.palette.size()) ++need;
        if (s.bits == 0) {
            s.bits = need;
            s.words.assign((4096 + 64 / need - 1) / (64 / need), 0);
        } else if (need > s.bits) {
            s.repack(need);
        }
        std::uint32_t v = 0;
        while (!(s.palette[v] == state)) ++v;
        s.set_value(index, v);
    }

    [[noreturn]] static void unused() { throw std::logic_error("not used by the cursor cases"); }
    Tick now() const override { return 0; }
    std::int32_t game_rule(std::string_view) override { unused(); }
    std::uint8_t light(BlockPos) override { unused(); }
    std::uint8_t sky_light(BlockPos) override { unused(); }
    std::uint8_t block_light(BlockPos) override { unused(); }
    bool player_near(ChunkPos) override { unused(); }
    std::optional<Presence> nearest_player(contracts::Vec3, double) override { unused(); }
    std::optional<Presence> presence_of(contracts::PlayerKey) override { unused(); }
    bool final_ids() const noexcept override { return true; }
    bool section_ticks_randomly(ChunkPos, std::int32_t) override { unused(); }
    std::uint8_t difficulty() override { unused(); }
    Sky sky() override { unused(); }
    std::optional<contracts::Generation> chunk_generation(ChunkPos) override { unused(); }
    bool has_tick(TickList, BlockPos, BlockType) override { unused(); }
    void add_tick(TickList, BlockPos, BlockType, Tick, std::int8_t, std::uint32_t) override { unused(); }
    bool has_block_event(const BlockEvent&) override { unused(); }
    std::size_t torch_turn_offs(BlockPos, Tick, bool) override { unused(); }
    void add_block_event(const BlockEvent&, std::uint32_t) override { unused(); }
    const BlockEntity* peek_block_entity(BlockPos) override { unused(); }
    BlockEntity* block_entity(BlockPos) override { unused(); }
    void create_block_entity(BlockPos, BlockType) override { unused(); }
    void remove_block_entity(BlockPos) override { unused(); }
    std::vector<EntityId> entities_in_cell(BlockPos) override { unused(); }
    std::vector<EntityId> entities_in_cells(BlockPos, BlockPos) override { unused(); }
    const Entity* peek_entity(EntityId) override { unused(); }
    Entity* entity(EntityId) override { unused(); }
    EntityId spawn_entity(const Entity&) override { unused(); }
    void relocate_entity(EntityId, Vec3) override { unused(); }
    void remove_entity(EntityId) override { unused(); }
    const PlayerInventory* peek_player(contracts::PlayerKey) override { unused(); }
    PlayerInventory& player(contracts::PlayerKey) override { unused(); }
    const MapData* peek_map(std::int32_t) override { unused(); }
    MapData* map(std::int32_t) override { unused(); }
    std::vector<std::int32_t> map_ids() override { unused(); }
    std::int32_t add_map(MapData) override { unused(); }
    std::int32_t last_map_id() override { unused(); }
    std::vector<Presence> presences() override { unused(); }
    std::optional<std::int32_t> column_top(std::int32_t, std::int32_t) override { unused(); }
    std::uint32_t record(TraceKind, BlockPos, std::uint64_t, std::uint64_t, std::uint64_t) override { return 0; }
    RandomSequence& random_sequence(const std::string&, const SequenceIdHash&) override { unused(); }

protected:
    LevelRandom& level_random() override { unused(); }
    std::uint32_t& random_tick_state() override { unused(); }
};

const contracts::VersionBundle& bundle() { return **contracts::BundleRegistry::select_for_simulation("java-1.21.8"); }

// States to fill sections with: distinct and valid.
std::vector<BlockState> states(std::size_t n) {
    const std::vector<BlockState> base{blocks::stone(), blocks::glass(), blocks::obsidian(), blocks::redstone_block(),
                                       blocks::slime(), blocks::honey()};
    std::vector<BlockState> out;
    for (std::size_t i = 0; i < n; ++i) {
        if (i < base.size()) out.push_back(base[i]);
        else out.push_back(blocks::wire(static_cast<std::uint8_t>(i % 16)));
    }
    return out;
}

// A section whose block i is palette[(i * 7 + seed) % palette.size()].
Section filled(std::uint8_t bits, std::size_t palette_size, std::uint32_t seed) {
    Section s;
    s.palette = states(palette_size);
    s.bits = bits;
    if (bits > 0) {
        s.words.assign((4096 + 64 / bits - 1) / (64 / bits), 0);
        for (std::uint32_t i = 0; i < 4096; ++i) s.set_value(i, static_cast<std::uint32_t>((i * 7 + seed) % palette_size));
    }
    return s;
}
BlockState expected(const Section& s, std::uint32_t i) { return s.palette[s.value(i)]; }

BlockPos at(std::int32_t sx, std::int32_t sy, std::int32_t sz, std::uint32_t i) {
    return {sx * 16 + static_cast<std::int32_t>(i & 15), sy * 16 + static_cast<std::int32_t>(i >> 8), sz * 16 + static_cast<std::int32_t>((i >> 4) & 15)};
}
} // namespace

CIRCUIT_TEST("the cursor reads every block of a section at every width, with one miss") {
    // Widths 1 to 16, and 5, 6 and 7, whose values do not fill a word.
    for (const auto& [bits, size] : {std::pair{1, 2}, std::pair{4, 16}, std::pair{5, 17}, std::pair{6, 40}, std::pair{7, 70},
                                    std::pair{8, 200}, std::pair{15, 300}, std::pair{16, 600}}) {
        CursorContext ctx(bundle());
        const auto s = filled(static_cast<std::uint8_t>(bits), static_cast<std::size_t>(size), 3);
        ctx.sections[{0, 2, -4, 5}] = s;
        for (std::uint32_t i = 0; i < 4096; ++i) {
            const auto pos = at(2, -4, 5, i);
            require(ctx.block(pos) == expected(s, i), std::format("{} bits: block {}", bits, i));
        }
        require(ctx.misses == 1, std::format("{} bits: {} misses for one section", bits, ctx.misses));
    }
}

CIRCUIT_TEST("a one-state section reads through the cursor") {
    CursorContext ctx(bundle());
    Section s;
    s.palette = {blocks::stone()};
    ctx.sections[{0, 0, 0, 0}] = s;
    for (std::uint32_t i = 0; i < 4096; i += 97) require(ctx.block(at(0, 0, 0, i)) == blocks::stone(), "stone");
    require(ctx.misses == 1, "one miss");
}

CIRCUIT_TEST("reads in another section, column or unloaded place go to load_block") {
    CursorContext ctx(bundle());
    ctx.sections[{0, 0, 0, 0}] = filled(4, 9, 1);
    ctx.sections[{0, 0, 1, 0}] = filled(4, 9, 2); // above
    ctx.sections[{0, 1, 0, 0}] = filled(4, 9, 5); // next column
    (void)ctx.block({1, 2, 3});
    require(ctx.misses == 1 && ctx.holds({15, 15, 15}) && !ctx.holds({16, 15, 15}), "the cursor holds the first section");
    require(ctx.block({1, 18, 3}) == expected(ctx.sections[{0, 0, 1, 0}], contracts::section_local_index({1, 18, 3})) && ctx.misses == 2,
            "the section above misses");
    require(ctx.block({17, 2, 3}) == expected(ctx.sections[{0, 1, 0, 0}], contracts::section_local_index({17, 2, 3})) && ctx.misses == 3,
            "the next column misses");
    require(ctx.block({18, 2, 3}) == expected(ctx.sections[{0, 1, 0, 0}], contracts::section_local_index({18, 2, 3})) && ctx.misses == 3,
            "then holds");
    require(ctx.block({40, 2, 3}).is_unloaded() && ctx.misses == 4, "an unloaded place reads as the sentinel through load_block");
    require(ctx.block({18, 2, 3}) == expected(ctx.sections[{0, 1, 0, 0}], contracts::section_local_index({18, 2, 3})) && ctx.misses == 4,
            "a miss on an unloaded place leaves the cursor where it was");
}

CIRCUIT_TEST("sections at negative coordinates are told apart") {
    CursorContext ctx(bundle());
    ctx.sections[{0, -1, -1, -1}] = filled(4, 11, 4);
    ctx.sections[{0, 0, 0, 0}] = filled(4, 11, 6);
    require(ctx.block({-1, -1, -1}) == expected(ctx.sections[{0, -1, -1, -1}], contracts::section_local_index({-1, -1, -1})), "(-1,-1,-1)");
    require(ctx.block({-16, -16, -16}) == expected(ctx.sections[{0, -1, -1, -1}], contracts::section_local_index({-16, -16, -16})) && ctx.misses == 1,
            "the same section");
    require(ctx.block({0, 0, 0}) == expected(ctx.sections[{0, 0, 0, 0}], 0) && ctx.misses == 2, "(0,0,0) is another section");
    require(ctx.block({-17, 0, 0}).is_unloaded() && ctx.misses == 3, "(-17,0,0) is not loaded");
}

CIRCUIT_TEST("a write in place keeps the cursor and is read back through it") {
    CursorContext ctx(bundle());
    ctx.sections[{0, 0, 0, 0}] = filled(4, 9, 1);
    const auto& palette = ctx.sections[{0, 0, 0, 0}].palette;
    (void)ctx.block({0, 0, 0});
    for (std::uint32_t i = 0; i < 4096; i += 13) {
        const auto state = palette[i % palette.size()];
        ctx.write_block(at(0, 0, 0, i), state);
        require(ctx.block(at(0, 0, 0, i)) == state, std::format("block {} written in place", i));
    }
    require(ctx.misses == 1, "no write dropped the cursor");
}

CIRCUIT_TEST("a write that grows the palette drops the cursor and the next read is right") {
    CursorContext ctx(bundle());
    Section s;
    s.palette = {blocks::air()};
    ctx.sections[{0, 0, 0, 0}] = s;
    (void)ctx.block({5, 5, 5});
    require(ctx.misses == 1, "pointed");
    // Each new state grows the palette; widths go 0 -> 1 -> 2 -> 3, and the
    // palette and words move.
    const auto fresh = states(7);
    for (std::size_t n = 0; n < fresh.size(); ++n) {
        const BlockPos pos{static_cast<std::int32_t>(n), 1, 2};
        const auto before = ctx.misses;
        ctx.write_block(pos, fresh[n]);
        require(!ctx.holds(pos), std::format("write {} dropped the cursor", n));
        require(ctx.block(pos) == fresh[n] && ctx.misses == before + 1, std::format("write {} is read back after one miss", n));
        for (std::size_t k = 0; k <= n; ++k)
            require(ctx.block({static_cast<std::int32_t>(k), 1, 2}) == fresh[k], std::format("write {} kept block {}", n, k));
        require(ctx.block({9, 9, 9}) == blocks::air(), "untouched blocks stay air");
    }
    require(ctx.sections[{0, 0, 0, 0}].bits == 3, "the section ended at 3 bits");
}

CIRCUIT_TEST("changing the level drops the cursor") {
    CursorContext ctx(bundle());
    ctx.sections[{0, 0, 0, 0}] = filled(4, 9, 1);
    ctx.sections[{static_cast<int>(contracts::DimensionId::the_nether), 0, 0, 0}] = filled(4, 9, 8);
    (void)ctx.block({1, 1, 1});
    require(ctx.holds({1, 1, 1}), "held in the overworld");
    ctx.use_level(contracts::DimensionId::the_nether);
    require(!ctx.holds({1, 1, 1}), "dropped by the level change");
    const auto nether = ctx.sections[{static_cast<int>(contracts::DimensionId::the_nether), 0, 0, 0}];
    require(ctx.block({1, 1, 1}) == expected(nether, contracts::section_local_index({1, 1, 1})) && ctx.misses == 2,
            "the nether's section is read, not the overworld's");
}
