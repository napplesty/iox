// iox — unified async IO for Linux
// include/iox/signal/watcher.h — the signalfd half of signal handling.
#pragma once

#include <sys/signalfd.h>
#include <unistd.h>

#include <expected>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/signal/set.h"

namespace iox::signal {

class watcher {
public:
    watcher() noexcept = default;
    ~watcher() { reset(); }

    static std::expected<watcher, error> create(const set& s) noexcept {
        const int raw = ::signalfd(-1, &s.mask(), SFD_NONBLOCK | SFD_CLOEXEC);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return watcher{raw};
    }

    watcher(watcher&& other) noexcept : fd_(std::exchange(other.fd_, iox::fd{})) {}
    watcher& operator=(watcher&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, iox::fd{});
        }
        return *this;
    }
    watcher(const watcher&) = delete;
    watcher& operator=(const watcher&) = delete;

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
    explicit watcher(int raw) noexcept : fd_(raw) {}

    iox::fd fd_{};
};

static_assert(io::readable<watcher> && !io::writable<watcher> && !io::seekable<watcher>);

}
