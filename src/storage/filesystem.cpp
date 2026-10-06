// SPDX-License-Identifier: AGPL-3.0-only
#include <cerrno>
#include <circuit/storage/filesystem.hpp>
#include <cstring>
#include <fcntl.h>
#include <format>
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

} // namespace circuit::storage
