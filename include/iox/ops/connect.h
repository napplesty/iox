// iox — ops/connect.h: io::connect(context, socket, endpoint) — the socket must be unconnected.
#pragma once

#include <sys/socket.h>

#include <cstring>

#include "iox/net/endpoint.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct connect_args {
    sockaddr_storage address{};
    socklen_t length = 0;
};

inline void prep_connect(io_uring_sqe* sqe, iox::fd fd, connect_args& args) noexcept {
    ::io_uring_prep_connect(sqe, fd.v, reinterpret_cast<const sockaddr*>(&args.address), args.length);
}

struct connect_policy : basic_policy<connect_args, prep_connect, void_complete> {
    using signatures = io_signatures<>;
};

}

struct connect_tag {};
using connect_t = cpo<connect_tag>;
inline constexpr connect_t connect{};

template <class Handle>
requires connectable<std::remove_cvref_t<Handle>>
auto tag_invoke(connect_t, io_context& context, Handle&& handle, const net::endpoint& endpoint) noexcept {
    detail::connect_policy::args_t args{};
    std::memcpy(&args.address, endpoint.data(), endpoint.size());
    args.length = endpoint.size();
    return detail::fd_sender<detail::connect_policy>{&context, handle.connect_handle(), args};
}

}
