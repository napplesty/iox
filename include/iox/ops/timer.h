// iox — ops/timer.h: io::sleep_for / io::sleep_until (IORING_OP_TIMEOUT); the timespec lives inside the op.
#pragma once

#include <chrono>
#include <linux/time_types.h>

#include "iox/ops/fd_sender.h"

namespace iox::io {

namespace detail {

inline __kernel_timespec to_kernel_timespec(std::chrono::nanoseconds duration) noexcept {
    if (duration < std::chrono::nanoseconds{0}) {
        duration = std::chrono::nanoseconds{0}; // a past deadline fires immediately
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto nanoseconds = duration - seconds;
    return __kernel_timespec{seconds.count(), nanoseconds.count()};
}

struct timeout_args {
    __kernel_timespec timespec{};
};

inline void prep_timeout(io_uring_sqe* sqe, iox::fd, timeout_args& args) noexcept {
    ::io_uring_prep_timeout(sqe, &args.timespec, 0, 0);
}

struct timeout_policy : basic_policy<timeout_args, prep_timeout, deadline_complete> {
    using signatures = io_signatures<>;
};

using sleep_sender = fd_sender<timeout_policy>;

}

inline constexpr struct sleep_for_t {
    detail::sleep_sender operator()(io_context& context,
                                    std::chrono::nanoseconds duration) const noexcept {
        return {&context, {}, {detail::to_kernel_timespec(duration)}};
    }
} sleep_for{};

inline constexpr struct sleep_until_t {
    detail::sleep_sender operator()(io_context& context,
                                    std::chrono::steady_clock::time_point time_point) const noexcept {
        return sleep_for(context, time_point - std::chrono::steady_clock::now());
    }
} sleep_until{};

}
