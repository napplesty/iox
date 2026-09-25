// iox — ops/poll.h: io::poll(context, fd, events) → revents bitmask.
#pragma once

#include <poll.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct poll_args {
    short events = 0;
};

inline void prep_poll(io_uring_sqe* sqe, iox::fd fd, poll_args& args) noexcept {
    ::io_uring_prep_poll_add(sqe, fd.v, args.events);
}

struct poll_policy : basic_policy<poll_args, prep_poll, revents_complete> {
    using signatures = io_signatures<std::uint32_t>;
};

}

struct poll_tag {};
using poll_t = cpo<poll_tag>;
inline constexpr poll_t poll{};

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd>
auto tag_invoke(poll_t, io_context& context, Handle&& fd, short events) noexcept {
    return detail::fd_sender<detail::poll_policy>{&context, fd, {events}};
}

}
