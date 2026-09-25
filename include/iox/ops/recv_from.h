// iox — ops/recv_from.h: io::recv_from(context, datagram handle, wbytes) → (bytes, sender endpoint).
#pragma once

#include <sys/socket.h>

#include "iox/net/endpoint.h"
#include "iox/core/buffer.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct recv_from_args {
    wbytes destination{};
    iovec iov{};
    msghdr msg{};
    sockaddr_storage address{};
};

inline void prep_recv_from(io_uring_sqe* sqe, iox::fd fd, recv_from_args& args) noexcept {
    args.iov = iovec{args.destination.data(), args.destination.size()};
    args.msg = msghdr{};
    args.msg.msg_name = &args.address;
    args.msg.msg_namelen = sizeof(args.address);
    args.msg.msg_iov = &args.iov;
    args.msg.msg_iovlen = 1;
    ::io_uring_prep_recvmsg(sqe, fd.v, &args.msg, 0);
}

struct recv_from_complete {
    template <class Receiver, class Args>
    static void complete(Receiver& receiver, std::int32_t result, const Args& args) noexcept {
        if (result < 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_negative(result));
            return;
        }
        if (auto endpoint = net::endpoint::from_native(
                reinterpret_cast<const sockaddr*>(&args.address), args.msg.msg_namelen)) {
            stdexec::set_value(std::move(receiver), static_cast<std::size_t>(result), std::move(*endpoint));
        } else {
            stdexec::set_error(std::move(receiver), endpoint.error());
        }
    }
};

struct recv_from_policy : basic_policy<recv_from_args, prep_recv_from, recv_from_complete> {
    using signatures = io_signatures<std::size_t, net::endpoint>;
};

}

struct recv_from_tag {};
using recv_from_t = cpo<recv_from_tag>;
inline constexpr recv_from_t recv_from{};

template <class Handle>
requires datagram<std::remove_cvref_t<Handle>>
auto tag_invoke(recv_from_t, io_context& context, Handle&& handle, wbytes destination) noexcept {
    detail::recv_from_policy::args_t args{};
    args.destination = destination;
    return detail::fd_sender<detail::recv_from_policy>{&context, handle.datagram_handle(), args};
}

}
