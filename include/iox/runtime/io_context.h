// iox — unified async IO for Linux
// io_context.h — the event loop: owns an io_uring instance, runs/stops,
// reaps and dispatches completions, and hosts the batching protocol.
// Operation ABI lives in runtime/op.h; the driver bridge (external
// completion sources) lives in runtime/source_registry.h.
//
// Threading model (design §三.⑨): one io_context per thread, no internal
// locking. run()/stop() and all submissions must happen on the owning
// thread. Cross-thread wakeup arrives in a later milestone.
//
// Completion dispatch: every submitted SQE carries the address of its
// operation state in user_data. When the CQE comes back, io_context invokes
// op_base::thunk — a single function pointer per op, no vtable (§三.①⑧).
// dispatch() is public: it is the injection point for unit tests (fake CQEs)
// and for external completion sources (Driver SPI "主动注入" mode, M5).
#pragma once

#include "iox/core/error.h"
#include "iox/runtime/op.h"
#include "iox/runtime/source_registry.h"
#include "iox/uring/ring.h"

#include <chrono>
#include <csignal>
#include <deque>
#include <cstdint>
#include <linux/time_types.h>

namespace iox {

/// RAII batching scope (design §三.③): operations submitted inside the scope
/// are held back and flushed to the kernel in one io_uring_enter when the
/// outermost scope exits — turning "one syscall per op" into "one per batch".
class batch_scope;

class io_context {
public:
    io_context() { disarm_sigpipe(); }
    explicit io_context(uring::ring_params p) : ring_(p) { disarm_sigpipe(); }

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    /// Event loop: reap and dispatch completions until stop() is requested
    /// (typically from inside a completion). Blocks waiting for events when
    /// nothing is ready — an io_context with no outstanding operations and
    /// no timer will sleep in io_uring_enter.
    void run() noexcept;

    /// Run for at most `d`. Safe to call repeatedly: the stopped flag is
    /// reset on entry, and stale deadlines from an earlier stop are
    /// neutralized by an internal epoch check.
    void run_for(std::chrono::nanoseconds d) noexcept;

    /// Request loop exit. Must be called on the owning thread (i.e. from a
    /// completion or before run()).
    void stop() noexcept { stopped_ = true; }
    bool stopped() const noexcept { return stopped_; }
    void restart() noexcept { stopped_ = false; }

    /// True while a batch_scope on this thread is holding submissions back.
    /// sync_wait() refuses to run inside one (it could never complete).
    bool in_batch() const noexcept { return batch_depth_ != 0; }

    /// Reserve an SQE for `op` and stamp user_data = &op. Flushes first if
    /// the submission queue is full. Returns nullptr only if the ring is
    /// unusable (caller completes the op inline with an error).
    io_uring_sqe* acquire_sqe(op_base& op) noexcept;

    /// Push prepared SQEs to the kernel unless a batch_scope is active
    /// (then the outermost scope flushes).
    void flush() noexcept;

    /// Route one completion to its operation. Also the test/bridge hook:
    /// tests synthesize completions with this entry point instead of going
    /// through the kernel.
    void dispatch(std::uint64_t user_data, std::int32_t res, std::uint32_t flags) noexcept {
        auto* op = reinterpret_cast<op_base*>(static_cast<std::uintptr_t>(user_data));
        op->thunk(op, *this, res, flags);
    }

    uring::ring& ring() noexcept { return ring_; }
    bool ok() const noexcept { return ring_.ok(); }

    // ---- fault injection (test hook, design §七.4) --------------------------
    //
    // Arm to make the `nth` SQE reservation (1-based, counting EVERY
    // acquire_sqe call — vocabulary ops, timers, cancel receipts — from
    // arming on) fail with `neg_errno` (e.g. -ENOMEM, -EMFILE). The refused
    // operation completes inline through its normal error path, exactly
    // like an unusable ring (EBUSY) does — no double completion, no kernel
    // state, full stop-callback discipline. Single-threaded by contract.
    void arm_failpoint(int nth, int neg_errno) noexcept {
        fail_nth_ = nth;
        fail_code_ = neg_errno;
        fail_ordinal_ = 0;
    }
    void clear_failpoint() noexcept { fail_nth_ = 0; }

    /// The error an operation whose acquire_sqe() returned nullptr must
    /// complete with: the injected code when a failpoint refused the
    /// reservation, EBUSY when the ring itself is unusable.
    iox::error acquire_error() noexcept {
        if (submit_error_ != 0) {
            const int code = submit_error_;
            submit_error_ = 0;
            return iox::error::from_negative(code);
        }
        return iox::error::from_errno(EBUSY);
    }

    // Registered-buffer table state (one table per ring — see buffer_pool).
    bool buffers_registered() const noexcept { return buffers_registered_; }
    void set_buffers_registered(bool v = true) noexcept { buffers_registered_ = v; }

    /// Submit IORING_OP_ASYNC_CANCEL against `target_user_data` (the address
    /// of an in-flight op_state). The cancel request's own lifetime is owned
    /// by the context via a receipt pool, so a late-arriving cancel CQE can
    /// never dangle — unlike the target op, which may have already completed.
    /// Cancellation is cooperative and single-threaded: the stop callback
    /// runs on this thread and merely submits; the target completes with
    /// -ECANCELED through the normal dispatch path.
    void submit_cancel(std::uint64_t target_user_data) noexcept {
        for (cancel_receipt& receipt : cancel_receipts_) {
            if (receipt.busy) {
                continue;
            }
            arm_cancel(receipt, target_user_data);
            return;
        }
        // All receipts busy: GROW. A mass cancel (>64 in-flight ops) must
        // never silently drop — the dropped op becomes permanently
        // uncancellable and its sync_wait hangs forever (red-team receipt
        // exhaustion: 100 ops, 36 stranded). deque keeps addresses stable.
        cancel_receipts_.emplace_back();
        arm_cancel(cancel_receipts_.back(), target_user_data);
    }

    // ---- external completion sources (driver bridge) — forwarding facades --
    // Semantics and machinery: runtime/source_registry.h.

    void attach_source(completion_source& s) noexcept { bridge_.attach(*this, s); }
    void detach_source(completion_source& s) noexcept { bridge_.detach(*this, s); }
    bool source_attached(const completion_source& s) const noexcept {
        return bridge_.attached(s);
    }

private:
    friend class batch_scope;

    // Raw-fd writes (io::write(ctx, iox::fd, ...)) use IORING_OP_WRITE,
    // which raises SIGPIPE on a closed peer — one dead connection would kill
    // the whole process. Typed socket handles send with MSG_NOSIGNAL; for
    // the raw escape hatch the only safe disposition is process-wide ignore
    // (same call libuv makes). ADR-008.
    static void disarm_sigpipe() noexcept {
        ::signal(SIGPIPE, SIG_IGN);
    }

    struct cancel_receipt final : op_base {
        bool busy = false;

        cancel_receipt() noexcept : op_base(&cancel_receipt::done) {}

        static void done(op_base* self, io_context&, std::int32_t,
                         std::uint32_t) noexcept {
            static_cast<cancel_receipt*>(self)->busy = false;
        }
    };

    void arm_cancel(cancel_receipt& receipt, std::uint64_t target_user_data) noexcept {
        receipt.busy = true;
        if (io_uring_sqe* sqe = acquire_sqe(receipt)) {
            ::io_uring_prep_cancel(sqe,
                                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(target_user_data)), 0);
        } else {
            receipt.busy = false; // ring unusable; the target completes on its own
        }
    }

    /// Drain all ready CQEs without blocking; returns how many were dispatched.
    unsigned drain() noexcept;

    // Deadline op backing run_for(). It lives for the lifetime of the context
    // so its address (used as user_data) is always valid, even when a stale
    // deadline fires after run_for already returned.
    struct deadline_op final : op_base {
        __kernel_timespec ts{};
        std::uint64_t epoch = 0;

        deadline_op() noexcept : op_base(&deadline_op::on_fired) {}

        static void on_fired(op_base* self, io_context& ctx, std::int32_t,
                             std::uint32_t) noexcept;
    };

    static void on_cqe(io_context* self, io_uring_cqe* cqe) noexcept {
        self->dispatch(cqe->user_data, cqe->res, cqe->flags);
    }

    uring::ring ring_{uring::ring_params{}};
    unsigned batch_depth_ = 0;
    bool stopped_ = false;
    bool buffers_registered_ = false;
    std::uint64_t run_epoch_ = 0;
    unsigned run_depth_ = 0; // >0 while a run()/run_for() frame is active
    int fail_nth_ = 0;       // 0 = failpoints off; else 1-based submit ordinal
    int fail_code_ = 0;      // negative errno handed to the refused op
    int fail_ordinal_ = 0;
    int submit_error_ = 0;   // set by acquire_sqe when a failpoint fires
    deadline_op deadline_op_{};
    std::deque<cancel_receipt> cancel_receipts_{}; // grows under mass cancel
    source_registry bridge_{};
};

class batch_scope {
public:
    explicit batch_scope(io_context& c) noexcept : ctx_(&c) { ++ctx_->batch_depth_; }

    ~batch_scope() {
        if (--ctx_->batch_depth_ == 0) {
            ctx_->ring_.flush();
        }
    }

    batch_scope(const batch_scope&) = delete;
    batch_scope& operator=(const batch_scope&) = delete;

private:
    io_context* ctx_;
};

#include "iox/runtime/source_registry_impl.inc"

// ---- inline impls ----

inline io_uring_sqe* io_context::acquire_sqe(op_base& op) noexcept {
    if (!ring_.ok()) {
        return nullptr; // unusable ring: caller completes the op inline
    }
    if (fail_nth_ > 0 && ++fail_ordinal_ == fail_nth_) {
        submit_error_ = fail_code_; // test failpoint: refuse this reservation
        return nullptr;
    }
    io_uring_sqe* sqe = ring_.next_sqe();
    if (sqe == nullptr) {
        ring_.flush();
        sqe = ring_.next_sqe();
    }
    if (sqe != nullptr) {
        ::io_uring_sqe_set_data(sqe, &op);
    }
    return sqe;
}

inline void io_context::flush() noexcept {
    if (batch_depth_ == 0) {
        ring_.flush();
    }
}

inline unsigned io_context::drain() noexcept {
    return ring_.for_each_cqe([this](io_uring_cqe* cqe) { on_cqe(this, cqe); });
}

inline void io_context::run() noexcept {
    // Steady state: drain() dispatches every ready completion (each may arm
    // new SQEs); only when nothing is ready do we submit the accumulated
    // SQEs and block for the next completion — one io_uring_enter per wake,
    // carrying whatever batch has formed. Flushing eagerly at the top of the
    // loop would collapse batching to one syscall per operation.
    ++run_depth_;
    while (!stopped_) {
        if (drain() != 0) {
            continue;
        }
        // Idle: if a batch_scope is active, its exit will submit — run()
        // cannot pump anything until then (and must not spin or submit
        // behind the scope's back).
        if (stopped_ || batch_depth_ != 0) {
            break;
        }
        // Nothing ready: submit pending SQEs and block until ≥1 completion.
        ring_.flush_and_wait(1);
    }
    --run_depth_;
}

inline void io_context::run_for(std::chrono::nanoseconds d) noexcept {
    namespace chrono = std::chrono;
    stopped_ = false;
    const auto deadline = chrono::steady_clock::now() + d;

    ++run_epoch_;
    deadline_op_.epoch = run_epoch_;
    deadline_op_.ts.tv_sec = chrono::duration_cast<chrono::seconds>(deadline.time_since_epoch()).count();
    deadline_op_.ts.tv_nsec =
        (chrono::duration_cast<chrono::nanoseconds>(deadline.time_since_epoch()) % chrono::seconds{1}).count();

    if (io_uring_sqe* sqe = acquire_sqe(deadline_op_)) {
        ::io_uring_prep_timeout(sqe, &deadline_op_.ts, 0, IORING_TIMEOUT_ABS);
        run();
    }
    // Nesting guard: a run_for() re-entered from inside a completion (the
    // outer run()'s dispatch is on the stack) must NOT do the exit
    // bookkeeping — clearing stopped_/bumping run_epoch_ there would retire
    // the OUTER invocation's still-in-flight deadline, and the outer loop
    // would block past its own deadline forever (stress a9e). A stop from
    // the nested frame legitimately ends the whole stack early; the
    // OUTERMOST run_for() does the cleanup once everything unwound.
    if (run_depth_ != 0) {
        return;
    }
    // Leave the loop runnable: the deadline (or a completion's stop()) ended
    // THIS invocation, not the context. A following run()/sync_wait must pump
    // — exiting instantly on a stale stopped flag strands in-flight SQEs.
    stopped_ = false;
    // Retire the deadline epoch too: an interrupted run_for leaves its
    // timeout SQE in the kernel with the CURRENT epoch — a later run()/
    // sync_wait (which never bump the epoch) would honor that stale deadline
    // and be killed by a stop() that belongs to nobody (red-team t1/t1b).
    ++run_epoch_;
}

inline void io_context::deadline_op::on_fired(op_base* self, io_context& ctx,
                                              std::int32_t, std::uint32_t) noexcept {
    auto& op = *static_cast<deadline_op*>(self);
    // A deadline may fire after its run_for() returned early (stopped from a
    // completion) — only stop the loop for the newest epoch.
    if (op.epoch == ctx.run_epoch_) {
        ctx.stop();
    }
}

} // namespace iox
