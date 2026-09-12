// io::signal — wait for one signal on a signal::watcher (or raw signalfd);
// completes with the consumed signalfd_siginfo. A read from the fd (into
// storage riding inside the op state) is all it takes: signals are just
// readable data (design §五: signalfd 异步等待).
#pragma once

#include <sys/signalfd.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

// Kernel semantics (verified on 6.x/7.x): io_uring WAITS on a signalfd read
// with nothing pending — it does not surface EAGAIN. res >= 0 is a full
// siginfo read (a signal arrived and is consumed). Drain patterns must
// count expected records, not read until error: standard signals coalesce
// (N raises -> 1 siginfo), realtime signals queue 1:1.
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
    // The completion buffer lives in the args pack, which lives in the op
    // state — the address given to the kernel stays valid until the CQE.
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


} // namespace detail

inline constexpr struct signal_t {
    /// Wait for the next pending signal. `h` is a signal::watcher, a raw
    /// signalfd, or anything readable that carries signalfd_siginfo records.
    /// Customization point: `tag_invoke(signal_t, ctx, handle)`.
    template <class H>
    requires tag_invocable<signal_t, io_context&, H>
    auto operator()(io_context& ctx, H&& h) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h))))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h))) {
        return tag_invoke(*this, ctx, std::forward<H>(h));
    }
} signal{};

// ---- fd driver default -----------------------------------------------------

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> || readable<std::remove_cvref_t<H>>
auto tag_invoke(signal_t, io_context& ctx, H&& h) noexcept {
    return detail::fd_sender<detail::signal_policy>{&ctx, detail::reader_fd(h), {}};
}

} // namespace iox::io
