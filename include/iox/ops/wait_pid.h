// io::wait_pid — asynchronous child reaping. A poll on the pidfd fires when
// the child exits; waitid(P_PIDFD) then reaps it without blocking (the exit
// already happened) and the operation completes with process::exit_status.
// Exactly one wait_pid per process — the reap is the read.
#pragma once

#include <poll.h>
#include <sys/wait.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"
#include "iox/process/exit_status.h"
#include "iox/process/process.h"

namespace iox::io {

namespace detail {

// POLLIN fired → the child is dead; waitid reaps (never blocks at that
// point) and the parsed status is the value.
struct exit_status_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A& a) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
            return;
        }
        ::siginfo_t si{};
        if (::waitid(P_PIDFD, a.pidfd, &si, WEXITED) != 0) {
            stdexec::set_error(std::move(r), iox::error::from_errno(errno));
            return;
        }
        stdexec::set_value(std::move(r), process::exit_status::from_siginfo(si));
    }
};

struct wait_pid_policy {
    struct args_t {
        int pidfd = -1;
    };
    using signatures = stdexec::completion_signatures<
        stdexec::set_value_t(process::exit_status), stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t&) noexcept {
        // POLLIN on a pidfd = the process exited (or was killed).
        ::io_uring_prep_poll_add(sqe, f.v, POLLIN);
    }
    using complete = exit_status_complete;
};


} // namespace detail

inline constexpr struct wait_pid_t {
    /// Customization point: drivers provide `tag_invoke(wait_pid_t, ctx,
    /// process-like handle)`; the fd default below serves process::process.
    template <class H>
    requires tag_invocable<wait_pid_t, io_context&, H>
    auto operator()(io_context& ctx, H&& p) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(p))))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(p))) {
        return tag_invoke(*this, ctx, std::forward<H>(p));
    }
} wait_pid{};

// ---- fd driver default -----------------------------------------------------

inline auto tag_invoke(wait_pid_t, io_context& ctx, const process::process& p) noexcept {
    return detail::fd_sender<detail::wait_pid_policy>{&ctx, p.pidfd(), {p.pidfd().v}};
}

} // namespace iox::io
