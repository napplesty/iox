// iox — ops/send_to.h: io::send_to(context, datagram handle, rbytes, endpoint).
#pragma once

#include <sys/socket.h>

#include <cstring>

#include "iox/net/endpoint.h"
#include "iox/core/buffer.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct send_to_args {
    sockaddr_storage address{};
    socklen_t length = 0;
    rbytes source{};
    iovec iov{};
    msghdr msg{};
};

inline void prep_send_to(io_uring_sqe* sqe, iox::fd fd, send_to_args& args) noexcept {
    args.iov = iovec{const_cast<void*>(static_cast<const void*>(args.source.data())), args.source.size()};
    args.msg = msghdr{};
    args.msg.msg_name = &args.address;
    args.msg.msg_namelen = args.length;
    args.msg.msg_iov = &args.iov;
    args.msg.msg_iovlen = 1;
    ::io_uring_prep_sendmsg(sqe, fd.v, &args.msg, 0);
}

using send_to_policy = basic_policy<send_to_args, prep_send_to>;

}

struct send_to_tag {};
using send_to_t = cpo<send_to_tag>;
inline constexpr send_to_t send_to{};

template <class Handle>
requires datagram<std::remove_cvref_t<Handle>>
auto tag_invoke(send_to_t, io_context& context, Handle&& handle, rbytes source,
                const net::endpoint& endpoint) noexcept {
    detail::send_to_policy::args_t args{};
    std::memcpy(&args.address, endpoint.data(), endpoint.size());
    args.length = endpoint.size();
    args.source = source;
    return detail::fd_sender<detail::send_to_policy>{&context, handle.datagram_handle(), args};
}

}
