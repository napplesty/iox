// io::connect — initiate a stream connection (IORING_OP_CONNECT). For TCP
// the socket must be in the unconnected state; completion is successful
// connect or error (ECONNREFUSED, ETIMEDOUT, …).
#pragma once

#include <sys/socket.h>

#include <cstring>

#include "iox/net/endpoint.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct connect_policy {
    struct args_t {
        sockaddr_storage ss{};
        socklen_t length = 0;
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_connect(sqe, f.v, reinterpret_cast<const sockaddr*>(&a.ss), a.length);
    }
    using complete = void_complete;
};

} // namespace detail

inline constexpr struct connect_t {
    /// Customization point: drivers provide `tag_invoke(connect_t, ctx,
    /// handle, const net::endpoint&)`; the fd default below serves
    /// socket-backed handles.
    template <class H>
    requires tag_invocable<connect_t, io_context&, H, const net::endpoint&>
    auto operator()(io_context& ctx, H&& h, const net::endpoint& ep) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), ep)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), ep)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), ep);
    }
} connect{};

// ---- fd driver default -----------------------------------------------------

template <class H>
requires connectable<std::remove_cvref_t<H>>
auto tag_invoke(connect_t, io_context& ctx, H&& h, const net::endpoint& ep) noexcept {
    detail::connect_policy::args_t a{};
    std::memcpy(&a.ss, ep.data(), ep.size());
    a.length = ep.size();
    return detail::fd_sender<detail::connect_policy>{&ctx, h.connect_handle(), a};
}

} // namespace iox::io
