// iox — unified async IO for Linux
// include/iox/ops/wait_pid.h — the child exits; waitid(P_PIDFD) then reaps it without blocking (the exit
#pragma once

#include <poll.h>
#include <sys/wait.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"
#include "iox/process/exit_status.h"
#include "iox/process/process.h"

namespace iox::io {

namespace detail {

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
        ::io_uring_prep_poll_add(sqe, f.v, POLLIN);
    }
    using complete = exit_status_complete;
};

}

inline constexpr struct wait_pid_t {
    template <class H>
    requires tag_invocable<wait_pid_t, io_context&, H>
    auto operator()(io_context& ctx, H&& p) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(p))))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(p))) {
        return tag_invoke(*this, ctx, std::forward<H>(p));
    }
} wait_pid{};

inline auto tag_invoke(wait_pid_t, io_context& ctx, const process::process& p) noexcept {
    return detail::fd_sender<detail::wait_pid_policy>{&ctx, p.pidfd(), {p.pidfd().v}};
}

}
