// iox — ops/open.h: io::open(context, path, mode) → fs::file via IORING_OP_OPENAT.
#pragma once

#include <fcntl.h>

#include "iox/core/cpo.h"
#include "iox/core/buffer.h"
#include "iox/fs/file.h"
#include "iox/ops/fd_sender.h"

namespace iox::io {

namespace detail {

struct open_complete {
    template <class Receiver, class Args>
    static void complete(Receiver& receiver, std::int32_t result, const Args&) noexcept {
        if (result < 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_negative(result));
        } else {
            stdexec::set_value(std::move(receiver), fs::file{fs::file::adopt_fd_t{}, result});
        }
    }
};

struct open_args {
    const char* path = nullptr;
    int flags = 0;
};

inline void prep_open(io_uring_sqe* sqe, iox::fd, open_args& args) noexcept {
    ::io_uring_prep_openat(sqe, AT_FDCWD, args.path, args.flags | O_CLOEXEC, 0644);
}

struct open_policy : basic_policy<open_args, prep_open, open_complete> {
    using signatures = io_signatures<fs::file>;
};

}

struct open_tag {};
using open_t = cpo<open_tag>;
inline constexpr open_t open{};

inline auto tag_invoke(open_t, io_context& context, const char* path, fs::mode mode) noexcept {
    return detail::fd_sender<detail::open_policy>{
        &context, iox::fd{}, {path, static_cast<int>(mode)}};
}

}
