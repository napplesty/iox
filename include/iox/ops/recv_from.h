// iox — unified async IO for Linux
// include/iox/ops/recv_from.h — endpoint).
#pragma once

#include <sys/socket.h>

#include "iox/net/endpoint.h"
#include "iox/core/buffer.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct recv_from_policy {
    struct args_t {
        wbytes dest{};
        iovec iov{};
        msghdr msg{};
        sockaddr_storage ss{};
    };
    using signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t, net::endpoint), stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        a.iov = iovec{a.dest.data(), a.dest.size()};
        a.msg = msghdr{};
        a.msg.msg_name = &a.ss;
        a.msg.msg_namelen = sizeof(a.ss);
        a.msg.msg_iov = &a.iov;
        a.msg.msg_iovlen = 1;
        ::io_uring_prep_recvmsg(sqe, f.v, &a.msg, 0);
    }
    struct complete_from {
        template <class R, class A>
        static void complete(R& r, std::int32_t res, const A& a) noexcept {
            if (res < 0) {
                stdexec::set_error(std::move(r), iox::error::from_negative(res));
                return;
            }
            if (auto ep = net::endpoint::from_native(
                    reinterpret_cast<const sockaddr*>(&a.ss), a.msg.msg_namelen)) {
                stdexec::set_value(std::move(r), static_cast<std::size_t>(res), std::move(*ep));
            } else {
                stdexec::set_error(std::move(r), ep.error());
            }
        }
    };
    using complete = complete_from;
};

}

inline constexpr struct recv_from_t {
    template <class H>
    requires tag_invocable<recv_from_t, io_context&, H, wbytes>
    auto operator()(io_context& ctx, H&& h, wbytes dest) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), dest)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), dest)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), dest);
    }
} recv_from{};

template <class H>
requires datagram<std::remove_cvref_t<H>>
auto tag_invoke(recv_from_t, io_context& ctx, H&& h, wbytes dest) noexcept {
    detail::recv_from_policy::args_t a{};
    a.dest = dest;
    return detail::fd_sender<detail::recv_from_policy>{&ctx, h.datagram_handle(), a};
}

}
