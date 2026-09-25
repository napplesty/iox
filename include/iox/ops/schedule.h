// iox — ops/schedule.h: io::schedule(context) (IORING_OP_NOP); runs the next step from the event loop itself.
#pragma once

#include "iox/ops/fd_sender.h"

namespace iox::io {

namespace detail {

struct nop_args {};

inline void prep_nop(io_uring_sqe* sqe, iox::fd, const nop_args&) noexcept {
    ::io_uring_prep_nop(sqe);
}

struct nop_policy : basic_policy<nop_args, prep_nop, void_complete> {
    using signatures = io_signatures<>;
};

using schedule_sender = fd_sender<nop_policy>;

}

inline constexpr struct schedule_t {
    detail::schedule_sender operator()(io_context& context) const noexcept {
        return {&context, {}, {}};
    }
} schedule{};

}
