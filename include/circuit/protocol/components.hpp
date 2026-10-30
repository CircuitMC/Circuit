// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/components.hpp>
#include <circuit/contracts/ids.hpp>
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/nbt.hpp>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Item data components on the network, for each supported protocol. The
// schema of every component type comes from a pinned public dataset (see
// tools/compile_components.py). Reading a component checks its structure and
// every registry id or enumeration value in it, and produces its canonical
// form: NBT compounds with keys in order and components by ascending type.
// Equal components therefore have equal bytes, whatever order a client sent.
namespace circuit::protocol {

// What a component may refer to besides the static registries the schema
// knows: the item and block registries and the registries the server
// synchronizes (enchantments, variants, damage types). A registry the
// callback does not know has no entries: an id into it is refused.
struct ComponentContext {
    std::uint32_t items{};
    std::uint32_t blocks{};
    std::function<std::optional<std::uint32_t>(std::string_view registry)> synced{};
    // Entry names of the synchronized registries ("minecraft:gold"), for
    // save forms that name entries instead of numbering them.
    std::function<std::optional<std::string_view>(std::string_view registry, std::uint32_t id)> name_of{};
    std::function<std::optional<std::uint32_t>(std::string_view registry, std::string_view name)> id_of{};
};

// A stack's damage and its other components, as read from the wire.
struct WirePatch {
    std::optional<std::int32_t> damage; // the damage component, kept apart
    contracts::ComponentPatch rest;
};

class ComponentCodec {
public:
    // nullptr for a protocol without a component schema.
    static const ComponentCodec* for_protocol(std::int32_t protocol) noexcept;

    [[nodiscard]] std::int32_t protocol() const noexcept;
    [[nodiscard]] std::size_t type_count() const noexcept;
    // "minecraft:"-less names, e.g. "custom_name"; empty for unknown ids.
    [[nodiscard]] std::string_view type_name(std::uint16_t type) const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> type_id(std::string_view name) const noexcept;

    // Reads the components of a stack after its item id: the added and
    // removed counts, the added components and the removed types. With
    // `length_prefixed`, each added value carries its length (1.21.5+
    // creative slots). Errors name the component and the reason.
    [[nodiscard]] std::expected<WirePatch, std::string> read(Reader& r, const ComponentContext& ctx, bool length_prefixed = false) const;
    // Writes them back, the damage among the others in type order.
    void write(Writer& w, std::optional<std::int32_t> damage, const contracts::ComponentPatch& patch) const;
    // Checks a patch from a record against the schema and the registries and
    // returns it in canonical form with its typed fields.
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> check(const contracts::ComponentPatch& patch,
                                                                              const ComponentContext& ctx) const;
    // Reads one component value of `type` from `value` alone (all bytes used)
    // and returns its canonical form.
    [[nodiscard]] std::expected<Bytes, std::string> canonical_value(std::uint16_t type, std::span<const std::byte> value,
                                                                    const ComponentContext& ctx) const;

    // The save form (NBT, as in region files) of a component value, and back,
    // for the components whose save form mirrors their network form (plain
    // values, text, NBT and lists of them) and for those that name registry
    // entries (trims, banner patterns, base colors; see components.cpp). The
    // others are errors that name them: Anvil interchange refuses them.
    [[nodiscard]] std::expected<nbt::Tag, std::string> to_save(std::uint16_t type, std::span<const std::byte> value,
                                                               const ComponentContext& ctx) const;
    [[nodiscard]] std::expected<Bytes, std::string> from_save(std::uint16_t type, const nbt::Tag& value,
                                                              const ComponentContext& ctx) const;

    // The level of `enchantment` (an id of the synchronized enchantment
    // registry) in the patch's enchantments component; 0 when absent. The
    // patch is canonical for this protocol.
    [[nodiscard]] long enchantment_level(const contracts::ComponentPatch& patch, std::uint32_t enchantment) const;
    // A patch with the enchantments component holding these (enchantment
    // id, level) pairs, canonical for this protocol (1.21.4's tooltip flag
    // shown).
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> enchanted(std::span<const std::pair<std::uint32_t, std::int32_t>> levels,
                                                                                  const ComponentContext& ctx) const;
    // The (enchantment id, level) pairs of the enchantments component, or of
    // stored_enchantments (`stored`), in their order; none when absent.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::int32_t>> enchantments_of(const contracts::ComponentPatch& patch, bool stored) const;
    // `base` with its enchantments (or stored_enchantments) holding these
    // pairs, the component left out when there are none unless `keep_empty`
    // (1.21.4's tooltip flag kept as it was), and its repair cost set (left
    // out at 0); its other components kept.
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_enchantments(const contracts::ComponentPatch& base, bool stored,
                                                                                          std::span<const std::pair<std::uint32_t, std::int32_t>> levels,
                                                                                          bool keep_empty, std::int32_t repair_cost,
                                                                                          const ComponentContext& ctx) const;
    // `base` with a layer of `pattern` (an id of the synchronized banner
    // pattern registry) in `color` (a dye color, 0-15) added on top of its
    // banner patterns; its other components kept.
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_banner_layer(const contracts::ComponentPatch& base, std::uint32_t pattern,
                                                                                          std::uint32_t color, const ComponentContext& ctx) const;
    // `base` (canonical for this protocol) with its trim component set to
    // `material` and `pattern` (ids of the synchronized trim registries;
    // 1.21.4's tooltip flag shown), its other components kept.
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_trim(const contracts::ComponentPatch& base, std::uint32_t material,
                                                                                  std::uint32_t pattern, const ComponentContext& ctx) const;

    // The potion (an id of the potion registry) of the patch's potion
    // contents; nullopt without one.
    [[nodiscard]] std::optional<std::uint32_t> potion_of(const contracts::ComponentPatch& patch) const;
    // `base` with the potion of its potion contents set to `potion`, their
    // color, effects and name and its other components kept.
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_potion(const contracts::ComponentPatch& base, std::uint32_t potion,
                                                                                    const ComponentContext& ctx) const;

    // A writable book's content of pages "1" to "n" (fixtures' books).
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> writable_book(std::int32_t pages, const ComponentContext& ctx) const;
    // A banner's pattern layers (0 without the component), a dyed color (none
    // without one), the patch with its dyed color set (1.21.4's tooltip flag
    // kept, else shown), and `base` with the component `name` as `from` has it
    // (removed when `from` has none).
    [[nodiscard]] std::int32_t banner_layers(const contracts::ComponentPatch& patch) const;
    [[nodiscard]] std::optional<std::int32_t> dyed_color_of(const contracts::ComponentPatch& patch) const;
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_dyed_color(const contracts::ComponentPatch& base, std::int32_t rgb,
                                                                                        const ComponentContext& ctx) const;
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_component_from(const contracts::ComponentPatch& base,
                                                                                            const contracts::ComponentPatch& from,
                                                                                            std::string_view name) const;
    // A firework star's components: its explosion (shape 0 small ball, 1
    // large ball, 2 star, 3 creeper, 4 burst; colors as RGB, no fade colors).
    // A star's `base` with its explosion's fade colors set (a small ball
    // without colors when it has no explosion), its other components kept.
    // And a rocket's components: fireworks flying `flight` with the
    // explosions of `stars` in order (those without one skipped).
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> firework_star(std::int32_t shape, std::span<const std::int32_t> colors,
                                                                                      bool trail, bool twinkle,
                                                                                      const ComponentContext& ctx) const;
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_fade_colors(const contracts::ComponentPatch& base,
                                                                                         std::span<const std::int32_t> fades,
                                                                                         const ComponentContext& ctx) const;
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> firework_rocket(std::int32_t flight,
                                                                                        std::span<const contracts::ComponentPatch> stars,
                                                                                        const ComponentContext& ctx) const;
    // The recipe ids of the patch's recipes component (a knowledge book's;
    // none without one).
    [[nodiscard]] std::optional<std::vector<std::string>> recipes_of(const contracts::ComponentPatch& patch) const;
    // The pages of the patch's writable or written book content (0 without one).
    [[nodiscard]] std::int32_t book_pages_of(const contracts::ComponentPatch& patch) const;
    // `base` with its writable book content holding `pages` (none filtered);
    // and a book and quill's `base` signed: its writable content gone, a
    // written book content added (the title, the author, generation 0, each
    // page as plain text, none filtered, resolved), its other components kept.
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_book_pages(const contracts::ComponentPatch& base,
                                                                                        std::span<const std::string> pages,
                                                                                        const ComponentContext& ctx) const;
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> signed_book(const contracts::ComponentPatch& base,
                                                                                    std::span<const std::string> pages, std::string_view title,
                                                                                    std::string_view author, const ComponentContext& ctx) const;
    // The stacks of a patch's container component (a shulker box item's
    // contents, slot by slot up to its last stack; none without one), their
    // protocol item ids mapped by `item_of`; and the patch with its container
    // holding `stacks` (trailing empty slots left out; all empty: no container
    // component), the items mapped by `wire_of`.
    [[nodiscard]] std::expected<std::vector<contracts::ItemStack>, std::string> container_of(
        const contracts::ComponentPatch& patch, const ComponentContext& ctx,
        const std::function<std::optional<std::uint16_t>(std::int32_t)>& item_of) const;
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_container(
        const contracts::ComponentPatch& base, std::span<const contracts::ItemStack> stacks, const ComponentContext& ctx,
        const std::function<std::optional<std::int32_t>(std::uint16_t)>& wire_of) const;
    // A component holding one VarInt (map_id, map_post_processing): its value
    // (nullopt when absent), and `base` with it set, or removed (nullopt).
    [[nodiscard]] std::optional<std::int32_t> varint_of(const contracts::ComponentPatch& patch, std::string_view component) const;
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_varint(const contracts::ComponentPatch& base,
                                                                                  std::string_view component,
                                                                                  std::optional<std::int32_t> value,
                                                                                  const ComponentContext& ctx) const;
    // The patch's repair cost (0 when absent), and its custom name's plain
    // text (nullopt without one).
    [[nodiscard]] std::int32_t repair_cost_of(const contracts::ComponentPatch& patch) const;
    [[nodiscard]] std::optional<std::string> custom_name_of(const contracts::ComponentPatch& patch) const;
    // `base` with its custom name set to plain text, or removed (nullopt).
    [[nodiscard]] std::expected<contracts::ComponentPatch, std::string> with_custom_name(const contracts::ComponentPatch& base,
                                                                                         const std::optional<std::string>& name,
                                                                                         const ComponentContext& ctx) const;

    explicit ComponentCodec(const void* schema) noexcept : schema_(schema) {}

private:
    const void* schema_;
};

} // namespace circuit::protocol
