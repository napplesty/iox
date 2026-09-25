// iox — ops/wait_pid.h: io::wait_pid(context, process) → exit_status; polls the pidfd, then waitid reaps without blocking.
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
    template <class Receiver, class Args>
    static void complete(Receiver& receiver, std::int32_t result, const Args& args) noexcept {
        if (result < 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_negative(result));
            return;
        }
        ::siginfo_t siginfo{};
        if (::waitid(P_PIDFD, args.pidfd, &siginfo, WEXITED) != 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_errno(errno));
            return;
        }
        stdexec::set_value(std::move(receiver), process::exit_status::from_siginfo(siginfo));
    }
};

struct wait_pid_args {
    int pidfd = -1;
};

inline void prep_wait_pid(io_uring_sqe* sqe, iox::fd fd, const wait_pid_args&) noexcept {
    ::io_uring_prep_poll_add(sqe, fd.v, POLLIN);
}

struct wait_pid_policy : basic_policy<wait_pid_args, prep_wait_pid, exit_status_complete> {
    using signatures = io_signatures<process::exit_status>;
};

}

struct wait_pid_tag {};
using wait_pid_t = cpo<wait_pid_tag>;
inline constexpr wait_pid_t wait_pid{};

inline auto tag_invoke(wait_pid_t, io_context& context, const process::process& process) noexcept {
    return detail::fd_sender<detail::wait_pid_policy>{&context, process.pidfd(), {process.pidfd().v}};
}

}
