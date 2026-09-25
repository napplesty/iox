// iox — net/tcp.h: TCP acceptor and stream socket.
#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <expected>
#include <type_traits>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/net/detail.h"
#include "iox/net/endpoint.h"

namespace iox::net::tcp {

class socket;

class acceptor {
public:
    using socket_type = socket;

    static std::expected<acceptor, error> listen(const endpoint& local, int backlog = 128) noexcept {
        auto socket_fd = detail::make_socket(detail::address_family_of(local.family()), SOCK_STREAM);
        if (!socket_fd) {
            return std::unexpected(socket_fd.error());
        }
        int one = 1;
        (void)::setsockopt(socket_fd->get().v, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        auto bound = detail::bind_listen(std::move(*socket_fd), local, backlog);
        if (!bound) {
            return std::unexpected(bound.error());
        }
        return acceptor{std::move(*bound)};
    }

    acceptor() noexcept = default;
    bool valid() const noexcept { return fd_.valid(); }

    iox::fd accept_handle() const noexcept { return fd_.get(); }
    static constexpr bool message_based = true;
    iox::fd* fd_slot() noexcept { return fd_.slot(); }
    void reset() noexcept { fd_.reset(); }

private:
    explicit acceptor(iox::unique_fd fd) noexcept : fd_(std::move(fd)) {}
    friend class socket;

    iox::unique_fd fd_{};
};

class socket {
public:
    struct adopt_fd_t {};

    static std::expected<socket, error> unconnected(endpoint::family_t family) noexcept {
        auto socket_fd = detail::make_socket(detail::address_family_of(family), SOCK_STREAM);
        if (!socket_fd) {
            return std::unexpected(socket_fd.error());
        }
        return socket{adopt_fd_t{}, socket_fd->release().v};
    }

    socket() noexcept = default;
    socket(adopt_fd_t, int raw_fd) noexcept : fd_(raw_fd) {}

    bool valid() const noexcept { return fd_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    iox::fd read_handle() const noexcept { return fd_.get(); }
    iox::fd write_handle() const noexcept { return fd_.get(); }
    iox::fd connect_handle() const noexcept { return fd_.get(); }
    static constexpr bool message_based = true;
    iox::fd* fd_slot() noexcept { return fd_.slot(); }

    std::expected<endpoint, error> peer() const noexcept {
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        if (::getpeername(fd_.get().v, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return endpoint::from_native(reinterpret_cast<const sockaddr*>(&storage), length);
    }

    void reset() noexcept { fd_.reset(); }

private:
    iox::unique_fd fd_{};
};

static_assert(!io::readable<acceptor> && !io::writable<acceptor>);
static_assert(io::readable<socket> && io::writable<socket> && !io::seekable<socket>);
static_assert(std::is_nothrow_move_constructible_v<socket> &&
              !std::is_copy_constructible_v<socket>);

}
