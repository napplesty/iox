// iox — unified async IO for Linux
// include/iox/runtime/io_context.h — the event loop: owns an io_uring instance, runs/stops,
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

class batch_scope;

class io_context {
public:
    io_context() { disarm_sigpipe(); }
    explicit io_context(uring::ring_params p) : ring_(p) { disarm_sigpipe(); }

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    void run() noexcept;

    void run_for(std::chrono::nanoseconds d) noexcept;

    void stop() noexcept { stopped_ = true; }
    bool stopped() const noexcept { return stopped_; }
    void restart() noexcept { stopped_ = false; }

    bool in_batch() const noexcept { return batch_depth_ != 0; }

    io_uring_sqe* acquire_sqe(op_base& op) noexcept;

    void flush() noexcept;

    void dispatch(std::uint64_t user_data, std::int32_t res, std::uint32_t flags) noexcept {
        auto* op = reinterpret_cast<op_base*>(static_cast<std::uintptr_t>(user_data));
        op->thunk(op, *this, res, flags);
    }

    uring::ring& ring() noexcept { return ring_; }
    bool ok() const noexcept { return ring_.ok(); }

    void arm_failpoint(int nth, int neg_errno) noexcept {
        fail_nth_ = nth;
        fail_code_ = neg_errno;
        fail_ordinal_ = 0;
    }
    void clear_failpoint() noexcept { fail_nth_ = 0; }

    iox::error acquire_error() noexcept {
        if (submit_error_ != 0) {
            const int code = submit_error_;
            submit_error_ = 0;
            return iox::error::from_negative(code);
        }
        return iox::error::from_errno(EBUSY);
    }

    bool buffers_registered() const noexcept { return buffers_registered_; }
    void set_buffers_registered(bool v = true) noexcept { buffers_registered_ = v; }

    void submit_cancel(std::uint64_t target_user_data) noexcept {
        for (cancel_receipt& receipt : cancel_receipts_) {
            if (receipt.busy) {
                continue;
            }
            arm_cancel(receipt, target_user_data);
            return;
        }
        cancel_receipts_.emplace_back();
        arm_cancel(cancel_receipts_.back(), target_user_data);
    }

    void attach_source(completion_source& s) noexcept { bridge_.attach(*this, s); }
    void detach_source(completion_source& s) noexcept { bridge_.detach(*this, s); }
    bool source_attached(const completion_source& s) const noexcept {
        return bridge_.attached(s);
    }

private:
    friend class batch_scope;

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
            receipt.busy = false;
        }
    }

    unsigned drain() noexcept;

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
    unsigned run_depth_ = 0;
    int fail_nth_ = 0;
    int fail_code_ = 0;
    int fail_ordinal_ = 0;
    int submit_error_ = 0;
    deadline_op deadline_op_{};
    std::deque<cancel_receipt> cancel_receipts_{};
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

inline io_uring_sqe* io_context::acquire_sqe(op_base& op) noexcept {
    if (!ring_.ok()) {
        return nullptr;
    }
    if (fail_nth_ > 0 && ++fail_ordinal_ == fail_nth_) {
        submit_error_ = fail_code_;
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
    ++run_depth_;
    while (!stopped_) {
        if (drain() != 0) {
            continue;
        }
        if (stopped_ || batch_depth_ != 0) {
            break;
        }
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
    if (run_depth_ != 0) {
        return;
    }
    stopped_ = false;
    ++run_epoch_;
}

inline void io_context::deadline_op::on_fired(op_base* self, io_context& ctx,
                                              std::int32_t, std::uint32_t) noexcept {
    auto& op = *static_cast<deadline_op*>(self);
    if (op.epoch == ctx.run_epoch_) {
        ctx.stop();
    }
}

}
