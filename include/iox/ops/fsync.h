// iox — ops/fsync.h: io::fsync(context, handle) — accepts any readable or writable handle.
#pragma once

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct fsync_args {};

inline void prep_fsync(io_uring_sqe* sqe, iox::fd fd, const fsync_args&) noexcept {
    ::io_uring_prep_fsync(sqe, fd.v, 0);
}

struct fsync_policy : basic_policy<fsync_args, prep_fsync, void_complete> {
    using signatures = io_signatures<>;
};

}

struct fsync_tag {};
using fsync_t = cpo<fsync_tag>;
inline constexpr fsync_t fsync{};

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd> ||
         readable<std::remove_cvref_t<Handle>> || writable<std::remove_cvref_t<Handle>>
auto tag_invoke(fsync_t, io_context& context, Handle&& handle) noexcept {
    iox::fd fd{};
    if constexpr (std::same_as<std::remove_cvref_t<Handle>, iox::fd>) {
        fd = handle;
    } else if constexpr (readable<std::remove_cvref_t<Handle>>) {
        fd = detail::reader_fd(handle);
    } else {
        fd = detail::writer_fd(handle);
    }
    return detail::fd_sender<detail::fsync_policy>{&context, fd, {}};
}

}
