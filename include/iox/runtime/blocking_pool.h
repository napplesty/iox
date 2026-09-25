// iox — unified async IO for Linux
// include/iox/runtime/blocking_pool.h — the blocking escape hatch (design §二 L2).
#pragma once

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <atomic>
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
    explicit blocking_pool(io_context& context, unsigned threads = 1) : context_(&context) {
        efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd_ < 0) {
            open_error_ = errno;
            return;
        }
        for (unsigned index = 0; index < threads; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        arm_wakeup();
    }

    ~blocking_pool() {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
            condition_.notify_all();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (efd_ >= 0) {
            if (wakeup_) {
                const std::uint64_t one = 1;
                (void)!::write(efd_, &one, sizeof(one));
                context_->flush();
                context_->run_for(std::chrono::milliseconds(100));
            }
            wakeup_.reset();
            for (task_base* task : done_) { // workers are joined: deliver, never strand a receiver
                task->deliver();
            }
            done_.clear();
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

    template <class Function>
    struct sender {
        blocking_pool* pool;
        Function function;

        using value_t = std::decay_t<std::invoke_result_t<Function&>>;
        static_assert(!std::is_void_v<value_t>,
                      "blocking_pool::run callables must return a value");

        using sender_concept = stdexec::sender_tag;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(value_t), stdexec::set_error_t(iox::error),
            stdexec::set_error_t(std::exception_ptr)>;

        template <class Receiver>
        struct op final {
            blocking_pool* pool;
            Function function;
            void* held = nullptr;
            Receiver receiver;

            using operation_state_concept = stdexec::operation_state_tag;

            op(sender source, Receiver&& receiver) : pool(source.pool), function(std::move(source.function)),
                                     receiver(std::forward<Receiver>(receiver)) {}

            void start() noexcept;
        };

        template <class Receiver>
        struct cell final : task_base {
            using op_t = op<Receiver>;

            Function function;
            std::optional<value_t> value;
            std::exception_ptr exception;
            op_t* owner;

            cell(Function function, op_t* owner) : function(std::move(function)), owner(owner) {}

            void execute() noexcept override {
                try {
                    value.emplace(function());
                } catch (...) {
                    exception = std::current_exception();
                }
            }

            void deliver() noexcept override {
                op_t* operation = owner;
                auto failure = std::move(exception);
                std::optional<value_t> result = std::move(value);
                delete this;
                if (failure) {
                    stdexec::set_error(std::move(operation->receiver), std::move(failure));
                } else {
                    stdexec::set_value(std::move(operation->receiver), std::move(*result));
                }
            }
        };

        template <class Self, class Receiver>
        auto connect(this Self&& self, Receiver&& receiver) {
            return op<std::remove_cvref_t<Receiver>>(std::forward<Self>(self),
                                                     std::forward<Receiver>(receiver));
        }
    };

    template <class Function>
    sender<std::decay_t<Function>> run(Function&& function) {
        return {this, std::forward<Function>(function)};
    }

private:
    bool usable() const noexcept { return efd_ >= 0 && !workers_.empty(); }

    void submit(task_base* task) noexcept {
        {
            std::lock_guard lock{mutex_};
            pending_.push_back(task);
        }
        condition_.notify_one();
    }

    void worker_loop() noexcept {
        for (;;) {
            task_base* task = nullptr;
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (pending_.empty()) {
                    return;
                }
                task = pending_.front();
                pending_.pop_front();
            }
            task->execute();
            {
                std::lock_guard lock{mutex_};
                done_.push_back(task);
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
        wakeup_.emplace(stdexec::connect(io::poll(*context_, iox::fd{efd_}, POLLIN),
                                         wakeup_receiver{this}));
        stdexec::start(*wakeup_);
    }

    void drain_and_rearm() noexcept {
        std::uint64_t count = 0;
        (void)!::read(efd_, &count, sizeof(count));
        std::deque<task_base*> ready;
        {
            std::lock_guard lock{mutex_};
            ready.swap(done_);
        }
        for (task_base* task : ready) {
            task->deliver();
        }
        if (!stopping_) {
            arm_wakeup();
        }
    }

    using wakeup_op_t = decltype(stdexec::connect(
        io::poll(std::declval<io_context&>(), iox::fd{}, static_cast<short>(POLLIN)),
        std::declval<wakeup_receiver>()));

    io_context* context_ = nullptr;
    int efd_ = -1;
    int open_error_ = 0;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<task_base*> pending_;
    std::deque<task_base*> done_;
    std::optional<wakeup_op_t> wakeup_;
    std::atomic<bool> stopping_{false};
};

template <class Function>
template <class Receiver>
void blocking_pool::sender<Function>::op<Receiver>::start() noexcept {
    if (!pool->usable()) {
        stdexec::set_error(std::move(receiver),
                           iox::error::from_errno(pool->open_error_ != 0 ? pool->open_error_ : ENODEV));
        return;
    }
    using cell_t = typename blocking_pool::sender<Function>::template cell<Receiver>;
    auto* cell = new (std::nothrow) cell_t{std::move(function), this};
    if (cell == nullptr) {
        stdexec::set_error(std::move(receiver), iox::error::from_errno(ENOMEM));
        return;
    }
    held = cell;
    pool->submit(cell);
}

}
