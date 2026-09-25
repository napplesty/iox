// iox — ops/read.h: io::read(context, handle, wbytes | registered_buffer).
#pragma once

#include "iox/core/buffer.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct read_args {
    wbytes destination{};
};
struct read_fixed_args {
    registered_buffer buffer{};
};

inline void prep_read(io_uring_sqe* sqe, iox::fd fd, read_args& args) noexcept {
    ::io_uring_prep_read(sqe, fd.v, args.destination.data(), args.destination.size(), -1);
}
inline void prep_read_fixed(io_uring_sqe* sqe, iox::fd fd, read_fixed_args& args) noexcept {
    ::io_uring_prep_read_fixed(sqe, fd.v, args.buffer.data, args.buffer.size, -1,
                               static_cast<int>(args.buffer.index));
}

using read_policy = basic_policy<read_args, prep_read>;
using read_fixed_policy = basic_policy<read_fixed_args, prep_read_fixed>;

}

struct read_tag {};
using read_t = cpo<read_tag>;
inline constexpr read_t read{};

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd> || readable<std::remove_cvref_t<Handle>>
auto tag_invoke(read_t, io_context& context, Handle&& handle, wbytes destination) noexcept {
    return detail::fd_sender<detail::read_policy>{&context, detail::reader_fd(handle), {destination}};
}

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd> || readable<std::remove_cvref_t<Handle>>
auto tag_invoke(read_t, io_context& context, Handle&& handle, registered_buffer& buffer) noexcept {
    return detail::fd_sender<detail::read_fixed_policy>{&context, detail::reader_fd(handle), {buffer}};
}

}
