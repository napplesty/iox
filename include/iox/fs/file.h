// iox — fs/file.h: the typed file handle (design §四.⑤).
#pragma once

#include <fcntl.h>
#include <cstdint>
#include <sys/stat.h>

#include <expected>
#include <type_traits>
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
        const int raw = ::open(path, static_cast<int>(m) | O_CLOEXEC, 0644);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return file{adopt_fd_t{}, raw};
    }

    explicit file(adopt_fd_t, int raw) noexcept : fd_(raw) {}

    bool valid() const noexcept { return fd_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    iox::fd read_handle() const noexcept { return fd_.get(); }
    iox::fd write_handle() const noexcept { return fd_.get(); }
    static constexpr bool is_seekable() noexcept { return true; }
    iox::fd* fd_slot() noexcept { return fd_.slot(); }

    std::expected<std::uint64_t, error> size() const noexcept {
        struct stat st {};
        if (::fstat(fd_.get().v, &st) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return static_cast<std::uint64_t>(st.st_size);
    }

    void reset() noexcept { fd_.reset(); }

private:
    iox::unique_fd fd_{};
};

static_assert(io::readable<file> && io::writable<file> && io::seekable<file>);
static_assert(std::is_nothrow_move_constructible_v<file> &&
              !std::is_copy_constructible_v<file>);

}
