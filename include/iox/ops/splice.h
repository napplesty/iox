// iox — unified async IO for Linux
// include/iox/ops/splice.h — (IORING_OP_SPLICE). One of the two fds must be a pipe; io::pump
#pragma once

#include <fcntl.h>

#include <optional>

#include "iox/core/units.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct splice_policy {
    struct args_t {
        int fd_in = -1;
        unsigned length = 0;
        std::int64_t off_in = -1;
        std::int64_t off_out = -1;
        unsigned flags = 0;
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (a.length == 0) {
            stdexec::set_value(std::move(r), std::size_t{0});
            return true;
        }
        if (a.length > 0xFFFFFFFFULL) {
            stdexec::set_error(std::move(r), iox::error::from_errno(EOVERFLOW)); // never silently move less
            return true;
        }
        return false;
    }
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_splice(sqe, a.fd_in, a.off_in, f.v, a.off_out, a.length, a.flags);
    }
    using complete = transfer_complete;
};

}

inline constexpr struct splice_t {
    template <class In, class Out>
    requires tag_invocable<splice_t, io_context&, In, Out, std::size_t,
                                    std::optional<uoffset_t>, std::optional<uoffset_t>, unsigned>
    auto operator()(io_context& ctx, In&& in, Out&& out, std::size_t length,
                    std::optional<uoffset_t> off_in = {}, std::optional<uoffset_t> off_out = {},
                    unsigned flags = 0) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<In>(in), std::forward<Out>(out),
                                              length, off_in, off_out, flags)))
        -> decltype(tag_invoke(*this, ctx, std::forward<In>(in), std::forward<Out>(out),
                                        length, off_in, off_out, flags)) {
        return tag_invoke(*this, ctx, std::forward<In>(in), std::forward<Out>(out),
                                   length, off_in, off_out, flags);
    }
} splice{};

template <class In, class Out>
requires std::same_as<std::remove_cvref_t<In>, iox::fd> &&
         std::same_as<std::remove_cvref_t<Out>, iox::fd>
auto tag_invoke(splice_t, io_context& ctx, In&& in, Out&& out, std::size_t length,
                std::optional<uoffset_t> off_in, std::optional<uoffset_t> off_out,
                unsigned flags) noexcept {
    const auto to_off = [](std::optional<uoffset_t> o) noexcept {
        return o.has_value() ? static_cast<std::int64_t>(o->v) : std::int64_t{-1};
    };
    return detail::fd_sender<detail::splice_policy>{
        &ctx, out, {in.v, static_cast<unsigned>(length), to_off(off_in), to_off(off_out), flags}};
}

}
