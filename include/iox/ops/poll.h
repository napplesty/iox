// io::poll — readiness wait (IORING_OP_POLL_ADD); completes with the
// revents bitmask. The wake-up seam blocking_pool and external completion
// sources build on.
#pragma once

#include <poll.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct poll_policy {
    struct args_t {
        short events = 0;
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::uint32_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_poll_add(sqe, f.v, a.events);
    }
    using complete = revents_complete;
};


} // namespace detail

inline constexpr struct poll_t {
    /// Customization point: drivers provide `tag_invoke(poll_t, ctx,
    /// handle, events)` for their own handle types; the fd default below
    /// serves raw descriptors.
    template <class H>
    requires tag_invocable<poll_t, io_context&, H, short>
    auto operator()(io_context& ctx, H&& h, short events) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), events)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), events)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), events);
    }
} poll{};

// ---- fd driver default -----------------------------------------------------

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd>
auto tag_invoke(poll_t, io_context& ctx, H&& h, short events) noexcept {
    return detail::fd_sender<detail::poll_policy>{&ctx, h, {events}};
}

} // namespace iox::io
