// iox — unified async IO for Linux
// include/iox/ops/tee.h — the classic
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
        if (a.length > 0xFFFFFFFFULL) {
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

}

inline constexpr struct tee_t {
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

template <class In, class Out>
requires std::same_as<std::remove_cvref_t<In>, iox::fd> &&
         std::same_as<std::remove_cvref_t<Out>, iox::fd>
auto tag_invoke(tee_t, io_context& ctx, In&& in, Out&& out, std::size_t length, unsigned flags) noexcept {
    return detail::fd_sender<detail::tee_policy>{&ctx, out, {in.v, static_cast<unsigned>(length), flags}};
}

}
