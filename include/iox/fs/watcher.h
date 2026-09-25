// iox — fs/watcher.h: an inotify instance as a readable handle.
#pragma once

#include <cstdint>

#include <sys/inotify.h>

#include <expected>
#include <string_view>
#include <type_traits>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"

namespace iox::fs {

class watcher {
public:
    watcher() noexcept = default;

    static std::expected<watcher, error> create() noexcept {
        const int raw = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return watcher{raw};
    }

    bool valid() const noexcept { return fd_.valid(); }

    std::expected<int, error> add(std::string_view path, std::uint32_t mask) const noexcept {
        const std::string p{path};
        const int wd = ::inotify_add_watch(fd_.get().v, p.c_str(), mask);
        if (wd < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return wd;
    }

    void erase(int wd) const noexcept { (void)!::inotify_rm_watch(fd_.get().v, wd); }

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
