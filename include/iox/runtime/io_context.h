// iox — unified async IO for Linux
// include/iox/runtime/io_context.h — the event loop: owns an io_uring instance, runs/stops,
#pragma once

#include "iox/core/error.h"
#include "iox/runtime/op.h"
#include "iox/runtime/source_registry.h"
#include "iox/uring/ring.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <cstdint>
#include <mutex>
#include <thread>
#include <linux/time_types.h>

namespace iox {

class batch_scope;

class io_context {
public:
    io_context() { init_inbox(); }
    explicit io_context(uring::ring_params params) : ring_(params) { init_inbox(); }
    ~io_context() {
        if (inbox_efd_ >= 0) {
            ::close(inbox_efd_);
        }
    }

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    void run() noexcept;

    iox::error run_for(std::chrono::nanoseconds duration) noexcept;

    void stop() noexcept {
        stopped_.store(true, std::memory_order_release);
        if (inbox_efd_ >= 0 &&
            std::this_thread::get_id() != io_thread_.load(std::memory_order_acquire)) {
            wake(); // a loop blocked in io_uring_enter must observe the flag
        }
    }
    bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }
    void restart() noexcept { stopped_.store(false, std::memory_order_release); }

    bool in_batch() const noexcept { return batch_depth_ != 0; }

    io_uring_sqe* acquire_sqe(op_base& operation) noexcept;

    void flush() noexcept;

    void dispatch(std::uint64_t user_data, std::int32_t result, std::uint32_t flags) noexcept {
        auto* operation = reinterpret_cast<op_base*>(static_cast<std::uintptr_t>(user_data));
        operation->thunk(operation, *this, result, flags);
    }

    // Cross-thread entry: run `function(*this, argument)` on the io thread. Safe from any thread;
    // the io thread itself may call it too (executed at the next drain point).
    using io_task = void (*)(io_context&, std::uint64_t);
    void post(io_task function, std::uint64_t argument) noexcept {
        {
            std::lock_guard lock{inbox_mtx_};
            try {
                inbox_.push_back({function, argument});
            } catch (...) {
                return; // the deferred call is lost; the caller's op completes on its own path
            }
        }
        wake();
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
    void set_buffers_registered(bool registered = true) noexcept { buffers_registered_ = registered; }

    void submit_cancel(std::uint64_t target_user_data) noexcept {
        if (std::this_thread::get_id() == io_thread_.load(std::memory_order_acquire)) {
            arm_cancel_from_pool(target_user_data);
            return;
        }
        post(&submit_cancel_thunk, target_user_data); // foreign thread: defer to the loop
    }

    void attach_source(completion_source& source) noexcept { bridge_.attach(*this, source); }
    void detach_source(completion_source& source) noexcept { bridge_.detach(*this, source); }
    bool source_attached(const completion_source& source) const noexcept {
        return bridge_.attached(source);
    }

private:
    friend class batch_scope;

    static void disarm_sigpipe() noexcept {
        ::signal(SIGPIPE, SIG_IGN);
    }

    void init_inbox() noexcept {
        disarm_sigpipe();
        inbox_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    }

    void wake() noexcept {
        if (inbox_efd_ >= 0) {
            const std::uint64_t one = 1;
            (void)!::write(inbox_efd_, &one, sizeof one);
        }
    }

    struct cancel_receipt final : op_base {
        bool busy = false;

        cancel_receipt() noexcept : op_base(&cancel_receipt::done) {}

        static void done(op_base* self, io_context&, std::int32_t,
                         std::uint32_t) noexcept {
            static_cast<cancel_receipt*>(self)->busy = false;
        }
    };

    struct inbox_entry {
        io_task function;
        std::uint64_t argument;
    };

    struct inbox_watch final : op_base {
        bool in_flight = false;

        inbox_watch() noexcept : op_base(&inbox_watch::on_fired) {}

        static void on_fired(op_base* self, io_context& context, std::int32_t result,
                             std::uint32_t) noexcept {
            auto& watch = *static_cast<inbox_watch*>(self);
            watch.in_flight = false;
            if (result < 0) {
                return; // failpoint interception: the next post or run() re-arms
            }
            context.drain_inbox();
            context.arm_inbox_watch();
        }
    };

    static void submit_cancel_thunk(io_context& context, std::uint64_t target) noexcept {
        context.arm_cancel_from_pool(target);
    }

    void arm_cancel_from_pool(std::uint64_t target_user_data) noexcept {
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

    void arm_inbox_watch() noexcept {
        if (inbox_efd_ < 0 || inbox_watch_.in_flight) {
            return;
        }
        if (io_uring_sqe* sqe = acquire_sqe(inbox_watch_)) {
            ::io_uring_prep_poll_add(sqe, inbox_efd_, POLLIN);
            inbox_watch_.in_flight = true;
        }
    }

    void drain_inbox() noexcept {
        if (inbox_efd_ >= 0) {
            std::uint64_t count = 0;
            (void)!::read(inbox_efd_, &count, sizeof count);
        }
        std::deque<inbox_entry> drained;
        {
            std::lock_guard lock{inbox_mtx_};
            drained.swap(inbox_);
        }
        for (const inbox_entry& entry : drained) {
            entry.function(*this, entry.argument);
        }
    }

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
        __kernel_timespec timespec{};
        std::uint64_t epoch = 0;

        deadline_op() noexcept : op_base(&deadline_op::on_fired) {}

        static void on_fired(op_base* self, io_context& context, std::int32_t,
                             std::uint32_t) noexcept;
    };

    static void on_cqe(io_context* self, io_uring_cqe* cqe) noexcept {
        self->dispatch(cqe->user_data, cqe->res, cqe->flags);
    }

    uring::ring ring_{uring::ring_params{}};
    unsigned batch_depth_ = 0;
    std::atomic<bool> stopped_{false};
    bool buffers_registered_ = false;
    std::uint64_t run_epoch_ = 0;
    unsigned run_depth_ = 0;
    int fail_nth_ = 0;
    int fail_code_ = 0;
    int fail_ordinal_ = 0;
    int submit_error_ = 0;
    int inbox_efd_ = -1;
    deadline_op deadline_op_{};
    inbox_watch inbox_watch_{};
    std::deque<cancel_receipt> cancel_receipts_{};
    std::mutex inbox_mtx_;
    std::deque<inbox_entry> inbox_;
    std::atomic<std::thread::id> io_thread_{};
    source_registry bridge_{};
};

// SQEs inside one batch execute in any order: dependent ops (write → fsync →
// close on the same fd) must be chained through completions, not batched.
class batch_scope {
public:
    explicit batch_scope(io_context& context) noexcept : context_(&context) { ++context_->batch_depth_; }

    ~batch_scope() {
        if (--context_->batch_depth_ == 0) {
            context_->ring_.flush();
        }
    }

    batch_scope(const batch_scope&) = delete;
    batch_scope& operator=(const batch_scope&) = delete;

private:
    io_context* context_;
};

#include "iox/runtime/source_registry_impl.inc"

inline io_uring_sqe* io_context::acquire_sqe(op_base& operation) noexcept {
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
        ::io_uring_sqe_set_data(sqe, &operation);
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
    if (!ring_.ok()) {
        return;
    }
    stopped_ = false;
    ++run_depth_;
    io_thread_.store(std::this_thread::get_id(), std::memory_order_release);
    arm_inbox_watch();
    drain_inbox();
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
    if (run_depth_ == 0) {
        io_thread_.store(std::thread::id{}, std::memory_order_release);
    }
}

inline iox::error io_context::run_for(std::chrono::nanoseconds duration) noexcept {
    namespace chrono = std::chrono;
    if (in_batch()) {
        return iox::error::from_errno(EDEADLK); // the deadline SQE would sit unflushed
    }
    stopped_ = false;
    const auto deadline = chrono::steady_clock::now() + duration;

    ++run_epoch_;
    deadline_op_.epoch = run_epoch_;
    deadline_op_.timespec.tv_sec = chrono::duration_cast<chrono::seconds>(deadline.time_since_epoch()).count();
    deadline_op_.timespec.tv_nsec =
        (chrono::duration_cast<chrono::nanoseconds>(deadline.time_since_epoch()) % chrono::seconds{1}).count();

    if (io_uring_sqe* sqe = acquire_sqe(deadline_op_)) {
        ::io_uring_prep_timeout(sqe, &deadline_op_.timespec, 0, IORING_TIMEOUT_ABS);
        run();
    }
    if (run_depth_ != 0) {
        return {};
    }
    stopped_ = false;
    ++run_epoch_;
    return {};
}

inline void io_context::deadline_op::on_fired(op_base* self, io_context& context,
                                              std::int32_t, std::uint32_t) noexcept {
    auto& operation = *static_cast<deadline_op*>(self);
    if (operation.epoch == context.run_epoch_) {
        context.stop();
    }
}

}
