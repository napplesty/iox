// iox — net/udp.h: UDP datagram socket.
#pragma once

#include <sys/socket.h>

#include <expected>
#include <type_traits>
#include <utility>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/net/detail.h"
#include "iox/net/endpoint.h"

namespace iox::net::udp {

class socket {
public:
    static std::expected<socket, error> open(const endpoint& local) noexcept {
        auto socket_fd = detail::make_socket(detail::address_family_of(local.family()), SOCK_DGRAM);
        if (!socket_fd) {
            return std::unexpected(socket_fd.error());
        }
        if (::bind(socket_fd->get().v, local.data(), local.size()) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return socket{std::move(*socket_fd)};
    }

    socket() noexcept = default;
    bool valid() const noexcept { return fd_.valid(); }

    iox::fd read_handle() const noexcept { return fd_.get(); }
    iox::fd write_handle() const noexcept { return fd_.get(); }
    iox::fd datagram_handle() const noexcept { return fd_.get(); }
    static constexpr bool message_based = true;
    iox::fd* fd_slot() noexcept { return fd_.slot(); }

    std::expected<endpoint, error> local() const noexcept {
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        if (::getsockname(fd_.get().v, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return endpoint::from_native(reinterpret_cast<const sockaddr*>(&storage), length);
    }

    void reset() noexcept { fd_.reset(); }

private:
    explicit socket(iox::unique_fd fd) noexcept : fd_(std::move(fd)) {}

    iox::unique_fd fd_{};
};

static_assert(io::readable<socket> && io::writable<socket> && !io::seekable<socket>);
static_assert(std::is_nothrow_move_constructible_v<socket> &&
              !std::is_copy_constructible_v<socket>);

}
