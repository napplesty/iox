// iox — unified async IO for Linux
// include/iox/core/concepts.h — capability concepts (design §四.①).
#pragma once

#include <concepts>

#include "iox/core/fd.h"

namespace iox::io {

template <class Handle>
concept readable = requires(const Handle& handle) {
    { handle.read_handle() } -> std::same_as<iox::fd>;
};

template <class Handle>
concept writable = requires(const Handle& handle) {
    { handle.write_handle() } -> std::same_as<iox::fd>;
};

template <class Handle>
concept seekable = requires(const Handle& handle) {
    { handle.is_seekable() } -> std::convertible_to<bool>;
};

template <class Handle>
concept read_seekable = readable<Handle> && seekable<Handle>;
template <class Handle>
concept write_seekable = writable<Handle> && seekable<Handle>;

template <class Handle>
concept acceptable = requires(const Handle& handle) {
    { handle.accept_handle() } -> std::same_as<iox::fd>;
} && requires {
    typename Handle::socket_type;
    typename Handle::socket_type::adopt_fd_t;
};

template <class Handle>
concept connectable = requires(const Handle& handle) {
    { handle.connect_handle() } -> std::same_as<iox::fd>;
};

template <class Handle>
concept datagram = requires(const Handle& handle) {
    { handle.datagram_handle() } -> std::same_as<iox::fd>;
};

}
