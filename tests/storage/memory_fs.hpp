// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// An in-memory file system with crash semantics, for storage tests:
// - file contents are durable after write_file or a synced append;
// - directory entries (creations, renames, removals) are durable after
//   sync_directory of their directory;
// - crashed() returns what survives a crash: durable entries with their
//   durable contents, optionally with a random prefix of unsynced appended
//   bytes (a torn write).
// Operations can fail from a chosen index on (a crash at that point) or once
// (a transient error such as a full disk).
#include <circuit/storage/filesystem.hpp>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>

namespace circuit::test {
using storage::Path;
using storage::Status;

class MemoryFilesystem final : public storage::Filesystem {
public:
    struct File {
        std::vector<std::byte> durable, current;
    };

    std::size_t operations() const noexcept { return ops_; }
    // Operation `index` and all later ones fail.
    void crash_at(std::size_t index) { crash_at_ = index; }
    // Only operation `index` fails, with `message`; a failing append stores
    // half of its data.
    void fail_once(std::size_t index, std::string message) {
        fail_once_ = index;
        fail_message_ = std::move(message);
    }

    MemoryFilesystem crashed(std::mt19937_64* torn = nullptr) const {
        MemoryFilesystem after;
        for (const auto& [path, file] : durable_) {
            auto copy = std::make_shared<File>();
            copy->durable = file->durable;
            const auto& cur = file->current;
            // Unsynced data appended after the durable content may partly survive.
            if (torn && cur.size() > file->durable.size() && std::equal(file->durable.begin(), file->durable.end(), cur.begin())) {
                const auto extra = std::uniform_int_distribution<std::size_t>(0, cur.size() - file->durable.size())(*torn);
                copy->durable.insert(copy->durable.end(), cur.begin() + static_cast<std::ptrdiff_t>(file->durable.size()),
                                     cur.begin() + static_cast<std::ptrdiff_t>(file->durable.size() + extra));
            }
            copy->current = copy->durable;
            after.current_[path] = copy;
            after.durable_[path] = copy;
        }
        return after;
    }

    Status create_directories(const Path&) override { return step(); }
    Status write_file(const Path& p, std::span<const std::byte> data) override {
        if (auto ok = step(); !ok) return ok;
        auto& slot = current_[p.string()];
        const auto d = durable_.find(p.string());
        // Rewriting an existing file keeps its inode; a new name gets a new one.
        if (!slot || d == durable_.end() || d->second != slot) slot = std::make_shared<File>();
        slot->current.assign(data.begin(), data.end());
        slot->durable = slot->current;
        return {};
    }
    Status append(const Path& p, std::span<const std::byte> data, bool sync) override {
        const auto it = current_.find(p.string());
        if (auto ok = step(); !ok) {
            // A failing write may have stored part of its data (a short write).
            if (it != current_.end() && ops_ - 1 == fail_once_)
                it->second->current.insert(it->second->current.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(data.size() / 2));
            return ok;
        }
        if (it == current_.end()) return std::unexpected("append: no such file");
        it->second->current.insert(it->second->current.end(), data.begin(), data.end());
        if (sync) it->second->durable = it->second->current;
        return {};
    }
    Status rename(const Path& from, const Path& to) override {
        if (auto ok = step(); !ok) return ok;
        const auto it = current_.find(from.string());
        if (it == current_.end()) return std::unexpected("rename: no such file");
        current_[to.string()] = it->second;
        current_.erase(from.string());
        return {};
    }
    Status sync_directory(const Path& dir) override {
        if (auto ok = step(); !ok) return ok;
        for (auto it = durable_.begin(); it != durable_.end();)
            it = Path(it->first).parent_path() == dir && !current_.contains(it->first) ? durable_.erase(it) : std::next(it);
        for (const auto& [path, file] : current_)
            if (Path(path).parent_path() == dir) durable_[path] = file;
        return {};
    }
    Status remove(const Path& p) override {
        if (auto ok = step(); !ok) return ok;
        current_.erase(p.string());
        return {};
    }
    std::expected<std::vector<std::byte>, std::string> read_file(const Path& p) const override {
        const auto it = current_.find(p.string());
        if (it == current_.end()) return std::unexpected("no such file " + p.string());
        return it->second->current;
    }
    std::expected<std::vector<std::string>, std::string> list(const Path& dir) const override {
        std::vector<std::string> names;
        for (const auto& [path, file] : current_)
            if (Path(path).parent_path() == dir) names.push_back(Path(path).filename().string());
        return names;
    }
    bool exists(const Path& p) const override { return current_.contains(p.string()); }
    // Locks are not files and not operations: they never fail by injection,
    // and a crashed image starts with none held.
    std::expected<std::unique_ptr<storage::FileLock>, std::string> lock(const Path& p) override {
        if (!locks_->insert(p.string()).second) return std::unexpected(p.string() + " is held by another owner");
        return std::make_unique<Held>(locks_, p.string());
    }

    // Mutable access to a file's bytes (corruption tests).
    std::vector<std::byte>* bytes(const Path& p) {
        const auto it = current_.find(p.string());
        if (it == current_.end()) return nullptr;
        it->second->durable = it->second->current;
        return &it->second->current;
    }

private:
    struct Held final : storage::FileLock {
        Held(std::shared_ptr<std::set<std::string>> set, std::string path) : set_(std::move(set)), path_(std::move(path)) {}
        ~Held() override { set_->erase(path_); }
        std::shared_ptr<std::set<std::string>> set_;
        std::string path_;
    };

    Status step() {
        const auto index = ops_++;
        if (index >= crash_at_) return std::unexpected("simulated crash");
        if (index == fail_once_) return std::unexpected(fail_message_);
        return {};
    }

    std::map<std::string, std::shared_ptr<File>> current_, durable_;
    std::shared_ptr<std::set<std::string>> locks_ = std::make_shared<std::set<std::string>>();
    std::size_t ops_{};
    std::size_t crash_at_{std::numeric_limits<std::size_t>::max()};
    std::size_t fail_once_{std::numeric_limits<std::size_t>::max()};
    std::string fail_message_;
};

} // namespace circuit::test
