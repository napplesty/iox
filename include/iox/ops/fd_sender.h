// iox — unified async IO for Linux
// include/iox/ops/fd_sender.h — the generic sender/op skeleton every operation builds on.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/concepts.h"
#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/ops/completers.h"
#include "iox/runtime/io_context.h"

namespace iox::io::detail {

inline iox::fd reader_fd(iox::fd f) noexcept { return f; }
template <readable H>
iox::fd reader_fd(const H& h) noexcept { return h.read_handle(); }

inline iox::fd writer_fd(iox::fd f) noexcept { return f; }
template <writable H>
iox::fd writer_fd(const H& h) noexcept { return h.write_handle(); }

struct cancel_fn {
    io_context* ctx;
    op_base* target;
    void operator()() const noexcept {
        ctx->submit_cancel(reinterpret_cast<std::uint64_t>(target));
    }
};

using stop_cb_t = stdexec::inplace_stop_callback<cancel_fn>;

struct stop_cb_slot {
    alignas(stop_cb_t) std::byte raw[sizeof(stop_cb_t)];
    bool armed = false;

    stop_cb_slot() noexcept = default;
    stop_cb_slot(const stop_cb_slot&) noexcept : armed(false) {}
    stop_cb_slot& operator=(const stop_cb_slot&) noexcept {
        reset();
        return *this;
    }
    ~stop_cb_slot() { reset(); }

    void arm(stdexec::inplace_stop_token token, cancel_fn fn) noexcept {
        ::new (static_cast<void*>(raw)) stop_cb_t(token, fn);
        armed = true;
    }
    void reset() noexcept {
        if (armed) {
            reinterpret_cast<stop_cb_t*>(raw)->~stop_cb_t();
            armed = false;
        }
    }
};

template <class Policy>
struct fd_sender {
    io_context* ctx = nullptr;
    iox::fd f{};
    typename Policy::args_t args{};

    using sender_concept = stdexec::sender_tag;
    using completion_signatures = typename Policy::signatures;

    template <class R>
    struct op final : op_base {
        io_context* ctx;
        iox::fd f;
        typename Policy::args_t args;
        R r;

        using operation_state_concept = stdexec::operation_state_tag;

        op(fd_sender s, R&& recv) noexcept
            : op_base(&op::on_cqe), ctx(s.ctx), f(s.f), args(s.args),
              r(std::move(recv)) {}

        static void on_cqe(op_base* self, io_context&, std::int32_t res,
                           std::uint32_t) noexcept {
            auto& o = *static_cast<op*>(self);
            o.stop_cb.reset();
            if constexpr (requires { Policy::after(o, res); }) {
                Policy::after(o, res);
            }
            if (res == -ECANCELED || res == -512) {
                stdexec::set_stopped(std::move(o.r));
            } else {
                Policy::complete::complete(o.r, res, o.args);
            }
        }

        stop_cb_slot stop_cb;

        bool check_or_arm_stop() noexcept {
            if constexpr (requires { stdexec::get_stop_token(stdexec::get_env(r)); }) {
                auto token = stdexec::get_stop_token(stdexec::get_env(r));
                if constexpr (std::same_as<std::remove_cvref_t<decltype(token)>,
                                           stdexec::inplace_stop_token>) {
                    if (token.stop_possible()) {
                        if (token.stop_requested()) {
                            return true;
                        }
                        stop_cb.arm(token, cancel_fn{ctx, static_cast<op_base*>(this)});
                    }
                }
            }
            return false;
        }

        void start() noexcept {
            if (check_or_arm_stop()) {
                stdexec::set_stopped(std::move(r));
                return;
            }
            if constexpr (requires { Policy::immediate(r, args); }) {
                stop_cb.reset();
                if (Policy::immediate(r, args)) {
                    return;
                }
                if (check_or_arm_stop()) {
                    stdexec::set_stopped(std::move(r));
                    return;
                }
            }
            io_uring_sqe* sqe = ctx->acquire_sqe(*this);
            if (sqe == nullptr) {
                stop_cb.reset();
                stdexec::set_error(std::move(r), ctx->acquire_error());
                return;
            }
            Policy::prep(sqe, f, args);
        }
    };

    template <class Self, class R>
    auto connect(this Self&& self, R&& r) {
        return op<std::remove_cvref_t<R>>(std::forward<Self>(self),
                                          std::forward<R>(r));
    }
};

}
