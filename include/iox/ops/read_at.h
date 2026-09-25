// iox — ops/read_at.h: io::read_at(context, handle, wbytes, uoffset_t); the strong offset type rejects size/offset mixups.
#pragma once

#include "iox/core/buffer.h"
#include "iox/core/units.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct read_at_args {
    wbytes destination{};
    std::int64_t offset = 0;
};

inline void prep_read_at(io_uring_sqe* sqe, iox::fd fd, read_at_args& args) noexcept {
    ::io_uring_prep_read(sqe, fd.v, args.destination.data(), args.destination.size(), args.offset);
}

struct read_at_policy : basic_policy<read_at_args, prep_read_at> {
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

struct read_at_tag {};
using read_at_t = cpo<read_at_tag>;
inline constexpr read_at_t read_at{};

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd> || read_seekable<std::remove_cvref_t<Handle>>
auto tag_invoke(read_at_t, io_context& context, Handle&& handle, wbytes destination, uoffset_t offset) noexcept {
    return detail::fd_sender<detail::read_at_policy>{
        &context, detail::reader_fd(handle), {destination, static_cast<std::int64_t>(offset.v)}};
}

}
