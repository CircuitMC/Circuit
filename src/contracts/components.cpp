// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/components.hpp>
#include <algorithm>

namespace circuit::contracts {

struct ComponentPatch::Data {
    std::int32_t protocol{};
    std::vector<Added> added;
    std::vector<std::uint16_t> removed;
    Typed typed;
    std::uint64_t hash{};
    std::size_t bytes{};
};

namespace {
constexpr std::uint64_t fnv_offset = 0xcbf29ce484222325ULL, fnv_prime = 0x100000001b3ULL;
void mix(std::uint64_t& h, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        h ^= (v >> (8 * i)) & 0xFF;
        h *= fnv_prime;
    }
}
const ComponentPatch::Typed no_typed{};
} // namespace

ComponentPatch ComponentPatch::make(std::int32_t protocol, std::vector<Added> added, std::vector<std::uint16_t> removed, Typed typed) {
    if (added.empty() && removed.empty()) return {};
    auto d = std::make_shared<Data>();
    d->protocol = protocol;
    std::uint64_t h = fnv_offset;
    mix(h, static_cast<std::uint32_t>(protocol), 4);
    mix(h, added.size(), 2);
    for (const auto& a : added) {
        mix(h, a.type, 2);
        mix(h, a.value.size(), 4);
        for (const auto b : a.value) mix(h, static_cast<std::uint8_t>(b), 1);
        d->bytes += a.value.size();
    }
    mix(h, removed.size(), 2);
    for (const auto r : removed) mix(h, r, 2);
    d->hash = h == 0 ? 1 : h;
    d->added = std::move(added);
    d->removed = std::move(removed);
    d->typed = typed;
    ComponentPatch p;
    p.data_ = std::move(d);
    return p;
}

std::int32_t ComponentPatch::protocol() const noexcept { return data_ ? data_->protocol : 0; }
std::span<const ComponentPatch::Added> ComponentPatch::added() const noexcept {
    return data_ ? std::span<const Added>(data_->added) : std::span<const Added>{};
}
std::span<const std::uint16_t> ComponentPatch::removed() const noexcept {
    return data_ ? std::span<const std::uint16_t>(data_->removed) : std::span<const std::uint16_t>{};
}
const ComponentPatch::Typed& ComponentPatch::typed() const noexcept { return data_ ? data_->typed : no_typed; }
std::uint64_t ComponentPatch::hash() const noexcept { return data_ ? data_->hash : 0; }
std::size_t ComponentPatch::value_bytes() const noexcept { return data_ ? data_->bytes : 0; }

bool ComponentPatch::operator==(const ComponentPatch& o) const noexcept {
    if (data_ == o.data_) return true;
    if (!data_ || !o.data_ || data_->hash != o.data_->hash) return false;
    return data_->protocol == o.data_->protocol && data_->added == o.data_->added && data_->removed == o.data_->removed;
}

std::strong_ordering ComponentPatch::operator<=>(const ComponentPatch& o) const noexcept {
    if (*this == o) return std::strong_ordering::equal;
    if (const auto c = hash() <=> o.hash(); c != 0) return c;
    // Equal hashes of different content: order by content.
    if (const auto c = data_->protocol <=> o.data_->protocol; c != 0) return c;
    const auto key = [](const Data& d) {
        std::vector<std::byte> k;
        for (const auto& a : d.added) {
            k.push_back(static_cast<std::byte>(a.type >> 8));
            k.push_back(static_cast<std::byte>(a.type));
            k.insert(k.end(), a.value.begin(), a.value.end());
        }
        k.push_back(std::byte{0xFF});
        for (const auto r : d.removed) {
            k.push_back(static_cast<std::byte>(r >> 8));
            k.push_back(static_cast<std::byte>(r));
        }
        return k;
    };
    const auto a = key(*data_), b = key(*o.data_);
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
}

Blob::Blob(std::vector<std::byte> bytes) {
    if (bytes.empty()) return;
    std::uint64_t h = fnv_offset;
    for (const auto b : bytes) mix(h, static_cast<std::uint8_t>(b), 1);
    hash_ = h;
    data_ = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
}
std::span<const std::byte> Blob::bytes() const noexcept { return data_ ? std::span<const std::byte>(*data_) : std::span<const std::byte>{}; }
bool Blob::operator==(const Blob& o) const noexcept {
    if (data_ == o.data_) return true;
    if (!data_ || !o.data_ || hash_ != o.hash_) return false;
    return *data_ == *o.data_;
}

namespace {
void put(std::vector<std::byte>& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
bool get(std::span<const std::byte>& in, std::uint64_t& v, int bytes) {
    if (in.size() < static_cast<std::size_t>(bytes)) return false;
    v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[static_cast<std::size_t>(i)])) << (8 * i);
    in = in.subspan(static_cast<std::size_t>(bytes));
    return true;
}
} // namespace

void write_patch(std::vector<std::byte>& out, const ComponentPatch& patch) {
    put(out, static_cast<std::uint32_t>(patch.protocol()), 4);
    if (patch.empty()) return;
    put(out, patch.added().size(), 2);
    for (const auto& a : patch.added()) {
        put(out, a.type, 2);
        put(out, a.value.size(), 4);
        out.insert(out.end(), a.value.begin(), a.value.end());
    }
    put(out, patch.removed().size(), 2);
    for (const auto r : patch.removed()) put(out, r, 2);
    const auto& t = patch.typed();
    put(out, (t.max_stack_size ? 1u : 0u) | (t.max_damage ? 2u : 0u), 1);
    put(out, t.max_stack_size.value_or(0), 1);
    put(out, static_cast<std::uint32_t>(t.max_damage.value_or(0)), 4);
}

std::optional<ComponentPatch> read_patch(std::span<const std::byte>& in, std::size_t max_bytes) {
    std::uint64_t protocol{}, count{}, v{};
    if (!get(in, protocol, 4)) return std::nullopt;
    if (protocol == 0) return ComponentPatch{};
    if (protocol > 0x7FFFFFFF || !get(in, count, 2)) return std::nullopt;
    std::vector<ComponentPatch::Added> added(count);
    std::size_t total = 0;
    for (std::size_t i = 0; i < added.size(); ++i) {
        std::uint64_t type{}, length{};
        if (!get(in, type, 2) || !get(in, length, 4)) return std::nullopt;
        if ((i > 0 && type <= added[i - 1].type) || length > in.size() || (total += length) > max_bytes) return std::nullopt;
        added[i].type = static_cast<std::uint16_t>(type);
        added[i].value.assign(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(length));
        in = in.subspan(length);
    }
    if (!get(in, count, 2)) return std::nullopt;
    std::vector<std::uint16_t> removed(count);
    for (std::size_t i = 0; i < removed.size(); ++i) {
        if (!get(in, v, 2) || (i > 0 && v <= removed[i - 1])) return std::nullopt;
        removed[i] = static_cast<std::uint16_t>(v);
    }
    std::uint64_t flags{}, stack{}, damage{};
    if (!get(in, flags, 1) || !get(in, stack, 1) || !get(in, damage, 4) || flags > 3) return std::nullopt;
    if (added.empty() && removed.empty()) return std::nullopt; // an empty patch has no protocol
    ComponentPatch::Typed typed;
    if (flags & 1) typed.max_stack_size = static_cast<std::uint8_t>(stack);
    if (flags & 2) typed.max_damage = static_cast<std::int32_t>(static_cast<std::uint32_t>(damage));
    return ComponentPatch::make(static_cast<std::int32_t>(protocol), std::move(added), std::move(removed), typed);
}

} // namespace circuit::contracts
