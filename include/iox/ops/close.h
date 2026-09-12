// iox — unified async IO for Linux
// include/iox/ops/close.h — syscall on the calling thread. The handle's fd slot is cleared when the
#pragma once

#include <type_traits>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct close_policy {
    struct args_t {
        iox::fd* slot = nullptr;
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, const args_t&) noexcept {
        ::io_uring_prep_close(sqe, f.v);
    }
    template <class Op>
    static void after(Op& o, std::int32_t) noexcept {
        if (o.args.slot != nullptr) {
            *o.args.slot = iox::fd{};
        }
    }
    using complete = void_complete;
};

template <class H>
concept closable_handle = requires(H& h) {
    { h.fd_slot() } -> std::same_as<iox::fd*>;
};

}

inline constexpr struct close_t {
    template <class H>
    requires tag_invocable<close_t, io_context&, H&>
    auto operator()(io_context& ctx, H& h) const
        noexcept(noexcept(tag_invoke(*this, ctx, h)))
        -> decltype(tag_invoke(*this, ctx, h)) {
        return tag_invoke(*this, ctx, h);
    }

    template <class H>
    requires std::is_rvalue_reference_v<H&&> &&
             tag_invocable<close_t, io_context&, H>
    auto operator()(io_context& ctx, H&& h) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h))))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h))) {
        return tag_invoke(*this, ctx, std::forward<H>(h));
    }
} close{};

template <class H>
requires detail::closable_handle<H>
auto tag_invoke(close_t, io_context& ctx, H& h) noexcept {
    return detail::fd_sender<detail::close_policy>{&ctx, *h.fd_slot(), {h.fd_slot()}};
}

template <class H>
requires detail::closable_handle<H> && std::is_rvalue_reference_v<H&&>
auto tag_invoke(close_t, io_context& ctx, H&& h) noexcept {
    const iox::fd stolen = *h.fd_slot();
    *h.fd_slot() = iox::fd{};
    return detail::fd_sender<detail::close_policy>{&ctx, stolen, {nullptr}};
}

}
