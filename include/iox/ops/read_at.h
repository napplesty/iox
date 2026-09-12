// io::read_at — positional read (seekable objects: files). uoffset_t is a
// strong type: passing a size where an offset belongs does not compile.
#pragma once

#include "iox/core/buffer.h"
#include "iox/core/units.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct read_at_policy {
    struct args_t {
        wbytes dest{};
        std::int64_t offset = 0;
    };
    // uoffset_t{UINT64_MAX} used to wrap to -1 — the kernel's "use the
    // current file position" sentinel — silently turning a positional read
    // into a stream read (red-team F3). Offsets that cannot round-trip
    // through int64 complete with EOVERFLOW instead.
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (a.offset < 0) {
            stdexec::set_error(std::move(r), iox::error::from_errno(EOVERFLOW));
            return true;
        }
        return false;
    }
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_read(sqe, f.v, a.dest.data(), a.dest.size(), a.offset);
    }
    using complete = transfer_complete;
};


} // namespace detail

inline constexpr struct read_at_t {
    /// Customization point: drivers provide `tag_invoke(read_at_t, ctx,
    /// handle, wbytes, uoffset_t)` for their own handle types; the fd
    /// default below serves seekable fd-backed handles.
    template <class H>
    requires tag_invocable<read_at_t, io_context&, H, wbytes, uoffset_t>
    auto operator()(io_context& ctx, H&& h, wbytes dest, uoffset_t at) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), dest, at)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), dest, at)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), dest, at);
    }
} read_at{};

// ---- fd driver default -----------------------------------------------------

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> || read_seekable<std::remove_cvref_t<H>>
auto tag_invoke(read_at_t, io_context& ctx, H&& h, wbytes dest, uoffset_t at) noexcept {
    return detail::fd_sender<detail::read_at_policy>{
        &ctx, detail::reader_fd(h), {dest, static_cast<std::int64_t>(at.v)}};
}

} // namespace iox::io
