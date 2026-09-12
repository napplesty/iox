// iox — unified async IO for Linux
// fs/file.h — the typed file handle (design §四.⑤).
//
// fs::file is readable + writable + seekable: io::read/write/read_at/
// write_at/fsync/close all apply. Opening is a synchronous factory returning
// std::expected (typed error, no exceptions); the data path is fully async
// through the caller's io_context.
#pragma once

#include <fcntl.h>
#include <cstdint>
#include <sys/stat.h>
#include <unistd.h>

#include <expected>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"

namespace iox::fs {

/// Open flags — composable bitmask (iox::fs::mode::write | mode::create).
enum class mode : unsigned {
    read = O_RDONLY,
    write = O_WRONLY,
    rw = O_RDWR,
    create = O_CREAT,
    truncate = O_TRUNC,
    append = O_APPEND,
    direct = O_DIRECT, // bypass page cache; buffers must be aligned (use buffer_pool)
};

constexpr mode operator|(mode a, mode b) noexcept {
    return static_cast<mode>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool operator&(mode a, mode b) noexcept {
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) != 0;
}

class file {
public:
    struct adopt_fd_t {}; // internal: adopt an fd handed over by io::open/io::accept

    file() noexcept = default;

    /// Synchronous open(2) with typed errors. For the data path through the
    /// ring, io::open(ctx, path, mode) is the async form.
    static std::expected<file, error> open(const char* path, mode m) noexcept {
        const int raw = ::open(path, static_cast<int>(m), 0644);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return file{adopt_fd_t{}, raw};
    }

    /// Adopt a raw descriptor (io::open completes with this). Public but
    /// internal by convention — the tag type makes accidental adoption
    /// visible in review.
    explicit file(adopt_fd_t, int raw) noexcept : fd_(raw) {}

    ~file() {
        if (fd_.valid()) {
            ::close(fd_.v); // fallback path; io::close defers this through the ring
        }
    }

    file(file&& other) noexcept : fd_(std::exchange(other.fd_, iox::fd{})) {}
    file& operator=(file&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, iox::fd{});
        }
        return *this;
    }
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    bool valid() const noexcept { return fd_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    // capability accessors — consumed by the io:: vocabulary
    iox::fd read_handle() const noexcept { return fd_; }
    iox::fd write_handle() const noexcept { return fd_; }
    static constexpr bool is_seekable() noexcept { return true; }
    iox::fd* fd_slot() noexcept { return &fd_; }

    /// Sync query: current file size (typed error path).
    std::expected<std::uint64_t, error> size() const noexcept {
        struct stat st {};
        if (::fstat(fd_.v, &st) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return static_cast<std::uint64_t>(st.st_size);
    }

    void reset() noexcept {
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    iox::fd fd_{};
};

static_assert(io::readable<file> && io::writable<file> && io::seekable<file>);
static_assert(std::is_nothrow_move_constructible_v<file>);

} // namespace iox::fs
