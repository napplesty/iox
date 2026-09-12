// io::read — stream-position read. Plain wbytes takes IORING_OP_READ; a
// registered_buffer takes IORING_OP_READ_FIXED (zero-copy) — path choice is
// type-driven.
#pragma once

#include "iox/core/buffer.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct read_policy {
    struct args_t {
        wbytes dest{};
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_read(sqe, f.v, a.dest.data(), a.dest.size(), -1);
    }
    using complete = transfer_complete;
};

struct read_fixed_policy { // registered-buffer zero-copy read
    struct args_t {
        registered_buffer buffer{};
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_read_fixed(sqe, f.v, a.buffer.data, a.buffer.size, -1,
                                   static_cast<int>(a.buffer.index));
    }
    using complete = transfer_complete;
};


} // namespace detail

inline constexpr struct read_t {
    /// Stream-position read into a plain writable view. Customization point:
    /// drivers provide `tag_invoke(read_t, ctx, handle, wbytes)` for their
    /// own handle types; the fd default below serves fd-backed handles.
    template <class H>
    requires tag_invocable<read_t, io_context&, H, wbytes>
    auto operator()(io_context& ctx, H&& h, wbytes dest) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), dest)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), dest)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), dest);
    }

    /// Registered-buffer read: selects IORING_OP_READ_FIXED (zero-copy).
    template <class H>
    requires tag_invocable<read_t, io_context&, H, registered_buffer&>
    auto operator()(io_context& ctx, H&& h, registered_buffer& buffer) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), buffer)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), buffer)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), buffer);
    }
} read{};

// ---- fd driver defaults ----------------------------------------------------

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> || readable<std::remove_cvref_t<H>>
auto tag_invoke(read_t, io_context& ctx, H&& h, wbytes dest) noexcept {
    return detail::fd_sender<detail::read_policy>{&ctx, detail::reader_fd(h), {dest}};
}

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> || readable<std::remove_cvref_t<H>>
auto tag_invoke(read_t, io_context& ctx, H&& h, registered_buffer& buffer) noexcept {
    return detail::fd_sender<detail::read_fixed_policy>{&ctx, detail::reader_fd(h), {buffer}};
}

} // namespace iox::io
