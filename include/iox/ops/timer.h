// iox — unified async IO for Linux
// include/iox/ops/timer.h — (IORING_OP_TIMEOUT). The timespec lives inside the op; the kernel reads it
#pragma once

#include <chrono>
#include <linux/time_types.h>

#include "iox/ops/fd_sender.h"

namespace iox::io {

namespace detail {

inline __kernel_timespec to_kernel_ts(std::chrono::nanoseconds d) noexcept {
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(d);
    const auto nsec = d - sec;
    return __kernel_timespec{sec.count(), nsec.count()};
}

struct timeout_policy {
    struct args_t {
        __kernel_timespec ts{};
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd, args_t& a) noexcept {
        ::io_uring_prep_timeout(sqe, &a.ts, 0, 0);
    }
    using complete = deadline_complete;
};

using sleep_sender = fd_sender<timeout_policy>;

}

inline constexpr struct sleep_for_t {
    detail::sleep_sender operator()(io_context& ctx,
                                    std::chrono::nanoseconds d) const noexcept {
        return {&ctx, {}, {detail::to_kernel_ts(d)}};
    }
} sleep_for{};

inline constexpr struct sleep_until_t {
    detail::sleep_sender operator()(io_context& ctx,
                                    std::chrono::steady_clock::time_point tp) const noexcept {
        return sleep_for(ctx, tp - std::chrono::steady_clock::now());
    }
} sleep_until{};

}
