// iox — ops/write_at.h: io::write_at(context, handle, rbytes, uoffset_t); the strong offset type rejects size/offset mixups.
#pragma once

#include "iox/core/buffer.h"
#include "iox/core/units.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct write_at_args {
    rbytes source{};
    std::int64_t offset = 0;
};

inline void prep_write_at(io_uring_sqe* sqe, iox::fd fd, write_at_args& args) noexcept {
    ::io_uring_prep_write(sqe, fd.v, args.source.data(), args.source.size(), args.offset);
}

struct write_at_policy : basic_policy<write_at_args, prep_write_at> {
    template <class Receiver>
    static bool immediate(Receiver& receiver, args_t& args) noexcept {
        if (args.offset < 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_errno(EOVERFLOW));
            return true;
        }
        return false;
    }
};

}

struct write_at_tag {};
using write_at_t = cpo<write_at_tag>;
inline constexpr write_at_t write_at{};

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd> || write_seekable<std::remove_cvref_t<Handle>>
auto tag_invoke(write_at_t, io_context& context, Handle&& handle, rbytes source, uoffset_t offset) noexcept {
    return detail::fd_sender<detail::write_at_policy>{
        &context, detail::writer_fd(handle), {source, static_cast<std::int64_t>(offset.v)}};
}

}
