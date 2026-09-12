// io::accept — accept one connection on an acceptable handle; the value is
// the acceptor's own socket type, adopted from the raw accepted fd.
// (Multishot accept changes the programming model to event streams —
// tracked for the stream abstraction in a later milestone.)
#pragma once

#include <sys/socket.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

template <class Socket>
struct adopt_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_value(std::move(r), Socket{typename Socket::adopt_fd_t{}, res});
        }
    }
};

template <class Socket>
struct accept_policy {
    struct args_t {};
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(Socket),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t&) noexcept {
        ::io_uring_prep_accept(sqe, f.v, nullptr, nullptr, SOCK_CLOEXEC);
    }
    using complete = adopt_complete<Socket>;
};

} // namespace detail

inline constexpr struct accept_t {
    /// Customization point: drivers provide `tag_invoke(accept_t, ctx,
    /// handle)` for their own acceptor types; the fd default below serves
    /// socket-backed acceptors.
    template <class H>
    requires tag_invocable<accept_t, io_context&, H>
    auto operator()(io_context& ctx, H&& h) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h))))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h))) {
        return tag_invoke(*this, ctx, std::forward<H>(h));
    }
} accept{};

// ---- fd driver default -----------------------------------------------------

template <class H>
requires acceptable<std::remove_cvref_t<H>>
auto tag_invoke(accept_t, io_context& ctx, H&& h) noexcept {
    using sock_t = typename std::remove_cvref_t<H>::socket_type;
    return detail::fd_sender<detail::accept_policy<sock_t>>{&ctx, h.accept_handle(), {}};
}

} // namespace iox::io
