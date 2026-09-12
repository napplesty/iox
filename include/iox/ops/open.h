// io::open — deferred open through the ring (IORING_OP_OPENAT): no blocking
// syscall on the calling thread, and a cold open (say, a cache-missed path
// on a busy server) batches with the surrounding operations. Completes with
// an adopted fs::file. The path string is CALLER-OWNED (zero-copy contract,
// same as data buffers): it must stay alive until the sender completes.
#pragma once

#include <fcntl.h>

#include "iox/core/cpo.h"
#include "iox/core/buffer.h"
#include "iox/fs/file.h"
#include "iox/ops/fd_sender.h"

namespace iox::io {

namespace detail {

struct open_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_value(std::move(r), fs::file{fs::file::adopt_fd_t{}, res});
        }
    }
};

struct open_policy {
    struct args_t {
        const char* path = nullptr;
        int flags = 0;
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(fs::file),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd, args_t& a) noexcept {
        // O_CLOEXEC always, like every descriptor iox mints (accept uses
        // SOCK_CLOEXEC for the same reason): a fork+exec must not leak fds.
        ::io_uring_prep_openat(sqe, AT_FDCWD, a.path, a.flags | O_CLOEXEC, 0644);
    }
    using complete = open_complete;
};

} // namespace detail

inline constexpr struct open_t {
    /// Async open(2) relative to the working directory. Customization point:
    /// drivers provide `tag_invoke(open_t, ctx, path, fs::mode)`; the fd
    /// default below serves the page-cache filesystem path.
    /// `path` forwards as its own type (like the handle in every other CPO)
    /// so the dispatch stays dependent and resolves per call site.
    template <class P>
    requires std::convertible_to<const P&, const char*> &&
             tag_invocable<open_t, io_context&, P, fs::mode>
    auto operator()(io_context& ctx, P&& path, fs::mode m) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<P>(path), m)))
        -> decltype(tag_invoke(*this, ctx, std::forward<P>(path), m)) {
        return tag_invoke(*this, ctx, std::forward<P>(path), m);
    }
} open{};

// ---- fd driver default -----------------------------------------------------

inline auto tag_invoke(open_t, io_context& ctx, const char* path, fs::mode m) noexcept {
    return detail::fd_sender<detail::open_policy>{
        &ctx, iox::fd{}, {path, static_cast<int>(m)}};
}

} // namespace iox::io
