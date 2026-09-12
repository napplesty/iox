// iox — unified async IO for Linux
// include/iox/ops/fsync.h — (IORING_OP_FSYNC). Accepts any readable or writable handle.
#pragma once

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct fsync_policy {
    struct args_t {};
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, const args_t&) noexcept {
        ::io_uring_prep_fsync(sqe, f.v, 0);
    }
    using complete = void_complete;
};

}

inline constexpr struct fsync_t {
    template <class H>
    requires tag_invocable<fsync_t, io_context&, H>
    auto operator()(io_context& ctx, H&& h) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h))))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h))) {
        return tag_invoke(*this, ctx, std::forward<H>(h));
    }
} fsync{};

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> ||
         readable<std::remove_cvref_t<H>> || writable<std::remove_cvref_t<H>>
auto tag_invoke(fsync_t, io_context& ctx, H&& h) noexcept {
    iox::fd f{};
    if constexpr (std::same_as<std::remove_cvref_t<H>, iox::fd>) {
        f = h;
    } else if constexpr (readable<std::remove_cvref_t<H>>) {
        f = detail::reader_fd(h);
    } else {
        f = detail::writer_fd(h);
    }
    return detail::fd_sender<detail::fsync_policy>{&ctx, f, {}};
}

}
