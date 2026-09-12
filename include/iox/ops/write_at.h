// io::write_at — positional write (seekable objects: files). uoffset_t is a
// strong type: passing a size where an offset belongs does not compile.
#pragma once

#include "iox/core/buffer.h"
#include "iox/core/units.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct write_at_policy {
    struct args_t {
        rbytes src{};
        std::int64_t offset = 0;
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    // Offsets that wrap negative (UINT64_MAX -> -1 = "current position"
    // sentinel) are rejected up front (red-team F3): a positional write
    // must never silently land at the stream position.
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (a.offset < 0) {
            stdexec::set_error(std::move(r), iox::error::from_errno(EOVERFLOW));
            return true;
        }
        return false;
    }
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_write(sqe, f.v, a.src.data(), a.src.size(), a.offset);
    }
    using complete = transfer_complete;
};


} // namespace detail

inline constexpr struct write_at_t {
    /// Customization point: drivers provide `tag_invoke(write_at_t, ctx,
    /// handle, rbytes, uoffset_t)` for their own handle types; the fd
    /// default below serves seekable fd-backed handles.
    template <class H>
    requires tag_invocable<write_at_t, io_context&, H, rbytes, uoffset_t>
    auto operator()(io_context& ctx, H&& h, rbytes src, uoffset_t at) const
        noexcept(noexcept(tag_invoke(*this, ctx, std::forward<H>(h), src, at)))
        -> decltype(tag_invoke(*this, ctx, std::forward<H>(h), src, at)) {
        return tag_invoke(*this, ctx, std::forward<H>(h), src, at);
    }
} write_at{};

// ---- fd driver default -----------------------------------------------------

template <class H>
requires std::same_as<std::remove_cvref_t<H>, iox::fd> || write_seekable<std::remove_cvref_t<H>>
auto tag_invoke(write_at_t, io_context& ctx, H&& h, rbytes src, uoffset_t at) noexcept {
    return detail::fd_sender<detail::write_at_policy>{
        &ctx, detail::writer_fd(h), {src, static_cast<std::int64_t>(at.v)}};
}

} // namespace iox::io
