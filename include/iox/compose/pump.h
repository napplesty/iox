// iox — unified async IO for Linux
// compose/pump.h — io::pump: move every byte from one fd to another with
// the kernel's zero-copy page plumbing (design §九: "端点均为 fd 时自动走
// splice 零拷贝"). One io::loop, two IORING_OP_SPLICEs per chunk through a
// bounce pipe — the same shape sendfile and copy_file_range use internally,
// so file→socket IS sendfile and file→file IS copy_file_range here, with no
// per-mode vocabulary to learn.
//
// splice requires one pipe end; the bounce pipe supplies it when neither
// endpoint is a pipe. A one-byte synchronous probe of the SOURCE decides
// support before anything moves: a source that cannot splice completes
// set_error(EINVAL-family) with zero bytes moved, so callers can fall back
// to a read/write pump with nothing lost (examples/ioxpump.cc does exactly
// that). A SINK that rejects splice mid-run cannot be probed losslessly, so
// the loop armours itself with let_error: bytes stuck in the bounce pipe
// are drained and delivered with plain writes before the error propagates —
// the source's position always matches exactly what reached the sink.
//
// Completes set_value() at EOF, set_error on failure (after recovery),
// set_stopped under cancellation. Bytes moved accumulate into *moved (when
// non-null) as chunks land; read it after completion.
//
// ONE pump per sink fd at a time: stage 2 splices with off_out = -1
// (advance the file position), and the kernel does NOT serialize position
// writes across separate splices the way it does for WRITE — two pumps
// into the same fd silently overwrite each other's chunks while both
// report full success (red-team F2).
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

// The splice stage as the pump needs it: a `fail` verdict in the args lets
// a failed probe complete the chain with the probe's errno without ever
// touching the kernel (the immediate hook — same trick as write's zero-len
// completion).
struct pump_stage_policy {
    struct args_t {
        int fail = 0;  // probe errno to complete with; 0 = proceed
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
        if (a.len == 0) { // EOF hand-off: stage 2 of a zero-byte read
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

/// Flip an fd to O_NONBLOCK and return its previous flags (-1 on failure).
inline int set_nonblock(int fd, bool on) noexcept {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    const int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return (::fcntl(fd, F_SETFL, want) == 0) ? flags : -1;
}

} // namespace

/// Deliver everything stuck in the bounce pipe to `out` through plain
/// writes. Runs once, on the error path, before the error propagates.
///
/// Both fds are flipped to O_NONBLOCK for the duration (restored after): a
/// blocking read on an empty pipe or a blocking write to a full sink would
/// PARK THE IO THREAD — the very wedge this recovery exists to prevent
/// (red-team pump_wedge: the loop froze before even the cancel timer could
/// dispatch). The total budget is bounded; on its expiry whatever is still
/// stuck is abandoned (the error we are about to propagate already makes
/// the stream a loss case — the documentation covers it).
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
                return; // sink broke harder than the error we report
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

    // Bounce pipe (sized for a full chunk best-effort: a smaller kernel cap
    // only shortens per-iteration counts, never correctness). The LOOP
    // LAMBDA owns it — pump_impl returns before the loop ever arms, so a
    // local pair would close its fds and leave the splices on dead numbers.
    auto bounce_maybe = pipe::pair::create();
    int fail = 0;
    int bounce_write_fd = -1;
    int bounce_read_fd = -1;
    ::ssize_t probe = 0; // bytes the probe already placed in the bounce pipe
    if (bounce_maybe) {
        // Both ends stay BLOCKING on the hot path: io_uring does not retry
        // EAGAIN for splice, so a nonblocking end turns legitimate waits
        // (empty source, full sink) into spurious error completions
        // (red-team pump_wedge). The recovery drain below flips flags
        // temporarily instead.
        // Pipe capacity counts in PAGES: the probe byte below occupies a
        // whole page, so a pipe sized exactly to `chunk` (or 1 page) leaves
        // zero free pages and a blocking splice stalls forever. Always keep
        // at least the default 64 KiB plus one page of slack.
        const std::size_t want = std::max<std::size_t>(chunk, 65536) + 4096;
        (void)::fcntl(bounce_maybe->w.write_handle().v, F_SETPIPE_SZ,
                      static_cast<int>(want));
        bounce_write_fd = bounce_maybe->w.write_handle().v;
        bounce_read_fd = bounce_maybe->r.read_handle().v;
        // One-byte source probe. spliced == 1: the byte rides in the pipe and the
        // first iteration's stage 2 delivers it (FIFO — order preserved).
        // spliced == 0: source at EOF, the loop ends on its first iteration.
        // EAGAIN: source temporarily empty — proceed optimistically; a sink
        // that later rejects splice is covered by recover_stuck.
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

    // One lambda in one non-template inline function: every io::pump call
    // shares this sender type (same trick as write_all_impl).
    return loop(*ctx, [ctx, in, out, chunk, moved, bounce = std::move(bounce), fail,
                     pending = static_cast<unsigned>(probe)]() mutable {
        const int bounce_write_fd = bounce ? bounce->w.write_handle().v : -1;
        const int bounce_read_fd = bounce ? bounce->r.read_handle().v : -1;
        return fd_sender<pump_stage_policy>{ctx, iox::fd{bounce_write_fd},
                                            {fail, in.v, static_cast<unsigned>(chunk)}}
             | stdexec::let_value([ctx, out, bounce_read_fd, moved, &pending](std::size_t spliced) {
                   // stage 2 drains the chunk AND any probe byte waiting in
                   // front of it — the pipe is FIFO, this preserves order.
                   const unsigned n =
                       static_cast<unsigned>(spliced) + std::exchange(pending, 0u);
                   return fd_sender<pump_stage_policy>{ctx, out, {0, bounce_read_fd, n}}
                        | stdexec::then([moved, n](std::size_t) {
                              if (moved != nullptr) {
                                  *moved += n;
                              }
                              return n == 0; // EOF on the source
                          });
               })
             | stdexec::let_error([bounce_read_fd, out](auto&& e) {
                   // the chain's error channel is iox::error OR exception_ptr
                   // (adaptors add the latter); recover either way.
                   if (bounce_read_fd >= 0 && out.v >= 0) {
                       recover_stuck(bounce_read_fd, out.v);
                   }
                   return stdexec::just_error(std::forward<decltype(e)>(e));
               });
    });
}

} // namespace detail

inline constexpr struct pump_t {
    /// Move everything readable from `in` to `out` (kernel zero-copy) until
    /// EOF. `chunk` is the per-iteration splice size; `moved`, when non-null,
    /// accumulates the byte count (read it after completion; it must outlive
    /// the pump).
    auto operator()(io_context& ctx, iox::fd in, iox::fd out,
                    std::size_t chunk = 256 * 1024,
                    std::uint64_t* moved = nullptr) const noexcept {
        return detail::pump_impl(&ctx, in, out, chunk, moved);
    }
} pump{};

} // namespace iox::io
