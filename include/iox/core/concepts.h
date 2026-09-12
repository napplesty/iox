// iox — unified async IO for Linux
// include/iox/core/concepts.h — capability concepts (design §四.①).
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

template <class H>
concept acceptable = requires(const H& h) {
    { h.accept_handle() } -> std::same_as<iox::fd>;
} && requires {
    typename H::socket_type;
    typename H::socket_type::adopt_fd_t;
};

template <class H>
concept connectable = requires(const H& h) {
    { h.connect_handle() } -> std::same_as<iox::fd>;
};

template <class H>
concept datagram = requires(const H& h) {
    { h.datagram_handle() } -> std::same_as<iox::fd>;
};

}
