// iox — ops/tee.h: io::tee(context, in_pipe, out_pipe, length, flags) — the classic pipe-to-pipe duplicate.
#pragma once

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct tee_args {
    int fd_in = -1;
    std::size_t length = 0;
    unsigned flags = 0;
};

inline void prep_tee(io_uring_sqe* sqe, iox::fd fd, tee_args& args) noexcept {
    ::io_uring_prep_tee(sqe, args.fd_in, fd.v, static_cast<unsigned>(args.length), args.flags);
}

struct tee_policy : basic_policy<tee_args, prep_tee> {
    template <class Receiver>
    static bool immediate(Receiver& receiver, args_t& args) noexcept {
        if (args.length == 0) {
            stdexec::set_value(std::move(receiver), std::size_t{0});
            return true;
        }
        if (args.length > 0xFFFFFFFFULL) {
            stdexec::set_error(std::move(receiver), iox::error::from_errno(EOVERFLOW));
            return true;
        }
        return false;
    }
};

}

inline constexpr struct tee_t {
    template <class In, class Out>
    requires tag_invocable<tee_t, io_context&, In, Out, std::size_t, unsigned>
    auto operator()(io_context& context, In&& in, Out&& out, std::size_t length, unsigned flags = 0) const
        noexcept(noexcept(tag_invoke(*this, context, std::forward<In>(in), std::forward<Out>(out),
                                              length, flags)))
        -> decltype(tag_invoke(*this, context, std::forward<In>(in), std::forward<Out>(out),
                                        length, flags)) {
        return tag_invoke(*this, context, std::forward<In>(in), std::forward<Out>(out),
                                   length, flags);
    }
} tee{};

template <class In, class Out>
requires std::same_as<std::remove_cvref_t<In>, iox::fd> &&
         std::same_as<std::remove_cvref_t<Out>, iox::fd>
auto tag_invoke(tee_t, io_context& context, In&& in, Out&& out, std::size_t length, unsigned flags) noexcept {
    return detail::fd_sender<detail::tee_policy>{&context, out, {in.v, length, flags}};
}

}
