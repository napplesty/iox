// iox — unified async IO for Linux
// include/iox/ops/schedule.h — loop / continuation uses to run the next step from the event loop itself.
#pragma once

#include "iox/ops/fd_sender.h"

namespace iox::io {

namespace detail {

struct nop_policy {
    struct args_t {};
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd, args_t&) noexcept {
        ::io_uring_prep_nop(sqe);
    }
    using complete = void_complete;
};

using schedule_sender = fd_sender<nop_policy>;

}

inline constexpr struct schedule_t {
    detail::schedule_sender operator()(io_context& ctx) const noexcept {
        return {&ctx, {}, {}};
    }
} schedule{};

}
