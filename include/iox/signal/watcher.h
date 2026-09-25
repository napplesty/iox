// iox — signal/watcher.h: the signalfd half of signal handling.
#pragma once

#include <sys/signalfd.h>

#include <expected>
#include <type_traits>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/signal/set.h"

namespace iox::signal {

class watcher {
public:
    watcher() noexcept = default;

    static std::expected<watcher, error> create(const set& s) noexcept {
        const int raw = ::signalfd(-1, &s.mask(), SFD_NONBLOCK | SFD_CLOEXEC);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return watcher{raw};
    }

    bool valid() const noexcept { return fd_.valid(); }

    iox::fd read_handle() const noexcept { return fd_.get(); }
    iox::fd* fd_slot() noexcept { return fd_.slot(); }

    void reset() noexcept { fd_.reset(); }

private:
    explicit watcher(int raw) noexcept : fd_(raw) {}

    iox::unique_fd fd_{};
};

static_assert(io::readable<watcher> && !io::writable<watcher> && !io::seekable<watcher>);
static_assert(std::is_nothrow_move_constructible_v<watcher> &&
              !std::is_copy_constructible_v<watcher>);

}
