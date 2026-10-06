// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/replay.hpp>
#include <circuit/storage/checkpoint.hpp>
#include <circuit/storage/filesystem.hpp>
#include <mutex>
#include <optional>
#include <set>

// A world on disk: checkpoints plus a write-ahead log of the inputs admitted
// after them.
//
//   manifest                  current state, replaced atomically
//   ckpt-<tick>-<gen>.bin     checkpoint at the end of <tick>
//   wal-<tick>-<gen>.log      inputs of the ticks after <tick>
//
// Every checkpoint has a new generation number, so files the durable
// manifest refers to are never rewritten. The manifest names the current
// checkpoint, the previous one (a fallback if the current one cannot be
// read) and the log segments that follow the previous one. A checkpoint
// becomes current only when the manifest that names it is durable, so a
// crash at any point leaves either the old or the new state. Each log record
// carries its length and CRC-32; recovery replays complete records, discards
// a torn tail and fails on a record whose checksum is valid but whose
// content is not. Files from which recovery discarded data are renamed to
// damaged-<name> instead of being removed once no manifest needs them.
namespace circuit::storage {

struct Recovery {
    Checkpoint checkpoint;
    std::vector<contracts::TickInput> inputs; // ticks after the checkpoint, in order
    std::vector<std::string> notes;           // fallbacks used and data discarded
};

// A checkpoint whose log segment has started but whose data is not written yet.
struct PendingCheckpoint {
    std::uint64_t tick{}, generation{};
    std::string name, segment;
    std::string base_checkpoint, base_sha; // the checkpoint that was current when it began
};

class WorldDirectory {
public:
    WorldDirectory(Filesystem& fs, Path root, std::string bundle_id, std::string rules_id);

    // The latest durable state; nullopt for a world that was never saved.
    // Fails when a manifest exists but none of its checkpoints is usable, or
    // when the world was saved by other bundles or rules. After recovery,
    // write a checkpoint of the replayed state before appending.
    std::expected<std::optional<Recovery>, std::string> recover();

    // Writes `checkpoint` durably, makes it current and starts a new log
    // segment. The checkpoint must match the directory's bundle and rules and,
    // once the world has history, be taken at the last checkpointed or logged
    // tick. While logging is stopped (after recovery or a failed append), it
    // may also be taken at an earlier tick, not before the current
    // checkpoint: the inputs logged after it are then abandoned. After a
    // failure while replacing the manifest, call recover().
    Status checkpoint(const Checkpoint& checkpoint);

    // The same in two phases, so that the data can be written while logging
    // goes on. begin_checkpoint, on the thread that logs, ends the current
    // segment, starts the one for the ticks after `tick` and makes the
    // manifest list it; appends go to the new segment from then on.
    // finish_checkpoint, on any thread, writes the encoded checkpoint of the
    // state at `tick` and makes it current. One checkpoint at a time; if
    // finishing fails before the manifest switch, the next begin starts over.
    std::expected<PendingCheckpoint, std::string> begin_checkpoint(std::uint64_t tick);
    Status finish_checkpoint(const PendingCheckpoint& pending, std::span<const std::byte> encoded);

    // Appends one tick's input to the current segment. Ticks must follow the
    // last checkpoint or record without gaps. With `sync` the record is
    // durable when this returns. After a failure, logging resumes only after
    // a checkpoint.
    Status append(const contracts::TickInput& input, bool sync);

    [[nodiscard]] bool logging() noexcept {
        std::lock_guard lock(mutex_);
        return !segment_.empty();
    }
    [[nodiscard]] std::uint64_t last_tick() noexcept {
        std::lock_guard lock(mutex_);
        return last_tick_;
    }

    [[nodiscard]] const Path& root() const noexcept { return root_; }

    // The bundle and rule-set ids a world directory was saved under; nullopt
    // when it holds no world.
    struct Identity {
        std::string bundle_id, rules_id;
    };
    static std::expected<std::optional<Identity>, std::string> identity(const Filesystem& fs, const Path& root);

private:
    struct Manifest {
        std::string checkpoint, checkpoint_sha;
        std::string previous, previous_sha;
        std::vector<std::string> segments;
    };
    Status write_manifest(const Manifest& m);
    std::expected<std::optional<Manifest>, std::string> read_manifest() const;
    std::expected<Checkpoint, std::string> load(const std::string& name, const std::string& sha) const;
    // Reads the records of one segment that follow `tick`, advancing it. Returns
    // false when the segment is missing or a damaged tail was discarded.
    std::expected<bool, std::string> read_segment(const std::string& name, std::uint64_t& tick,
                                                  std::vector<contracts::TickInput>& out, std::vector<std::string>& notes,
                                                  std::set<std::string>& damaged) const;
    void remove_unreferenced(const Manifest& m);

    Filesystem& fs_;
    Path root_;
    std::string bundle_id_, rules_id_;
    std::optional<Manifest> manifest_;
    std::string segment_;          // current log segment; empty while appending is not possible
    std::uint64_t last_tick_{};    // last tick checkpointed or logged
    std::uint64_t generation_{};   // highest generation the manifest refers to
    bool broken_{false};           // a manifest write failed: the durable manifest is unknown
    bool pending_{false};          // a checkpoint has begun and not finished
    std::mutex mutex_;             // the state above; file writes of finish_checkpoint run outside it
    std::set<std::string> damaged_; // files recovery found damaged, kept as damaged-<name> once unreferenced
};

std::string checkpoint_name(std::uint64_t tick, std::uint64_t generation);
std::string segment_name(std::uint64_t tick, std::uint64_t generation);

} // namespace circuit::storage
