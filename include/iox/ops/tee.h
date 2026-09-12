// io::tee — duplicate bytes INSIDE two pipes (IORING_OP_TEE): pipe `in`'s
// data is copied into pipe `out` without consuming it — the classic
// multicast/tap primitive. Both fds must be pipes; count = bytes duplicated.
#pragma once

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct tee_policy {
    struct args_t {
        int fd_in = -1;
        unsigned length = 0;
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
        if (a.length > 0xFFFFFFFFULL) { // size_t -> unsigned truncation (design F3)
            stdexec::set_error(std::move(r), iox::error::from_errno(EOVERFLOW));
            return true;
        }
        return false;
    }
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_tee(sqe, a.fd_in, f.v, a.length, a.flags);
    }
    using complete = transfer_complete;
};


} // namespace detail

inline constexpr struct tee_t {
    /// Customization point: drivers provide the full 5-parameter
    /// `tag_invoke(tee_t, ctx, in, out, length, flags)`; the fd default below
    /// serves raw pipe descriptors.
    template <class In, class Out>
    requires tag_invocable<tee_t, io_context&, In, Out, std::size_t, unsigned>
    auto operator()(io_context& ctx, In&& in, Out&& out, std::size_t length, unsigned flags = 0) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<In>(in), std::forward<Out>(out),
                                              length, flags)))
        -> decltype(tag_invoke(*this, ctx, std::forward<In>(in), std::forward<Out>(out),
                                        length, flags)) {
        return tag_invoke(*this, ctx, std::forward<In>(in), std::forward<Out>(out),
                                   length, flags);
    }
} tee{};

// ---- fd driver default -----------------------------------------------------

template <class In, class Out>
requires std::same_as<std::remove_cvref_t<In>, iox::fd> &&
         std::same_as<std::remove_cvref_t<Out>, iox::fd>
auto tag_invoke(tee_t, io_context& ctx, In&& in, Out&& out, std::size_t length, unsigned flags) noexcept {
    return detail::fd_sender<detail::tee_policy>{&ctx, out, {in.v, static_cast<unsigned>(length), flags}};
}

} // namespace iox::io
