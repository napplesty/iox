// iox — ops/splice.h: io::splice(context, in, out, length, [offset_in], [offset_out], flags); one of the two fds must be a pipe.
#pragma once

#include <fcntl.h>

#include <optional>

#include "iox/core/units.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct splice_args {
    int fd_in = -1;
    std::size_t length = 0;
    std::int64_t offset_in = -1;
    std::int64_t offset_out = -1;
    unsigned flags = 0;
};

inline void prep_splice(io_uring_sqe* sqe, iox::fd fd, splice_args& args) noexcept {
    ::io_uring_prep_splice(sqe, args.fd_in, args.offset_in, fd.v, args.offset_out,
                           static_cast<unsigned>(args.length), args.flags);
}

struct splice_policy : basic_policy<splice_args, prep_splice> {
    template <class Receiver>
    static bool immediate(Receiver& receiver, args_t& args) noexcept {
        if (args.length == 0) {
            stdexec::set_value(std::move(receiver), std::size_t{0});
            return true;
        }
        if (args.length > 0xFFFFFFFFULL) {
            stdexec::set_error(std::move(receiver), iox::error::from_errno(EOVERFLOW)); // never silently move less
            return true;
        }
        return false;
    }
};

}

inline constexpr struct splice_t {
    template <class In, class Out>
    requires tag_invocable<splice_t, io_context&, In, Out, std::size_t,
                                    std::optional<uoffset_t>, std::optional<uoffset_t>, unsigned>
    auto operator()(io_context& context, In&& in, Out&& out, std::size_t length,
                    std::optional<uoffset_t> offset_in = {}, std::optional<uoffset_t> offset_out = {},
                    unsigned flags = 0) const
        noexcept(noexcept(tag_invoke(*this, context, std::forward<In>(in), std::forward<Out>(out),
                                              length, offset_in, offset_out, flags)))
        -> decltype(tag_invoke(*this, context, std::forward<In>(in), std::forward<Out>(out),
                                        length, offset_in, offset_out, flags)) {
        return tag_invoke(*this, context, std::forward<In>(in), std::forward<Out>(out),
                                   length, offset_in, offset_out, flags);
    }
} splice{};

template <class In, class Out>
requires std::same_as<std::remove_cvref_t<In>, iox::fd> &&
         std::same_as<std::remove_cvref_t<Out>, iox::fd>
auto tag_invoke(splice_t, io_context& context, In&& in, Out&& out, std::size_t length,
                std::optional<uoffset_t> offset_in, std::optional<uoffset_t> offset_out,
                unsigned flags) noexcept {
    const auto to_offset = [](std::optional<uoffset_t> offset) noexcept {
        return offset.has_value() ? static_cast<std::int64_t>(offset->v) : std::int64_t{-1};
    };
    return detail::fd_sender<detail::splice_policy>{
        &context, out, {in.v, length, to_offset(offset_in), to_offset(offset_out), flags}};
}

}
