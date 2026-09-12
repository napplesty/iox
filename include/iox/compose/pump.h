// iox — unified async IO for Linux
// include/iox/compose/pump.h — io::pump: move every byte from one fd to another with
#pragma once

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <stdexec/execution.hpp>

#include "iox/compose/loop.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/ops/fd_sender.h"
#include "iox/pipe/channel.h"

namespace iox::io {

namespace detail {

struct pump_stage_policy {
    struct args_t {
        int fail = 0;
        int fd_in = -1;
        unsigned len = 0;
        std::int64_t off_in = -1;
        std::int64_t off_out = -1;
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (a.fail != 0) {
            stdexec::set_error(std::move(r), iox::error::from_errno(a.fail));
            return true;
        }
        if (a.len == 0) {
            stdexec::set_value(std::move(r), std::size_t{0});
            return true;
        }
        return false;
    }
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_splice(sqe, a.fd_in, a.off_in, f.v, a.off_out, a.len, 0);
    }
    using complete = transfer_complete;
};

inline bool splice_unsupported(int e) noexcept {
    return e == EINVAL || e == ENOSYS || e == EOPNOTSUPP;
}

namespace {

inline int set_nonblock(int fd, bool on) noexcept {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    const int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return (::fcntl(fd, F_SETFL, want) == 0) ? flags : -1;
}

}

inline void recover_stuck(int bounce_read_fd, int out) noexcept {
    constexpr auto budget = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const int saved_read_flags = set_nonblock(bounce_read_fd, true);
    const int saved_write_flags = set_nonblock(out, true);
    auto restore = [&] {
        if (saved_read_flags >= 0) {
            (void)::fcntl(bounce_read_fd, F_SETFL, saved_read_flags);
        }
        if (saved_write_flags >= 0) {
            (void)::fcntl(out, F_SETFL, saved_write_flags);
        }
    };
    while (std::chrono::steady_clock::now() < deadline) {
        std::byte buffer[4096];
        const ::ssize_t r = ::read(bounce_read_fd, buffer, sizeof(buffer));
        if (r <= 0) {
            break; // EAGAIN: drained
        }
        std::size_t off = 0;
        while (off < static_cast<std::size_t>(r)) {
            const ::ssize_t w = ::write(out, buffer + off, static_cast<std::size_t>(r) - off);
            if (w > 0) {
                off += static_cast<std::size_t>(w);
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                restore();
                return;
            }
            const int remain_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count());
            ::pollfd p{out, POLLOUT, 0};
            (void)!::poll(&p, 1, std::min(remain_ms, 100));
        }
    }
    restore();
}

inline auto pump_impl(io_context* ctx, iox::fd in, iox::fd out, std::size_t chunk,
                      std::uint64_t* moved) {
    if (chunk == 0) {
        chunk = 1;
    }

    auto bounce_maybe = pipe::pair::create();
    int fail = 0;
    int bounce_write_fd = -1;
    int bounce_read_fd = -1;
    ::ssize_t probe = 0;
    if (bounce_maybe) {
        const std::size_t want = std::max<std::size_t>(chunk, 65536) + 4096;
        (void)::fcntl(bounce_maybe->w.write_handle().v, F_SETPIPE_SZ,
                      static_cast<int>(want));
        bounce_write_fd = bounce_maybe->w.write_handle().v;
        bounce_read_fd = bounce_maybe->r.read_handle().v;
        probe = ::splice(in.v, nullptr, bounce_write_fd, nullptr, 1, SPLICE_F_NONBLOCK);
        if (probe < 0 && splice_unsupported(errno)) {
            fail = errno;
        }
        if (probe < 0) {
            probe = 0;
        }
    } else {
        fail = bounce_maybe.error().code();
    }
    std::optional<pipe::pair> bounce =
        bounce_maybe ? std::optional<pipe::pair>{std::move(*bounce_maybe)} : std::nullopt;

    return loop(*ctx, [ctx, in, out, chunk, moved, bounce = std::move(bounce), fail,
                     pending = static_cast<unsigned>(probe)]() mutable {
        const int bounce_write_fd = bounce ? bounce->w.write_handle().v : -1;
        const int bounce_read_fd = bounce ? bounce->r.read_handle().v : -1;
        return fd_sender<pump_stage_policy>{ctx, iox::fd{bounce_write_fd},
                                            {fail, in.v, static_cast<unsigned>(chunk)}}
             | stdexec::let_value([ctx, out, bounce_read_fd, moved, &pending](std::size_t spliced) {
                   const unsigned n =
                       static_cast<unsigned>(spliced) + std::exchange(pending, 0u);
                   return fd_sender<pump_stage_policy>{ctx, out, {0, bounce_read_fd, n}}
                        | stdexec::then([moved, n](std::size_t) {
                              if (moved != nullptr) {
                                  *moved += n;
                              }
                              return n == 0;
                          });
               })
             | stdexec::let_error([bounce_read_fd, out](auto&& e) {
                   if (bounce_read_fd >= 0 && out.v >= 0) {
                       recover_stuck(bounce_read_fd, out.v);
                   }
                   return stdexec::just_error(std::forward<decltype(e)>(e));
               });
    });
}

}

inline constexpr struct pump_t {
    auto operator()(io_context& ctx, iox::fd in, iox::fd out,
                    std::size_t chunk = 256 * 1024,
                    std::uint64_t* moved = nullptr) const noexcept {
        return detail::pump_impl(&ctx, in, out, chunk, moved);
    }
} pump{};

}
