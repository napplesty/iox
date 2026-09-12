// iox — unified async IO for Linux
// net/tcp.h — TCP acceptor and stream socket.
//
// All handles create sockets O_NONBLOCK (io_uring punts blocking sockets to
// kernel workers — the fast path needs non-blocking fds) and O_CLOEXEC.
// The vocabulary drives them: io::accept (acceptor), io::connect (socket),
// io::read/write/close (socket).
#pragma once

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <expected>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/net/endpoint.h"

namespace iox::net::tcp {

class socket;

class acceptor {
public:
    using socket_type = socket;

    /// Bind to `ep` and listen. Fails with EADDRINUSE etc. as typed errors.
    static std::expected<acceptor, error> listen(const endpoint& ep, int backlog = 128) noexcept {
        const int raw = ::socket(ep.family() == endpoint::family_t::ipv6 ? AF_INET6 : AF_INET,
                                 SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        int one = 1;
        (void)::setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

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

    // capability: acceptable (io::accept)
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
    friend class socket;

    iox::fd fd_{};
};

class socket {
public:
    struct adopt_fd_t {}; // internal: adopt an fd handed over by io::accept

    /// Create an unconnected socket for io::connect.
    static std::expected<socket, error> unconnected(endpoint::family_t fam) noexcept {
        const int af = fam == endpoint::family_t::ipv6 ? AF_INET6 : AF_INET;
        const int raw = ::socket(af, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
    explicit operator bool() const noexcept { return valid(); }

    /// Adopt a connected fd (io::accept hands these out). Public but internal
    /// by convention — the tag type makes accidental adoption visible.
    socket(adopt_fd_t, int raw) noexcept : fd_(raw) {}

    // capabilities: readable + writable + connectable + closable
    iox::fd read_handle() const noexcept { return fd_; }
    iox::fd write_handle() const noexcept { return fd_; }
    iox::fd connect_handle() const noexcept { return fd_; }
    // sockets are message-based: io::write uses SEND|MSG_NOSIGNAL (no SIGPIPE)
    static constexpr bool message_based = true;

    iox::fd* fd_slot() noexcept { return &fd_; }

    /// Best-effort peer address (typed error if not connected).
    std::expected<endpoint, error> peer() const noexcept {
        sockaddr_storage ss{};
        socklen_t length = sizeof(ss);
        if (::getpeername(fd_.v, reinterpret_cast<sockaddr*>(&ss), &length) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return endpoint::from_native(reinterpret_cast<const sockaddr*>(&ss), length);
    }

    void reset() noexcept {
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    iox::fd fd_{};
};

static_assert(!io::readable<acceptor> && !io::writable<acceptor>);
static_assert(io::readable<socket> && io::writable<socket> && !io::seekable<socket>);

} // namespace iox::net::tcp
