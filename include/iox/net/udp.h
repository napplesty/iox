// iox — unified async IO for Linux
// net/udp.h — UDP datagram socket.
//
// Capabilities: readable + writable (io::read/write on a connected socket)
// plus datagram ops io::send_to / io::recv_from (connectionless use).
#pragma once

#include <sys/socket.h>
#include <unistd.h>

#include <expected>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/net/endpoint.h"

namespace iox::net::udp {

class socket {
public:
    /// Open a UDP socket bound to `local` (use endpoint::ipv4_any(port) for a
    /// plain receive socket; pass any endpoint for an ephemeral local port).
    static std::expected<socket, error> open(const endpoint& local) noexcept {
        const int af = local.family() == endpoint::family_t::ipv6 ? AF_INET6 : AF_INET;
        const int raw = ::socket(af, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (raw < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        if (::bind(raw, local.data(), local.size()) != 0) {
            const int e = errno;
            ::close(raw);
            return std::unexpected(error::from_errno(e));
        }
        return socket{raw};
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

    // capabilities: readable + writable + datagram + closable
    iox::fd read_handle() const noexcept { return fd_; }
    iox::fd write_handle() const noexcept { return fd_; }
    iox::fd datagram_handle() const noexcept { return fd_; }
    // sockets are message-based: io::write uses SEND|MSG_NOSIGNAL (no SIGPIPE)
    static constexpr bool message_based = true;

    iox::fd* fd_slot() noexcept { return &fd_; }

    std::expected<endpoint, error> local() const noexcept {
        sockaddr_storage ss{};
        socklen_t length = sizeof(ss);
        if (::getsockname(fd_.v, reinterpret_cast<sockaddr*>(&ss), &length) != 0) {
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
    explicit socket(int raw) noexcept : fd_(raw) {}
    iox::fd fd_{};
};

static_assert(io::readable<socket> && io::writable<socket> && !io::seekable<socket>);

} // namespace iox::net::udp
