// iox — unified async IO for Linux
// fs/watcher.h — an inotify instance as a readable handle.
//
// The fd reads variable-length event records; io::read (the ordinary
// vocabulary — nothing new needed) fills a byte buffer and fs::event_range
// (inotify_event.h) walks it. Watches are (path, mask) → wd.
#pragma once

#include <cstdint>

#include <sys/inotify.h>
#include <unistd.h>

#include <expected>
#include <string_view>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"

namespace iox::fs {

class watcher {
public:
    watcher() noexcept = default;
    ~watcher() { reset(); }

    static std::expected<watcher, error> create() noexcept {
        const int raw = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
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

    /// Start watching `path` for `mask` events; returns the watch descriptor.
    std::expected<int, error> add(std::string_view path, std::uint32_t mask) const noexcept {
        // inotify_add_watch takes a NUL-terminated path; string_view carries
        // no guarantee of one, so copy (control path).
        const std::string p{path};
        const int wd = ::inotify_add_watch(fd_.v, p.c_str(), mask);
        if (wd < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return wd;
    }

    void erase(int wd) const noexcept { (void)!::inotify_rm_watch(fd_.v, wd); }

    // capability: readable — event records come out via io::read
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

} // namespace iox::fs
