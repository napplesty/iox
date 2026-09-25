// iox — pipe/channel.h: typed pipe ends (design §四.①).
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <expected>
#include <type_traits>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"

namespace iox::pipe {

class read_end {
public:
    read_end() noexcept = default;

    bool valid() const noexcept { return fd_.valid(); }

    iox::fd read_handle() const noexcept { return fd_.get(); }
    iox::fd* fd_slot() noexcept { return fd_.slot(); }

    void reset() noexcept { fd_.reset(); }

private:
    friend struct pair;
    explicit read_end(int raw) noexcept : fd_(raw) {}

    iox::unique_fd fd_{};
};

class write_end {
public:
    write_end() noexcept = default;

    bool valid() const noexcept { return fd_.valid(); }

    iox::fd write_handle() const noexcept { return fd_.get(); }
    iox::fd* fd_slot() noexcept { return fd_.slot(); }

    void reset() noexcept { fd_.reset(); }

private:
    friend struct pair;
    explicit write_end(int raw) noexcept : fd_(raw) {}

    iox::unique_fd fd_{};
};

struct pair {
    read_end r;
    write_end w;

    static std::expected<pair, error> create() noexcept {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return pair{read_end{fds[0]}, write_end{fds[1]}};
    }
};

static_assert(io::readable<read_end> && !io::writable<read_end>);
static_assert(!io::readable<write_end> && io::writable<write_end>);
static_assert(!io::seekable<read_end> && !io::seekable<write_end>);
static_assert(std::is_nothrow_move_constructible_v<read_end> &&
              !std::is_copy_constructible_v<read_end>);
static_assert(std::is_nothrow_move_constructible_v<write_end> &&
              !std::is_copy_constructible_v<write_end>);

}
