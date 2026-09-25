// iox — ops/close.h: io::close(context, handle); an lvalue handle's fd slot is cleared when the close completes.
#pragma once

#include <type_traits>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct close_args {
    iox::fd* slot = nullptr;
};

inline void prep_close(io_uring_sqe* sqe, iox::fd fd, const close_args&) noexcept {
    ::io_uring_prep_close(sqe, fd.v);
}

struct close_policy : basic_policy<close_args, prep_close, void_complete> {
    using signatures = io_signatures<>;
    template <class Operation>
    static void after(Operation& operation, std::int32_t) noexcept {
        if (operation.args.slot != nullptr) {
            *operation.args.slot = iox::fd{};
        }
    }
};

template <class Handle>
concept closable_handle = requires(Handle& handle) {
    { handle.fd_slot() } -> std::same_as<iox::fd*>;
};

}

struct close_tag {};
using close_t = cpo<close_tag>;
inline constexpr close_t close{};

template <class Handle>
requires detail::closable_handle<Handle>
auto tag_invoke(close_t, io_context& context, Handle& handle) noexcept {
    return detail::fd_sender<detail::close_policy>{&context, *handle.fd_slot(), {handle.fd_slot()}};
}

template <class Handle>
requires detail::closable_handle<Handle> && std::is_rvalue_reference_v<Handle&&>
auto tag_invoke(close_t, io_context& context, Handle&& handle) noexcept {
    const iox::fd stolen = *handle.fd_slot();
    *handle.fd_slot() = iox::fd{};
    return detail::fd_sender<detail::close_policy>{&context, stolen, {nullptr}};
}

}
