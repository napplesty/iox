// iox — unified async IO for Linux
// concepts.h — capability concepts (design §四.①).
//
// A handle declares its capabilities structurally:
//   * readable<H>  — exposes read_handle() → iox::fd
//   * writable<H>  — exposes write_handle() → iox::fd
//   * seekable<H>  — supports positional io (io::read_at / write_at)
// Calling an operation a handle does not support is a compile error, not a
// runtime surprise. Pipe ends, for example, expose exactly one direction.
#pragma once

#include <concepts>

#include "iox/core/fd.h"

namespace iox::io {

template <class H>
concept readable = requires(const H& h) {
    { h.read_handle() } -> std::same_as<iox::fd>;
};

template <class H>
concept writable = requires(const H& h) {
    { h.write_handle() } -> std::same_as<iox::fd>;
};

template <class H>
concept seekable = requires(const H& h) {
    { h.is_seekable() } -> std::convertible_to<bool>;
};

template <class H>
concept read_seekable = readable<H> && seekable<H>;
template <class H>
concept write_seekable = writable<H> && seekable<H>;

/// Acceptors: io::accept(ctx, h) produces h::socket_type.
template <class H>
concept acceptable = requires(const H& h) {
    { h.accept_handle() } -> std::same_as<iox::fd>;
} && requires {
    typename H::socket_type;
    typename H::socket_type::adopt_fd_t;
};

/// Sockets that can initiate a connection: io::connect(ctx, h, endpoint).
template <class H>
concept connectable = requires(const H& h) {
    { h.connect_handle() } -> std::same_as<iox::fd>;
};

/// Datagram sockets: io::send_to / io::recv_from.
template <class H>
concept datagram = requires(const H& h) {
    { h.datagram_handle() } -> std::same_as<iox::fd>;
};

} // namespace iox::io
