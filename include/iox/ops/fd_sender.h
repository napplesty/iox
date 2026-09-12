// iox — unified async IO for Linux
// ops/fd_sender.h — the generic sender/op skeleton every operation builds on.
//
// Every async operation is a CPO returning a P2300 sender. Senders are thin:
// they carry the context, an fd and a per-operation argument pack. Connecting
// produces an operation state whose address rides in the SQE user_data; when
// the CQE returns, io_context::dispatch invokes the op's thunk — a single
// function pointer, no vtable, no allocation (§三.①②⑧).
//
// Structure: one generic fd_sender<Policy>/op skeleton (this file) plus a
// small policy per operation (prep / complete / signatures) in the per-op
// unit headers. This removes hand-copied thunk boilerplate — every operation
// gets identical treatment for error mapping, cancellation and completion
// dispatch.
//
//   * handles dispatch by capability: io::read accepts any readable<H>,
//     io::write any writable<H>; pipe ends expose exactly one direction.
//   * cancellation: ops that see an stdexec::inplace_stop_token in their
//     receiver's environment arm a stop callback that submits
//     IORING_OP_ASYNC_CANCEL; the op then completes set_stopped()
//     (-ECANCELED → set_stopped, never an error).
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

// fd extraction: typed handles expose the fd of the relevant direction.
inline iox::fd reader_fd(iox::fd f) noexcept { return f; }
template <readable H>
iox::fd reader_fd(const H& h) noexcept { return h.read_handle(); }

inline iox::fd writer_fd(iox::fd f) noexcept { return f; }
template <writable H>
iox::fd writer_fd(const H& h) noexcept { return h.write_handle(); }

// ---------------------------------------------------------------------------
// stop-callback storage
//
// stdexec::inplace_stop_callback registers its address with the stop source,
// making it (and anything containing it) non-movable. Operation states must
// stay movable until start (stdexec places them via connect). The slot below
// holds the callback in raw inline storage: the op moves as plain bytes
// before start; the callback is constructed only inside start() (arm) and
// destroyed before completion (reset). After start an op must not move —
// P2300 already requires that.
// ---------------------------------------------------------------------------

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
    // Copies are pre-arm only, by construction: an armed callback is
    // registered with a stop source and tied to ONE storage address, so a
    // copy claims nothing (forcing armed=false also protects the copy's
    // destructor from double-unregistering the original's callback).
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

// ---------------------------------------------------------------------------
// the generic sender/op skeleton
// ---------------------------------------------------------------------------

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
            o.stop_cb.reset(); // P2300: stop callbacks die before completion
            if constexpr (requires { Policy::after(o, res); }) {
                Policy::after(o, res);
            }
            // io_uring delivers -ECANCELED for cancelled operations, but a
            // blocking splice cancelled inside an io-wq worker completes
            // with -ERESTARTSYS (512) instead — both mean "cancelled by
            // request": set_stopped, never an error.
            if (res == -ECANCELED || res == -512) {
                stdexec::set_stopped(std::move(o.r));
            } else {
                Policy::complete::complete(o.r, res, o.args);
            }
        }

        stop_cb_slot stop_cb;

        // Returns true when the token was already stopped at start time —
        // the operation must complete set_stopped() synchronously instead of
        // submitting (a cancel-before-submit would race the kernel state).
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
                // The hook completes SYNCHRONOUSLY — a detached op deletes
                // itself inside it, so *this may be freed the moment it
                // returns true. Release the stop callback FIRST (P2300
                // ordering AND lifetime), run the hook, and if it did NOT
                // complete, restore cancellation before submitting.
                stop_cb.reset();
                if (Policy::immediate(r, args)) {
                    return; // *this may be gone; touch nothing
                }
                if (check_or_arm_stop()) {
                    stdexec::set_stopped(std::move(r));
                    return;
                }
            }
            io_uring_sqe* sqe = ctx->acquire_sqe(*this);
            if (sqe == nullptr) {
                stop_cb.reset();
                // EBUSY for an unusable ring, or the injected code when a
                // test failpoint refused the reservation (§七.4)
                stdexec::set_error(std::move(r), ctx->acquire_error());
                return;
            }
            Policy::prep(sqe, f, args);
        }
    };

    // stdexec connects senders from both value categories; deducing this
    // accepts them all (C++23 explicit object parameter).
    template <class Self, class R>
    auto connect(this Self&& self, R&& r) {
        return op<std::remove_cvref_t<R>>(std::forward<Self>(self),
                                          std::forward<R>(r));
    }
};

} // namespace iox::io::detail
