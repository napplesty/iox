// iox — unified async IO for Linux
// net/unix.h — Unix domain stream sockets.
//
// Same vocabulary as TCP (io::accept / io::connect / io::read / io::write);
// endpoints are filesystem paths or abstract names (endpoint::unix).
// socketpair() yields a typed bidirectional pair for the conformance suite
// and for local IPC.
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <expected>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/net/endpoint.h"

namespace iox::net::unix_dom {

class socket;

class acceptor {
public:
    using socket_type = socket;

    static std::expected<acceptor, error> listen(const endpoint& ep, int backlog = 128) noexcept {
        if (ep.family() != endpoint::family::unix_path) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        // Path sockets linger in the filesystem: unlink a stale entry first.
        if (ep.data()->sa_family == AF_UNIX &&
            reinterpret_cast<const sockaddr_un*>(ep.data())->sun_path[0] != '\0') {
            ::unlink(reinterpret_cast<const sockaddr_un*>(ep.data())->sun_path);
        }
        const int raw = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        if (::bind(raw, ep.data(), ep.size()) != 0 || ::listen(raw, backlog) != 0) {
            const int e = errno;
            ::close(raw);
            return std::unexpected(error::from_errno(e));
        }
        return acceptor{raw};
    }

    acceptor() noexcept = default;
    ~acceptor() { reset(); }
    acceptor(acceptor&& other) noexcept : fd_(std::exchange(other.fd_, iox::fd{})) {}
    acceptor& operator=(acceptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, iox::fd{});
        }
        return *this;
    }
    acceptor(const acceptor&) = delete;
    acceptor& operator=(const acceptor&) = delete;

    bool valid() const noexcept { return fd_.valid(); }

    iox::fd accept_handle() const noexcept { return fd_; }
    // sockets are message-based: io::write uses SEND|MSG_NOSIGNAL (no SIGPIPE)
    static constexpr bool message_based = true;

    iox::fd* fd_slot() noexcept { return &fd_; }

    void reset() noexcept {
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    explicit acceptor(int raw) noexcept : fd_(raw) {}
    iox::fd fd_{};
};

class socket {
public:
    struct adopt_fd_t {};

    static std::expected<socket, error> unconnected() noexcept {
        const int raw = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return socket{adopt_fd_t{}, raw};
    }

    socket() noexcept = default;
    ~socket() { reset(); }
    socket(socket&& other) noexcept : fd_(std::exchange(other.fd_, iox::fd{})) {}
    socket& operator=(socket&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, iox::fd{});
        }
        return *this;
    }
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    bool valid() const noexcept { return fd_.valid(); }

    socket(adopt_fd_t, int raw) noexcept : fd_(raw) {}

    iox::fd read_handle() const noexcept { return fd_; }
    iox::fd write_handle() const noexcept { return fd_; }
    iox::fd connect_handle() const noexcept { return fd_; }
    // sockets are message-based: io::write uses SEND|MSG_NOSIGNAL (no SIGPIPE)
    static constexpr bool message_based = true;

    iox::fd* fd_slot() noexcept { return &fd_; }

    void reset() noexcept {
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    iox::fd fd_{};
};

/// Connected pair over Unix domain sockets — the typed replacement for
/// socketpair(2) raw fds. Both ends readable and writable.
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

} // namespace iox::net::unix_dom
