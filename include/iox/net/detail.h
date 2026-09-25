// iox — net/detail.h: shared socket factories for the transport handles.
#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <expected>
#include <utility>

#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/net/endpoint.h"

namespace iox::net::detail {

constexpr int address_family_of(endpoint::family_t family) noexcept {
    return family == endpoint::family_t::ipv6 ? AF_INET6 : AF_INET;
}

inline std::expected<iox::unique_fd, error> make_socket(int family, int type) noexcept {
    const int raw_fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (raw_fd < 0) {
        return std::unexpected(error::from_errno(errno));
    }
    return iox::unique_fd{raw_fd};
}

inline std::expected<iox::unique_fd, error> bind_listen(iox::unique_fd socket_fd, const endpoint& local,
                                                        int backlog) noexcept {
    if (::bind(socket_fd.get().v, local.data(), local.size()) != 0 || ::listen(socket_fd.get().v, backlog) != 0) {
        return std::unexpected(error::from_errno(errno));
    }
    return std::move(socket_fd);
}

}
