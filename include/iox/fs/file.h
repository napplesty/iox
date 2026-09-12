// iox — unified async IO for Linux
// include/iox/fs/file.h — the typed file handle (design §四.⑤).
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
    struct adopt_fd_t {};

    file() noexcept = default;

    static std::expected<file, error> open(const char* path, mode m) noexcept {
        const int raw = ::open(path, static_cast<int>(m), 0644);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return file{adopt_fd_t{}, raw};
    }

    explicit file(adopt_fd_t, int raw) noexcept : fd_(raw) {}

    ~file() {
        if (fd_.valid()) {
            ::close(fd_.v);
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

    iox::fd read_handle() const noexcept { return fd_; }
    iox::fd write_handle() const noexcept { return fd_; }
    static constexpr bool is_seekable() noexcept { return true; }
    iox::fd* fd_slot() noexcept { return &fd_; }

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

}
