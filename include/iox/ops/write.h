// io::write — stream-position write. Plain rbytes takes WRITE (files/pipes)
// or SEND|MSG_NOSIGNAL (message-based handles, see below); a
// registered_buffer takes IORING_OP_WRITE_FIXED (zero-copy).
#pragma once

#include <sys/socket.h>
#include <type_traits>

#include "iox/core/buffer.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

// io::write on message-based handles (sockets) uses SEND|MSG_NOSIGNAL: a
// plain WRITE to a socket raises SIGPIPE when the peer has closed — a
// classic way to lose a whole server to one dead client. The choice is a
// bool in the args (not a template parameter) so every writable handle maps
// to the SAME sender type — generic code (pumps, proxies, conformance
// suites) can dispatch over handle variants uniformly.
struct write_policy {
    struct args_t {
        rbytes src{};
        bool use_send = false;
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    // A zero-length write has nothing to transfer: complete immediately with
    // 0 and never touch the kernel. (Otherwise SEND(0) on a peer-closed
    // socket returns EPIPE — an error for an operation that does nothing.)
    // The EOF idiom read→write(n) terminates loops cleanly because of this.
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (a.src.size() == 0) {
            stdexec::set_value(std::move(r), std::size_t{0});
            return true;
        }
        return false;
    }
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        if (a.use_send) {
            ::io_uring_prep_send(sqe, f.v, a.src.data(), a.src.size(), MSG_NOSIGNAL);
        } else {
            ::io_uring_prep_write(sqe, f.v, a.src.data(), a.src.size(), -1);
        }
    }
    using complete = transfer_complete;
};

struct write_fixed_policy {
    struct args_t {
        registered_buffer buffer{};
    };
    // zero-length guard parity with the rbytes overload (design F4): a
    // nothing-write is a value 0, never a kernel EPIPE round trip
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (a.buffer.size == 0) {
            stdexec::set_value(std::move(r), std::size_t{0});
            return true;
        }
        return false;
    }
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_write_fixed(sqe, f.v, a.buffer.data, a.buffer.size, -1,
                                    static_cast<int>(a.buffer.index));
    }
    using complete = transfer_complete;
};


// void_t detection (GCC 15 hard-errors on qualified dependent member lookup
// inside requires-expression bodies — same quirk noted in test_capabilities).
template <class T, class = void>
struct has_message_based : std::false_type {};
template <class T>
struct has_message_based<T, std::void_t<decltype(T::message_based)>> : std::true_type {};

template <class H>
constexpr bool message_based_value() noexcept {
    if constexpr (has_message_based<std::remove_cvref_t<H>>::value) {
        return std::remove_cvref_t<H>::message_based;
    } else {
        return false;
    }
}

template <class H>
inline constexpr bool is_message_based = message_based_value<H>();

} // namespace detail

inline constexpr struct write_t {
    /// Stream-position write. Customization point: drivers provide
    /// `tag_invoke(write_t, ctx, handle, rbytes)`; the fd default below
    /// serves fd-backed handles.
    template <class H>
    requires tag_invocable<write_t, io_context&, H, rbytes>
    auto operator()(io_context& ctx, H&& h, rbytes src) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), src)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), src)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), src);
    }

    /// Registered-buffer write: selects IORING_OP_WRITE_FIXED (zero-copy).
    /// Customization point: `tag_invoke(write_t, ctx, handle, registered_buffer&)`.
    template <class H>
    requires tag_invocable<write_t, io_context&, H, registered_buffer&>
    auto operator()(io_context& ctx, H&& h, registered_buffer& buffer) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), buffer)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), buffer)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), buffer);
    }
} write{};

// ---- fd driver defaults ----------------------------------------------------

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> || writable<std::remove_cvref_t<H>>
auto tag_invoke(write_t, io_context& ctx, H&& h, rbytes src) noexcept {
    return detail::fd_sender<detail::write_policy>{
        &ctx, detail::writer_fd(h), {src, detail::is_message_based<H>}};
}

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> || writable<std::remove_cvref_t<H>>
auto tag_invoke(write_t, io_context& ctx, H&& h, registered_buffer& buffer) noexcept {
    return detail::fd_sender<detail::write_fixed_policy>{&ctx, detail::writer_fd(h), {buffer}};
}

} // namespace iox::io
