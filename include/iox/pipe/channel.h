// iox — unified async IO for Linux
// include/iox/pipe/channel.h — typed pipe ends (design §四.①).
#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <unistd.h>

#include <expected>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"

namespace iox::pipe {

class read_end {
public:
    read_end() noexcept = default;
    ~read_end() { reset(); }

    read_end(read_end&& other) noexcept : fd_(std::exchange(other.fd_, iox::fd{})) {}
    read_end& operator=(read_end&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, iox::fd{});
        }
        return *this;
    }
    read_end(const read_end&) = delete;
    read_end& operator=(const read_end&) = delete;

    bool valid() const noexcept { return fd_.valid(); }

    iox::fd read_handle() const noexcept { return fd_; }
    iox::fd* fd_slot() noexcept { return &fd_; }

    void reset() noexcept {
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    friend struct pair;
    explicit read_end(int raw) noexcept : fd_(raw) {}

    iox::fd fd_{};
};

class write_end {
public:
    write_end() noexcept = default;
    ~write_end() { reset(); }

    write_end(write_end&& other) noexcept : fd_(std::exchange(other.fd_, iox::fd{})) {}
    write_end& operator=(write_end&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, iox::fd{});
        }
        return *this;
    }
    write_end(const write_end&) = delete;
    write_end& operator=(const write_end&) = delete;

    bool valid() const noexcept { return fd_.valid(); }

    iox::fd write_handle() const noexcept { return fd_; }
    iox::fd* fd_slot() noexcept { return &fd_; }

    void reset() noexcept {
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    friend struct pair;
    explicit write_end(int raw) noexcept : fd_(raw) {}

    iox::fd fd_{};
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

}
