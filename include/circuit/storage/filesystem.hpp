// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

// The file operations durable storage relies on, behind an interface so that
// tests can inject failures and simulate crashes. Content is durable once a
// write or sync of the file returned; a directory entry (creation, rename,
// removal) is durable once the directory was synced.
namespace circuit::storage {

using Status = std::expected<void, std::string>;
using Path = std::filesystem::path;

// Exclusive ownership of a file, held until this object is destroyed.
class FileLock {
public:
    virtual ~FileLock() = default;
};

class Filesystem {
public:
    virtual ~Filesystem() = default;
    virtual Status create_directories(const Path& dir) = 0;
    // Creates or truncates the file, writes `data` and syncs the content.
    virtual Status write_file(const Path& file, std::span<const std::byte> data) = 0;
    // Appends to an existing file; syncs the content when `sync` is set.
    virtual Status append(const Path& file, std::span<const std::byte> data, bool sync) = 0;
    // Atomically replaces `to` with `from`.
    virtual Status rename(const Path& from, const Path& to) = 0;
    virtual Status sync_directory(const Path& dir) = 0;
    virtual Status remove(const Path& file) = 0;
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, std::string> read_file(const Path& file) const = 0;
    // Names of the regular files in `dir`.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, std::string> list(const Path& dir) const = 0;
    [[nodiscard]] virtual bool exists(const Path& file) const = 0;
    // Takes exclusive ownership of `file`, creating it if needed, without
    // waiting. Fails when another owner holds it, in this process or another.
    // Ownership ends when the returned object is destroyed or the process
    // exits, so a crash never leaves it held. It changes no other file.
    [[nodiscard]] virtual std::expected<std::unique_ptr<FileLock>, std::string> lock(const Path& file) = 0;
};

// The host file system, with fsync for file contents and directories.
class PosixFilesystem final : public Filesystem {
public:
    Status create_directories(const Path& dir) override;
    Status write_file(const Path& file, std::span<const std::byte> data) override;
    Status append(const Path& file, std::span<const std::byte> data, bool sync) override;
    Status rename(const Path& from, const Path& to) override;
    Status sync_directory(const Path& dir) override;
    Status remove(const Path& file) override;
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> read_file(const Path& file) const override;
    [[nodiscard]] std::expected<std::vector<std::string>, std::string> list(const Path& dir) const override;
    [[nodiscard]] bool exists(const Path& file) const override;
    // flock(2) on the file; the file holds the owner's process id.
    [[nodiscard]] std::expected<std::unique_ptr<FileLock>, std::string> lock(const Path& file) override;
};

} // namespace circuit::storage
