// iox — unified async IO for Linux
// include/iox/ops/signal.h — completes with the consumed signalfd_siginfo. A read from the fd (into
#pragma once

#include <sys/signalfd.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct siginfo_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A& a) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_value(std::move(r), a.info);
        }
    }
};

struct signal_policy {
    struct args_t {
        ::signalfd_siginfo info{};
    };
    using signatures = stdexec::completion_signatures<
        stdexec::set_value_t(::signalfd_siginfo), stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_read(sqe, f.v, &a.info, sizeof(a.info), 0);
    }
    using complete = siginfo_complete;
};

}

inline constexpr struct signal_t {
    template <class H>
    requires tag_invocable<signal_t, io_context&, H>
    auto operator()(io_context& ctx, H&& h) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h))))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h))) {
        return tag_invoke(*this, ctx, std::forward<H>(h));
    }
} signal{};

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> || readable<std::remove_cvref_t<H>>
auto tag_invoke(signal_t, io_context& ctx, H&& h) noexcept {
    return detail::fd_sender<detail::signal_policy>{&ctx, detail::reader_fd(h), {}};
}

}
