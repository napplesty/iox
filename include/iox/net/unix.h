// iox — net/unix.h: Unix domain stream sockets.
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <expected>
#include <type_traits>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/net/detail.h"
#include "iox/net/endpoint.h"

namespace iox::net::unix_dom {

class socket;

class acceptor {
public:
    using socket_type = socket;

    static std::expected<acceptor, error> listen(const endpoint& local, int backlog = 128) noexcept {
        if (local.family() != endpoint::family::unix_path) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        if (local.data()->sa_family == AF_UNIX &&
            reinterpret_cast<const sockaddr_un*>(local.data())->sun_path[0] != '\0') {
            ::unlink(reinterpret_cast<const sockaddr_un*>(local.data())->sun_path);
        }
        auto socket_fd = detail::make_socket(AF_UNIX, SOCK_STREAM);
        if (!socket_fd) {
            return std::unexpected(socket_fd.error());
        }
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

    iox::unique_fd fd_{};
};

class socket {
public:
    struct adopt_fd_t {};

    static std::expected<socket, error> unconnected() noexcept {
        auto socket_fd = detail::make_socket(AF_UNIX, SOCK_STREAM);
        if (!socket_fd) {
            return std::unexpected(socket_fd.error());
        }
        return socket{adopt_fd_t{}, socket_fd->release().v};
    }

    socket() noexcept = default;
    socket(adopt_fd_t, int raw_fd) noexcept : fd_(raw_fd) {}

    bool valid() const noexcept { return fd_.valid(); }

    iox::fd read_handle() const noexcept { return fd_.get(); }
    iox::fd write_handle() const noexcept { return fd_.get(); }
    iox::fd connect_handle() const noexcept { return fd_.get(); }
    static constexpr bool message_based = true;
    iox::fd* fd_slot() noexcept { return fd_.slot(); }

    void reset() noexcept { fd_.reset(); }

private:
    iox::unique_fd fd_{};
};

struct pair {
    socket a;
    socket b;

    static std::expected<pair, error> create() noexcept {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return pair{socket{socket::adopt_fd_t{}, fds[0]}, socket{socket::adopt_fd_t{}, fds[1]}};
    }
};

static_assert(io::readable<socket> && io::writable<socket> && !io::seekable<socket>);
static_assert(std::is_nothrow_move_constructible_v<acceptor> &&
              !std::is_copy_constructible_v<acceptor>);
static_assert(std::is_nothrow_move_constructible_v<socket> &&
              !std::is_copy_constructible_v<socket>);

}
