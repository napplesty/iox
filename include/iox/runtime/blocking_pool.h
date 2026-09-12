// iox — unified async IO for Linux
// include/iox/runtime/blocking_pool.h — the blocking escape hatch (design §二 L2).
#pragma once

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/runtime/io_context.h"
#include "iox/ops/poll.h"

namespace iox {

class blocking_pool {
public:
    explicit blocking_pool(io_context& ctx, unsigned threads = 1) : ctx_(&ctx) {
        efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd_ < 0) {
            return;
        }
        for (unsigned i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        arm_wakeup();
    }

    ~blocking_pool() {
        {
            std::lock_guard lock{mtx_};
            stopping_ = true;
            cv_.notify_all();
        }
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
        if (efd_ >= 0) {
            if (wakeup_) {
                const std::uint64_t one = 1;
                (void)!::write(efd_, &one, sizeof(one));
                ctx_->flush();
                ctx_->run_for(std::chrono::milliseconds(100));
            }
            wakeup_.reset();
            {
                std::lock_guard lock{mtx_};
                for (task_base* t : done_) {
                    delete t;
                }
                done_.clear();
            }
            ::close(efd_);
        }
    }

    blocking_pool(const blocking_pool&) = delete;
    blocking_pool& operator=(const blocking_pool&) = delete;

    bool ok() const noexcept { return efd_ >= 0; }

    struct task_base {
        virtual void execute() noexcept = 0;
        virtual void deliver() noexcept = 0; // io thread
        virtual ~task_base() = default;
    };

    template <class F>
    struct sender {
        blocking_pool* pool;
        F f;

        using value_t = std::decay_t<std::invoke_result_t<F&>>;
        static_assert(!std::is_void_v<value_t>,
                      "blocking_pool::run callables must return a value");

        using sender_concept = stdexec::sender_tag;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(value_t), stdexec::set_error_t(iox::error),
            stdexec::set_error_t(std::exception_ptr)>;

        template <class R>
        struct op final {
            blocking_pool* pool;
            F func;
            void* held = nullptr;
            R r;

            using operation_state_concept = stdexec::operation_state_tag;

            op(sender s, R&& recv) : pool(s.pool), func(std::move(s.f)),
                                     r(std::forward<R>(recv)) {}

            void start() noexcept;
        };

        template <class R>
        struct cell final : task_base {
            using op_t = op<R>;

            F func;
            std::optional<value_t> value;
            std::exception_ptr exception;
            op_t* owner;

            cell(F func_, op_t* o) : func(std::move(func_)), owner(o) {}

            void execute() noexcept override {
                try {
                    value.emplace(func());
                } catch (...) {
                    exception = std::current_exception();
                }
            }

            void deliver() noexcept override {
                op_t* o = owner;
                auto e = std::move(exception);
                std::optional<value_t> v = std::move(value);
                delete this;
                if (e) {
                    stdexec::set_error(std::move(o->r), std::move(e));
                } else {
                    stdexec::set_value(std::move(o->r), std::move(*v));
                }
            }
        };

        template <class Self, class R>
        auto connect(this Self&& s, R&& r) {
            return op<std::remove_cvref_t<R>>(std::forward<Self>(s),
                                              std::forward<R>(r));
        }
    };

    template <class F>
    sender<std::decay_t<F>> run(F&& f) {
        return {this, std::forward<F>(f)};
    }

private:
    void submit(task_base* t) noexcept {
        {
            std::lock_guard lock{mtx_};
            pending_.push_back(t);
        }
        cv_.notify_one();
    }

    void worker_loop() noexcept {
        for (;;) {
            task_base* t = nullptr;
            {
                std::unique_lock lock{mtx_};
                cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (pending_.empty()) {
                    return;
                }
                t = pending_.front();
                pending_.pop_front();
            }
            t->execute();
            {
                std::lock_guard lock{mtx_};
                done_.push_back(t);
            }
            const std::uint64_t one = 1;
            (void)!::write(efd_, &one, sizeof(one));
        }
    }

    struct wakeup_receiver {
        using receiver_concept = stdexec::receiver_tag;
        blocking_pool* pool;

        stdexec::env<> get_env() const noexcept { return {}; }
        void set_value(std::uint32_t) && noexcept { pool->drain_and_rearm(); }
        void set_error(iox::error) && noexcept {}
        void set_error(std::exception_ptr) && noexcept {}
        void set_stopped() && noexcept {}
    };

    void arm_wakeup() noexcept {
        wakeup_.emplace(stdexec::connect(io::poll(*ctx_, iox::fd{efd_}, POLLIN),
                                         wakeup_receiver{this}));
        stdexec::start(*wakeup_);
    }

    void drain_and_rearm() noexcept {
        std::uint64_t count = 0;
        (void)!::read(efd_, &count, sizeof(count));
        std::deque<task_base*> ready;
        {
            std::lock_guard lock{mtx_};
            ready.swap(done_);
        }
        for (task_base* t : ready) {
            t->deliver();
        }
        if (!stopping_) {
            arm_wakeup();
        }
    }

    using wakeup_op_t = decltype(stdexec::connect(
        io::poll(std::declval<io_context&>(), iox::fd{}, static_cast<short>(POLLIN)),
        std::declval<wakeup_receiver>()));

    io_context* ctx_ = nullptr;
    int efd_ = -1;
    std::vector<std::thread> workers_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<task_base*> pending_;
    std::deque<task_base*> done_;
    std::optional<wakeup_op_t> wakeup_;
    bool stopping_ = false;
};

template <class F>
template <class R>
void blocking_pool::sender<F>::op<R>::start() noexcept {
    using cell_t = typename blocking_pool::sender<F>::template cell<R>;
    auto* c = new (std::nothrow) cell_t{std::move(func), this};
    if (c == nullptr) {
        stdexec::set_error(std::move(r), iox::error::from_errno(ENOMEM));
        return;
    }
    held = c;
    pool->submit(c);
}

}
