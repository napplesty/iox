// iox — ops/accept.h: io::accept(context, acceptor) → the acceptor's own socket type, adopted from the raw accepted fd.
#pragma once

#include <sys/socket.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

template <class Socket>
struct adopt_complete {
    template <class Receiver, class Args>
    static void complete(Receiver& receiver, std::int32_t result, const Args&) noexcept {
        if (result < 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_negative(result));
        } else {
            stdexec::set_value(std::move(receiver), Socket{typename Socket::adopt_fd_t{}, result});
        }
    }
};

struct accept_args {};

inline void prep_accept(io_uring_sqe* sqe, iox::fd fd, const accept_args&) noexcept {
    ::io_uring_prep_accept(sqe, fd.v, nullptr, nullptr, SOCK_CLOEXEC);
}

template <class Socket>
struct accept_policy : basic_policy<accept_args, prep_accept, adopt_complete<Socket>> {
    using signatures = io_signatures<Socket>;
};

}

struct accept_tag {};
using accept_t = cpo<accept_tag>;
inline constexpr accept_t accept{};

template <class Handle>
requires acceptable<std::remove_cvref_t<Handle>>
auto tag_invoke(accept_t, io_context& context, Handle&& handle) noexcept {
    using socket_type = typename std::remove_cvref_t<Handle>::socket_type;
    return detail::fd_sender<detail::accept_policy<socket_type>>{&context, handle.accept_handle(), {}};
}

}
