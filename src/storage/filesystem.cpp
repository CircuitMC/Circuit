// SPDX-License-Identifier: AGPL-3.0-only
#include <cerrno>
#include <circuit/storage/filesystem.hpp>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <sys/file.h>
#include <unistd.h>

namespace circuit::storage {
namespace {
std::unexpected<std::string> fail(std::string_view what, const Path& p) {
    return std::unexpected(std::format("{} {}: {}", what, p.string(), std::strerror(errno)));
}

Status write_all(int fd, std::span<const std::byte> data, const Path& p) {
    std::size_t done = 0;
    while (done < data.size()) {
        const auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail("write", p);
        }
        done += static_cast<std::size_t>(n);
    }
    return {};
}

struct Fd {
    int fd{-1};
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
};
} // namespace

Status PosixFilesystem::create_directories(const Path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return std::unexpected(std::format("create {}: {}", dir.string(), ec.message()));
    return {};
}

Status PosixFilesystem::write_file(const Path& file, std::span<const std::byte> data) {
    Fd f{::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
    if (f.fd < 0) return fail("open", file);
    if (auto ok = write_all(f.fd, data, file); !ok) return ok;
    if (::fsync(f.fd) != 0) return fail("fsync", file);
    if (::close(f.fd) != 0) {
        f.fd = -1;
        return fail("close", file);
    }
    f.fd = -1;
    return {};
}

Status PosixFilesystem::append(const Path& file, std::span<const std::byte> data, bool sync) {
    Fd f{::open(file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC)};
    if (f.fd < 0) return fail("open", file);
    if (auto ok = write_all(f.fd, data, file); !ok) return ok;
    if (sync && ::fdatasync(f.fd) != 0) return fail("fdatasync", file);
    return {};
}

Status PosixFilesystem::rename(const Path& from, const Path& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) return fail("rename", from);
    return {};
}

Status PosixFilesystem::sync_directory(const Path& dir) {
    Fd f{::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (f.fd < 0) return fail("open", dir);
    if (::fsync(f.fd) != 0) return fail("fsync", dir);
    return {};
}

Status PosixFilesystem::remove(const Path& file) {
    if (::unlink(file.c_str()) != 0 && errno != ENOENT) return fail("unlink", file);
    return {};
}

std::expected<std::vector<std::byte>, std::string> PosixFilesystem::read_file(const Path& file) const {
    Fd f{::open(file.c_str(), O_RDONLY | O_CLOEXEC)};
    if (f.fd < 0) return fail("open", file);
    std::vector<std::byte> out;
    std::byte buf[65536];
    for (;;) {
        const auto n = ::read(f.fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail("read", file);
        }
        if (n == 0) break;
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

std::expected<std::vector<std::string>, std::string> PosixFilesystem::list(const Path& dir) const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file()) names.push_back(e.path().filename().string());
    if (ec) return std::unexpected(std::format("list {}: {}", dir.string(), ec.message()));
    return names;
}

bool PosixFilesystem::exists(const Path& file) const {
    std::error_code ec;
    return std::filesystem::exists(file, ec);
}

namespace {
class FlockLock final : public FileLock {
public:
    explicit FlockLock(int fd) : fd_(fd) {}
    ~FlockLock() override { ::close(fd_); } // closing the last descriptor releases the lock
    FlockLock(const FlockLock&) = delete;
    FlockLock& operator=(const FlockLock&) = delete;

private:
    int fd_;
};
} // namespace

// flock locks belong to the open file description, so a second open of the
// same file conflicts even within one process, and the kernel releases the
// lock when its holder exits for any reason.
std::expected<std::unique_ptr<FileLock>, std::string> PosixFilesystem::lock(const Path& file) {
    Fd f{::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644)};
    if (f.fd < 0) return fail("open", file);
    while (::flock(f.fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EINTR) continue;
        if (errno != EWOULDBLOCK) return fail("lock", file);
        std::string holder;
        char buf[32]{};
        if (const auto n = ::pread(f.fd, buf, sizeof buf - 1, 0); n > 0) holder.assign(buf, static_cast<std::size_t>(n));
        while (!holder.empty() && (holder.back() == '\n' || holder.back() == ' ')) holder.pop_back();
        return std::unexpected(std::format("{} is held by {}", file.string(),
                                           holder.empty() ? std::string("another owner") : "process " + holder));
    }
    const auto pid = std::format("{}\n", ::getpid());
    if (::ftruncate(f.fd, 0) != 0 || ::pwrite(f.fd, pid.data(), pid.size(), 0) != static_cast<ssize_t>(pid.size()))
        return fail("write", file);
    const int fd = f.fd;
    f.fd = -1;
    return std::make_unique<FlockLock>(fd);
}

} // namespace circuit::storage
