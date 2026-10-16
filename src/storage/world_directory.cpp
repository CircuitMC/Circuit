// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <circuit/storage/world_directory.hpp>
#include <cstring>
#include <format>
#include <limits>
#include <set>
#include <sstream>
#include <zlib.h>

namespace circuit::storage {
namespace {
constexpr std::string_view manifest_file = "manifest";
// Log segments: "CIRCWAL" and a format byte. 8: spawned items may carry a
// pickup delay and an age (tag 22); 7 (read only): positions and velocities
// are doubles; 6 (read only): weather; 5 (read only): day time; 4 (read
// only): game rule changes; 3 (read only): typed tickets, presences; 2 (read
// only): commands name their level; 1 (read only): overworld commands, stacks
// with data components; 0 (read only): stacks without components. The record
// encoding of formats 2 to 6 is the same; a format 2 record holds no command
// of tags 16-21, a format 3 record none of tags 19-21, a format 4 record none
// of tags 20-21, a format 5 record none of tags 21-22, a format 6 or 7
// record none of tag 22. Formats 7 and 8 write positions and velocities as
// doubles instead of 1/4096 block units.
constexpr std::string_view segment_prefix{"CIRCWAL", 7};
constexpr char segment_format = 8;
constexpr std::size_t segment_magic_size = 8;
constexpr std::size_t max_record = std::size_t{64} << 20;

std::uint32_t crc32_of(std::span<const std::byte> data) {
    return static_cast<std::uint32_t>(::crc32(0L, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
}
void put32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void put64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
std::uint64_t get(std::span<const std::byte> in, std::size_t at, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[at + static_cast<std::size_t>(i)])) << (8 * i);
    return v;
}
std::span<const std::byte> bytes_of(std::string_view s) { return std::as_bytes(std::span(s.data(), s.size())); }

// "<prefix><tick:20>-<generation:10><suffix>"
struct FileName {
    std::uint64_t tick{}, generation{};
};
std::optional<FileName> parse_name(std::string_view name, std::string_view prefix, std::string_view suffix) {
    if (name.size() != prefix.size() + 31 + suffix.size() || !name.starts_with(prefix) || !name.ends_with(suffix)) return std::nullopt;
    const auto body = name.substr(prefix.size(), 31);
    if (body[20] != '-') return std::nullopt;
    FileName f;
    for (std::size_t i = 0; i < 31; ++i) {
        if (i == 20) continue;
        if (body[i] < '0' || body[i] > '9') return std::nullopt;
        auto& v = i < 20 ? f.tick : f.generation;
        if (v > (std::numeric_limits<std::uint64_t>::max() - 9) / 10) return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(body[i] - '0');
    }
    return f;
}
std::optional<FileName> parse_checkpoint(std::string_view name) { return parse_name(name, "ckpt-", ".bin"); }
std::optional<FileName> parse_segment(std::string_view name) { return parse_name(name, "wal-", ".log"); }
bool valid_sha(std::string_view s) {
    return s.size() == 64 && std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace

std::string checkpoint_name(std::uint64_t tick, std::uint64_t generation) {
    return std::format("ckpt-{:020}-{:010}.bin", tick, generation);
}
std::string segment_name(std::uint64_t tick, std::uint64_t generation) {
    return std::format("wal-{:020}-{:010}.log", tick, generation);
}

WorldDirectory::WorldDirectory(Filesystem& fs, Path root, std::string bundle_id, std::string rules_id)
    : fs_(fs), root_(std::move(root)), bundle_id_(std::move(bundle_id)), rules_id_(std::move(rules_id)) {}

Status WorldDirectory::acquire() {
    if (lock_) return {};
    if (auto ok = fs_.create_directories(root_); !ok) return ok;
    auto held = fs_.lock(root_ / lock_file_name);
    if (!held) return std::unexpected(std::format("the world {} is in use: {}", root_.string(), held.error()));
    lock_ = std::move(*held);
    return {};
}

Status WorldDirectory::write_manifest(const Manifest& m) {
    std::string text = std::format("circuit-world 1\nbundle {}\nrules {}\ncheckpoint {} {}\n", bundle_id_, rules_id_, m.checkpoint,
                                   m.checkpoint_sha);
    if (!m.previous.empty()) text += std::format("previous {} {}\n", m.previous, m.previous_sha);
    for (const auto& s : m.segments) text += std::format("wal {}\n", s);
    const auto tmp = root_ / "manifest.tmp";
    if (auto ok = fs_.write_file(tmp, bytes_of(text)); !ok) return ok;
    if (auto ok = fs_.rename(tmp, root_ / manifest_file); !ok) return ok;
    return fs_.sync_directory(root_);
}

std::expected<std::optional<WorldDirectory::Identity>, std::string> WorldDirectory::identity(const Filesystem& fs, const Path& root) {
    if (!fs.exists(root / manifest_file)) return std::optional<Identity>{};
    const auto data = fs.read_file(root / manifest_file);
    if (!data) return std::unexpected(data.error());
    std::istringstream in(std::string(reinterpret_cast<const char*>(data->data()), data->size()));
    std::string line;
    Identity id;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string key, value;
        words >> key >> value;
        if (key == "bundle") id.bundle_id = value;
        else if (key == "rules") id.rules_id = value;
    }
    if (id.bundle_id.empty() || id.rules_id.empty()) return std::unexpected("incomplete manifest");
    return std::optional<Identity>{std::move(id)};
}

std::expected<std::optional<WorldDirectory::Manifest>, std::string> WorldDirectory::read_manifest() const {
    if (!fs_.exists(root_ / manifest_file)) return std::optional<Manifest>{};
    const auto data = fs_.read_file(root_ / manifest_file);
    if (!data) return std::unexpected(data.error());
    std::istringstream in(std::string(reinterpret_cast<const char*>(data->data()), data->size()));
    std::string line;
    Manifest m;
    bool header = false;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string key, a, b;
        words >> key >> a >> b;
        if (key == "circuit-world") {
            if (a != "1") return std::unexpected("unsupported manifest version");
            header = true;
        } else if (key == "bundle") {
            if (a != bundle_id_) return std::unexpected(std::format("the world belongs to bundle {}, not {}", a, bundle_id_));
        } else if (key == "rules") {
            if (a != rules_id_) return std::unexpected(std::format("the world was saved under {}, not {}", a, rules_id_));
        } else if (key == "checkpoint" || key == "previous") {
            if (!parse_checkpoint(a) || !valid_sha(b)) return std::unexpected(std::format("invalid manifest line `{}`", line));
            (key == "checkpoint" ? m.checkpoint : m.previous) = a;
            (key == "checkpoint" ? m.checkpoint_sha : m.previous_sha) = b;
        } else if (key == "wal") {
            if (!parse_segment(a)) return std::unexpected(std::format("invalid manifest line `{}`", line));
            m.segments.push_back(a);
        } else if (!key.empty()) {
            return std::unexpected(std::format("unknown manifest line `{}`", line));
        }
    }
    if (!header || m.checkpoint.empty()) return std::unexpected("incomplete manifest");
    return std::optional<Manifest>{std::move(m)};
}

std::expected<Checkpoint, std::string> WorldDirectory::load(const std::string& name, const std::string& sha) const {
    const auto data = fs_.read_file(root_ / name);
    if (!data) return std::unexpected(data.error());
    if (sha256_hex(*data) != sha) return std::unexpected(std::format("{} does not match the manifest", name));
    auto c = decode_checkpoint(*data);
    if (!c) return std::unexpected(std::format("{}: {}", name, c.error()));
    if (c->bundle_id != bundle_id_ || c->rules_id != rules_id_) return std::unexpected(std::format("{}: other bundle or rules", name));
    return c;
}

std::expected<bool, std::string> WorldDirectory::read_segment(const std::string& name, std::uint64_t& tick,
                                                              std::vector<contracts::TickInput>& out, std::vector<std::string>& notes,
                                                              std::set<std::string>& damaged) const {
    const auto data = fs_.read_file(root_ / name);
    if (!data) {
        notes.push_back(std::format("{} is missing", name));
        return false;
    }
    const std::span<const std::byte> in(*data);
    const auto format = in.size() >= segment_magic_size ? std::to_integer<char>(in[segment_prefix.size()]) : char{-1};
    if (in.size() < segment_magic_size + 8 || !std::equal(segment_prefix.begin(), segment_prefix.end(),
                                                          reinterpret_cast<const char*>(in.data())) ||
        format < 0 || format > segment_format || get(in, segment_magic_size, 8) != tick) {
        notes.push_back(std::format("{} has no valid header", name));
        damaged.insert(name);
        return false;
    }
    std::size_t at = segment_magic_size + 8;
    contracts::ReplayLimits limits;
    limits.max_commands_per_tick = 1 << 20;
    while (at < in.size()) {
        if (in.size() - at < 8) break;
        const auto length = static_cast<std::size_t>(get(in, at, 4));
        const auto crc = static_cast<std::uint32_t>(get(in, at + 4, 4));
        if (length > max_record || in.size() - at - 8 < length) break;
        const auto payload = in.subspan(at + 8, length);
        if (crc32_of(payload) != crc) break;
        // A record with a valid checksum is not a torn write: one that does not
        // decode or does not continue the history means the log is corrupt.
        auto input = contracts::decode_tick_input(payload, limits, format >= 1, format >= 2, format >= 7);
        if (!input) return std::unexpected(std::format("{}: undecodable record after tick {}: {}", name, tick, contracts::error_name(input.error())));
        if (input->tick != tick + 1)
            return std::unexpected(std::format("{}: record for tick {} after tick {}", name, input->tick, tick));
        tick = input->tick;
        out.push_back(std::move(*input));
        at += 8 + length;
    }
    if (at < in.size()) {
        notes.push_back(std::format("{}: discarded {} bytes of an incomplete or damaged tail after tick {}", name, in.size() - at, tick));
        damaged.insert(name);
        return false;
    }
    return true;
}

std::expected<std::optional<Recovery>, std::string> WorldDirectory::recover() {
    std::lock_guard lock(mutex_);
    if (auto ok = acquire(); !ok) return std::unexpected(ok.error());
    pending_ = false;
    auto manifest = read_manifest();
    if (!manifest) return std::unexpected(manifest.error());
    if (!*manifest) {
        broken_ = false;
        generation_ = 0;
        manifest_.reset();
        segment_.clear();
        last_tick_ = 0;
        return std::optional<Recovery>{};
    }
    const auto& m = **manifest;
    Recovery r;
    std::set<std::string> damaged;
    auto base = load(m.checkpoint, m.checkpoint_sha);
    bool fallback = false;
    if (!base) {
        r.notes.push_back(std::format("current checkpoint unusable: {}", base.error()));
        damaged.insert(m.checkpoint);
        if (m.previous.empty()) return std::unexpected(std::format("no usable checkpoint: {}", base.error()));
        base = load(m.previous, m.previous_sha);
        if (!base) return std::unexpected(std::format("no usable checkpoint: {}", base.error()));
        r.notes.push_back(std::format("continuing from the previous checkpoint {}", m.previous));
        fallback = true;
    }
    r.checkpoint = std::move(*base);
    std::uint64_t tick = r.checkpoint.state.time;
    // Segments before the checkpoint hold inputs it already contains. From
    // then on, each segment continues where the previous one ended: a
    // segment whose tail was discarded can be followed by one that a
    // checkpoint at its last complete record started.
    bool reading = false;
    for (const auto& seg : m.segments) {
        const auto start = parse_segment(seg)->tick;
        if (!reading && start < tick) continue;
        if (start != tick) {
            r.notes.push_back(std::format("{} does not continue tick {}", seg, tick));
            break;
        }
        reading = true;
        const auto complete = read_segment(seg, tick, r.inputs, r.notes, damaged);
        if (!complete) return std::unexpected(complete.error());
    }
    broken_ = false;
    damaged_ = std::move(damaged);
    manifest_ = m;
    generation_ = parse_checkpoint(m.checkpoint)->generation;
    if (!m.previous.empty()) generation_ = std::max(generation_, parse_checkpoint(m.previous)->generation);
    for (const auto& seg : m.segments) generation_ = std::max(generation_, parse_segment(seg)->generation);
    if (fallback) {
        // Later checkpoints and the fallback refer to the checkpoint that was read.
        manifest_->checkpoint = m.previous;
        manifest_->checkpoint_sha = m.previous_sha;
    }
    // Appending after a discarded tail would hide new records behind it: the
    // caller replays the inputs and writes a checkpoint, which starts a fresh
    // segment, before logging again.
    segment_.clear();
    last_tick_ = tick;
    return std::optional<Recovery>{std::move(r)};
}

std::expected<PendingCheckpoint, std::string> WorldDirectory::begin_checkpoint(std::uint64_t tick) {
    std::lock_guard lock(mutex_);
    if (auto ok = acquire(); !ok) return std::unexpected(ok.error());
    if (broken_) return std::unexpected("the manifest state is unknown after a failed write; recover first");
    if (pending_) return std::unexpected("a checkpoint is already in progress");
    if (manifest_ && tick != last_tick_) {
        // Before logging resumes, a checkpoint may end the history earlier,
        // abandoning logged inputs that the caller could not apply.
        const bool abandons = segment_.empty() && tick < last_tick_ && tick >= parse_checkpoint(manifest_->checkpoint)->tick;
        if (!abandons)
            return std::unexpected(std::format("checkpoint at tick {} does not match the history at tick {}", tick, last_tick_));
    }
    // The fallback replays the current segment, so it must be complete.
    if (!segment_.empty())
        if (auto ok = fs_.append(root_ / segment_, {}, true); !ok) return std::unexpected(ok.error());
    // A new generation never reuses a name the durable manifest refers to.
    const auto generation = generation_ + 1;
    // The segment for the ticks after this checkpoint.
    std::vector<std::byte> header(segment_magic_size);
    std::memcpy(header.data(), segment_prefix.data(), segment_prefix.size());
    header[segment_prefix.size()] = static_cast<std::byte>(segment_format);
    put64(header, tick);
    const auto seg = segment_name(tick, generation);
    if (auto ok = fs_.write_file(root_ / seg, header); !ok) return std::unexpected(ok.error());
    if (auto ok = fs_.sync_directory(root_); !ok) return std::unexpected(ok.error());
    PendingCheckpoint pending{tick, generation, checkpoint_name(tick, generation), seg, {}, {}};
    if (manifest_) {
        // Until the checkpoint is written, recovery continues from the current
        // checkpoint through the new segment.
        auto next = *manifest_;
        next.segments.push_back(seg);
        segment_.clear();
        if (auto ok = write_manifest(next); !ok) {
            broken_ = true;
            return std::unexpected(ok.error());
        }
        pending.base_checkpoint = manifest_->checkpoint;
        pending.base_sha = manifest_->checkpoint_sha;
        manifest_ = next;
    }
    generation_ = generation;
    segment_ = seg;
    last_tick_ = tick;
    pending_ = true;
    return pending;
}

Status WorldDirectory::finish_checkpoint(const PendingCheckpoint& pending, std::span<const std::byte> encoded) {
    const auto clear = [&] {
        std::lock_guard lock(mutex_);
        pending_ = false;
    };
    // The data is written without the lock: logging continues meanwhile.
    if (auto ok = fs_.write_file(root_ / (pending.name + ".tmp"), encoded); !ok) return clear(), ok;
    if (auto ok = fs_.rename(root_ / (pending.name + ".tmp"), root_ / pending.name); !ok) return clear(), ok;
    if (auto ok = fs_.sync_directory(root_); !ok) return clear(), ok;
    Manifest next;
    {
        std::lock_guard lock(mutex_);
        pending_ = false;
        if (broken_) return std::unexpected("the manifest state is unknown after a failed write; recover first");
        next.checkpoint = pending.name;
        next.checkpoint_sha = sha256_hex(encoded);
        if (manifest_) {
            next.previous = pending.base_checkpoint;
            next.previous_sha = pending.base_sha;
            // Segments from the previous checkpoint on, so the fallback can replay to now.
            const auto prev_tick = parse_checkpoint(pending.base_checkpoint)->tick;
            for (const auto& s : manifest_->segments)
                if (parse_segment(s)->tick >= prev_tick) next.segments.push_back(s);
        } else {
            next.segments.push_back(pending.segment);
        }
        // A failure from here on leaves the old or the new manifest durable;
        // either recovers, but this object can no longer tell which.
        if (auto ok = write_manifest(next); !ok) {
            broken_ = true;
            segment_.clear();
            return ok;
        }
        manifest_ = next;
    }
    remove_unreferenced(next);
    return {};
}

Status WorldDirectory::checkpoint(const Checkpoint& c) {
    if (c.bundle_id != bundle_id_ || c.rules_id != rules_id_) return std::unexpected("checkpoint from other bundle or rules");
    auto pending = begin_checkpoint(c.state.time);
    if (!pending) return std::unexpected(pending.error());
    return finish_checkpoint(*pending, encode_checkpoint(c));
}

void WorldDirectory::remove_unreferenced(const Manifest& m) {
    std::set<std::string> keep{std::string(manifest_file), m.checkpoint};
    if (!m.previous.empty()) keep.insert(m.previous);
    keep.insert(m.segments.begin(), m.segments.end());
    const auto names = fs_.list(root_);
    if (!names) return;
    for (const auto& n : *names) {
        if (keep.contains(n) || !(n.starts_with("ckpt-") || n.starts_with("wal-") || n.ends_with(".tmp"))) continue;
        // Files recovery found damaged stay for diagnosis under another name.
        if (damaged_.erase(n) > 0) (void)fs_.rename(root_ / n, root_ / ("damaged-" + n));
        else (void)fs_.remove(root_ / n);
    }
}

Status WorldDirectory::append(const contracts::TickInput& input, bool sync) {
    std::lock_guard lock(mutex_);
    if (!lock_) return std::unexpected("the world directory is not owned");
    if (segment_.empty()) return std::unexpected("no checkpoint: the log has no segment");
    if (input.tick != last_tick_ + 1) return std::unexpected(std::format("tick {} does not follow {}", input.tick, last_tick_));
    const auto payload = contracts::encode_tick_input(input);
    std::vector<std::byte> record;
    record.reserve(payload.size() + 8);
    put32(record, static_cast<std::uint32_t>(payload.size()));
    put32(record, crc32_of(payload));
    record.insert(record.end(), payload.begin(), payload.end());
    if (auto ok = fs_.append(root_ / segment_, record, sync); !ok) {
        // A partly written record would hide every later one: stop logging to
        // this segment. A checkpoint of the state at the last logged tick
        // starts a new one.
        segment_.clear();
        return ok;
    }
    last_tick_ = input.tick;
    return {};
}

} // namespace circuit::storage
