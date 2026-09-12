// io::send_to — connectionless datagram send (IORING_OP_SENDMSG) with an
// explicit destination endpoint.
#pragma once

#include <sys/socket.h>

#include <cstring>

#include "iox/net/endpoint.h"
#include "iox/core/buffer.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct send_to_policy {
    struct args_t {
        sockaddr_storage ss{};
        socklen_t len = 0;
        rbytes src{};
        iovec iov{};
        msghdr msg{};
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        a.iov = iovec{const_cast<void*>(static_cast<const void*>(a.src.data())), a.src.size()};
        a.msg = msghdr{};
        a.msg.msg_name = &a.ss;
        a.msg.msg_namelen = a.len;
        a.msg.msg_iov = &a.iov;
        a.msg.msg_iovlen = 1;
        ::io_uring_prep_sendmsg(sqe, f.v, &a.msg, 0);
    }
    using complete = transfer_complete;
};

} // namespace detail

inline constexpr struct send_to_t {
    /// Customization point: drivers provide `tag_invoke(send_to_t, ctx,
    /// handle, rbytes, const net::endpoint&)`; the fd default below serves
    /// socket-backed datagram handles.
    template <class H>
    requires tag_invocable<send_to_t, io_context&, H, rbytes, const net::endpoint&>
    auto operator()(io_context& ctx, H&& h, rbytes src, const net::endpoint& to) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), src, to)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), src, to)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), src, to);
    }
} send_to{};

// ---- fd driver default -----------------------------------------------------

template <class H>
requires datagram<std::remove_cvref_t<H>>
auto tag_invoke(send_to_t, io_context& ctx, H&& h, rbytes src,
                const net::endpoint& to) noexcept {
    detail::send_to_policy::args_t a{};
    std::memcpy(&a.ss, to.data(), to.size());
    a.len = to.size();
    a.src = src;
    return detail::fd_sender<detail::send_to_policy>{&ctx, h.datagram_handle(), a};
}

} // namespace iox::io
