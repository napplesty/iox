// iox — unified async IO for Linux
// uring/ring.h — thin RAII wrapper around liburing's io_uring.
//
// Design notes (design §三):
//  * No virtual dispatch, no allocation: sqe/cqe handling maps 1:1 onto the
//    liburing C API so the compiler sees straight-line code on the hot path.
//  * `enters()` counts io_uring_enter(2) syscalls — it backs the batch_scope
//    tests and the submit-path micro benchmark (perf budget: many ops per
//    enter).
#pragma once

#include <liburing.h>

#include <cstdint>
#include <utility>

namespace iox::uring {

struct ring_params {
    /// SQ size (power of two). 256 keeps the ring inside a couple of pages.
    unsigned entries = 256;
    /// CQ size override (0 = kernel default of 2 × entries). Raise it for
    /// bursty completion workloads (benchmarks, multishot ops).
    unsigned cq_entries = 0;
    /// Kernel-side submission polling thread. Off by default: it burns a core
    /// and is only worth it for very high submit rates (ADR-003).
    bool sq_poll = false;
    /// 128-byte SQEs: doubles the SQ memory but gives uring_cmd payloads
    /// (e.g. NVMe passthru's 72-byte nvme_uring_cmd) room in the SQE's inline
    /// command area. Required by the NVMe driver; ordinary ops don't care.
    bool sqe128 = false;
};

class ring {
public:
    ring() noexcept = default;

    explicit ring(const ring_params& p) : sqe128_(p.sqe128) {
        io_uring_params params{};
        if (p.sq_poll) {
            params.flags |= IORING_SETUP_SQPOLL;
        }
        if (p.cq_entries != 0) {
            params.cq_entries = p.cq_entries;
            params.flags |= IORING_SETUP_CQSIZE;
        }
        if (p.sqe128) {
            params.flags |= IORING_SETUP_SQE128;
        }
        ok_ = ::io_uring_queue_init_params(p.entries, &ring_, &params) == 0;
    }

    ~ring() {
        if (ok_) {
            ::io_uring_queue_exit(&ring_);
        }
    }

    ring(ring&& other) noexcept : ring_(other.ring_), ok_(other.ok_) {
        other.ok_ = false;
    }

    ring& operator=(ring&& other) noexcept {
        if (this != &other) {
            if (ok_) {
                ::io_uring_queue_exit(&ring_);
            }
            ring_ = other.ring_;
            ok_ = other.ok_;
            other.ok_ = false;
        }
        return *this;
    }

    ring(const ring&) = delete;
    ring& operator=(const ring&) = delete;

    bool ok() const noexcept { return ok_; }

    /// Whether this ring was created with 128-byte SQEs (uring_cmd payload
    /// room). Drivers whose inline commands don't fit a 64-byte SQE check
    /// this and fail with a typed error instead of a kernel -EINVAL.
    bool sqe128() const noexcept { return sqe128_; }

    io_uring* native() noexcept { return &ring_; }
    const io_uring* native() const noexcept { return &ring_; }

    // ---- submission ----

    /// Reserve the next SQE, or nullptr when the SQ ring is full (caller
    /// should flush() and try again). Does not touch user_data.
    io_uring_sqe* next_sqe() noexcept { return ::io_uring_get_sqe(&ring_); }

    /// Flush prepared SQEs to the kernel. Returns submitted count (>=0) or
    /// negative errno. No-op — and no syscall — when nothing is pending
    /// (empty flushes would otherwise burn one io_uring_enter per loop
    /// iteration; perf budget §三.③).
    int flush() noexcept {
        if (!ok_) {
            return -EINVAL;
        }
        if (::io_uring_sq_ready(&ring_) == 0) {
            return 0;
        }
        ++enters_;
        return ::io_uring_submit(&ring_);
    }

    /// Flush and block until at least `wait_nr` completions are ready (or the
    /// ring has an eventfd wakeup — M-later). Returns submitted count or
    /// negative errno.
    int flush_and_wait(unsigned wait_nr) noexcept {
        if (!ok_) {
            return -EINVAL;
        }
        ++enters_;
        return ::io_uring_submit_and_wait(&ring_, static_cast<int>(wait_nr));
    }

    // ---- completion ----

    /// Number of CQEs ready to reap without blocking.
    unsigned cq_ready() const noexcept { return ::io_uring_cq_ready(&ring_); }

    /// Invoke `f(io_uring_cqe*)` for every ready CQE and return how many
    /// were visited. Never blocks. Each entry is COPIED and marked seen
    /// BEFORE the callback runs: a nested drain inside a completion
    /// (sync_wait/run_for re-entered from a thunk) must never re-reap the
    /// entry being dispatched — the batch-advance loop had exactly that
    /// re-entrancy hole (red-team F1: unbounded recursion → stack overflow).
    template <class F>
    unsigned for_each_cqe(F&& f) noexcept(noexcept(f(static_cast<io_uring_cqe*>(nullptr)))) {
        unsigned n = 0;
        for (;;) {
            io_uring_cqe* cqe = nullptr;
            if (::io_uring_peek_cqe(&ring_, &cqe) != 0 || cqe == nullptr) {
                break;
            }
            const io_uring_cqe copy = *cqe; // the slot is reusable once seen
            ::io_uring_cqe_seen(&ring_, cqe);
            f(const_cast<io_uring_cqe*>(&copy));
            ++n;
        }
        return n;
    }

    // ---- observability ----

    /// io_uring_enter(2) syscalls issued through this wrapper. Testing +
    /// benchmark hook; not a kernel API.
    std::uint64_t enters() const noexcept { return enters_; }

private:
    io_uring ring_{};
    bool ok_ = false;
    bool sqe128_ = false;
    std::uint64_t enters_ = 0;
};

} // namespace iox::uring
