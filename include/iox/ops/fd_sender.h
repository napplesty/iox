// iox — ops/fd_sender.h: the generic sender/op skeleton every fd operation builds on.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/concepts.h"
#include "iox/core/cpo.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/ops/completers.h"
#include "iox/runtime/io_context.h"

namespace iox::io::detail {

inline iox::fd reader_fd(iox::fd fd) noexcept { return fd; }
template <readable Handle>
iox::fd reader_fd(const Handle& handle) noexcept { return handle.read_handle(); }

inline iox::fd writer_fd(iox::fd fd) noexcept { return fd; }
template <writable Handle>
iox::fd writer_fd(const Handle& handle) noexcept { return handle.write_handle(); }

template <class Policy>
concept op_policy = requires(io_uring_sqe* sqe, iox::fd fd, typename Policy::args_t& args) {
    typename Policy::signatures;
    typename Policy::complete;
    { Policy::prep(sqe, fd, args) } noexcept;
};

template <class Args, auto Prep, class Complete = transfer_complete>
struct basic_policy {
    using args_t = Args;
    using signatures = io_signatures<std::size_t>;
    static void prep(io_uring_sqe* sqe, iox::fd fd, args_t& args) noexcept { Prep(sqe, fd, args); }
    using complete = Complete;
};

struct cancel_fn {
    io_context* context;
    op_base* target;
    void operator()() const noexcept {
        context->submit_cancel(reinterpret_cast<std::uint64_t>(target));
    }
};

using stop_cb_t = stdexec::inplace_stop_callback<cancel_fn>;

struct stop_cb_slot {
    alignas(stop_cb_t) std::byte storage[sizeof(stop_cb_t)];
    bool armed = false;

    stop_cb_slot() noexcept = default;
    stop_cb_slot(const stop_cb_slot&) noexcept : armed(false) {}
    stop_cb_slot& operator=(const stop_cb_slot&) noexcept {
        reset();
        return *this;
    }
    ~stop_cb_slot() { reset(); }

    void arm(stdexec::inplace_stop_token token, cancel_fn cancel) noexcept {
        ::new (static_cast<void*>(storage)) stop_cb_t(token, cancel);
        armed = true;
    }
    void reset() noexcept {
        if (armed) {
            reinterpret_cast<stop_cb_t*>(storage)->~stop_cb_t();
            armed = false;
        }
    }
};

template <op_policy Policy>
struct fd_sender {
    io_context* context = nullptr;
    iox::fd fd{};
    typename Policy::args_t args{};

    using sender_concept = stdexec::sender_tag;
    using completion_signatures = typename Policy::signatures;

    template <class Receiver>
    struct op final : op_base {
        io_context* context;
        iox::fd fd;
        typename Policy::args_t args;
        Receiver receiver;

        using operation_state_concept = stdexec::operation_state_tag;

        op(fd_sender sender, Receiver&& receiver) noexcept
            : op_base(&op::on_cqe), context(sender.context), fd(sender.fd), args(sender.args),
              receiver(std::move(receiver)) {}

        static void on_cqe(op_base* self, io_context&, std::int32_t result,
                           std::uint32_t) noexcept {
            auto& operation = *static_cast<op*>(self);
            operation.stop_callback.reset();
            if constexpr (requires { Policy::after(operation, result); }) {
                Policy::after(operation, result);
            }
            if (result == -ECANCELED || result == -512) {
                stdexec::set_stopped(std::move(operation.receiver));
            } else {
                Policy::complete::complete(operation.receiver, result, operation.args);
            }
        }

        stop_cb_slot stop_callback;

        bool check_or_arm_stop() noexcept {
            if constexpr (requires { stdexec::get_stop_token(stdexec::get_env(receiver)); }) {
                auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
                if constexpr (std::same_as<std::remove_cvref_t<decltype(token)>,
                                           stdexec::inplace_stop_token>) {
                    if (token.stop_possible()) {
                        if (token.stop_requested()) {
                            return true;
                        }
                        stop_callback.arm(token, cancel_fn{context, static_cast<op_base*>(this)});
                    }
                }
            }
            return false;
        }

        void start() noexcept {
            if (check_or_arm_stop()) {
                stdexec::set_stopped(std::move(receiver));
                return;
            }
            if constexpr (requires { Policy::immediate(receiver, args); }) {
                stop_callback.reset();
                if (Policy::immediate(receiver, args)) {
                    return;
                }
                if (check_or_arm_stop()) {
                    stdexec::set_stopped(std::move(receiver));
                    return;
                }
            }
            io_uring_sqe* sqe = context->acquire_sqe(*this);
            if (sqe == nullptr) {
                stop_callback.reset();
                stdexec::set_error(std::move(receiver), context->acquire_error());
                return;
            }
            Policy::prep(sqe, fd, args);
        }
    };

    template <class Self, class Receiver>
    auto connect(this Self&& self, Receiver&& receiver) {
        return op<std::remove_cvref_t<Receiver>>(std::forward<Self>(self),
                                                 std::forward<Receiver>(receiver));
    }
};

}
