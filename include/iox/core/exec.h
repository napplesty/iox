// iox — unified async IO for Linux
// exec.h — the execution layer seam (design §风险: stdexec 隔离).
//
// All iox code and all user code depends on `iox::exec`, never on stdexec
// directly. stdexec (NVIDIA's P2300 reference implementation) is the current
// backing implementation; when libstdc++'s std::execution is complete enough
// the alias flips and user code is unaffected (ADR-002).
//
// iox::exec::sync_wait differs from stdexec::sync_wait on purpose: our
// completions are produced by io_uring, so the *io_context must be pumped on
// the very thread that waits*. stdexec's sync_wait runs its own internal
// run_loop, which would deadlock against that; ours runs ctx.run().
#pragma once

#include <exception>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "iox/core/error.h"
#include "iox/runtime/io_context.h"

namespace iox::exec {

using namespace stdexec;

/// Result of sync_wait: a value (as a tuple of the sender's value channel),
/// an iox::error, an exception, or a stopped signal.
template <class Vals>
struct sync_wait_result {
    std::optional<Vals> value;
    std::optional<iox::error> error;
    bool stopped = false;

    explicit operator bool() const noexcept { return value.has_value(); }
    Vals& operator*() & noexcept { return *value; }
    const Vals& operator*() const& noexcept { return *value; }
};

namespace detail {

template <class Sndr>
struct sync_wait_state {
    io_context* ctx;
    // Stop source exposed to the sender tree via the receiver environment:
    // vocabulary ops arm IORING_OP_ASYNC_CANCEL from this token (M2).
    // sync_wait(ctx, stop_source, sender) lets the caller trigger it externally.
    stdexec::inplace_stop_source internal_stop_source;
    stdexec::inplace_stop_source* stop_source = &internal_stop_source;
    bool done = false;
    // variant<tuple<Vs...>...> over every set_value signature; sync_wait
    // requires exactly one (all iox vocabulary senders are monomorphic).
    using values_variant = stdexec::value_types_of_t<Sndr, stdexec::env<>>;
    static_assert(std::variant_size_v<values_variant> == 1,
                  "iox::exec::sync_wait requires a monomorphic value channel");
    // libstdc++ (GCC 15) does not specialize std::tuple_element for variant;
    // variant_alternative_t is the portable accessor.
    using values_t = std::variant_alternative_t<0, values_variant>;
    std::optional<values_t> value;
    std::optional<iox::error> error;
    std::exception_ptr exception;
};

template <class Sndr>
struct sync_wait_receiver {
    using receiver_concept = stdexec::receiver_tag;
    using state_t = sync_wait_state<Sndr>;
    state_t* st;

    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, st->stop_source->get_token()}};
    }

    void finish() noexcept {
        st->done = true;
        st->ctx->stop();
    }

    template <class... As>
    void set_value(As&&... as) && noexcept {
        st->value.emplace(std::forward<As>(as)...);
        finish();
    }

    void set_error(iox::error e) && noexcept {
        st->error = e;
        finish();
    }

    void set_error(std::exception_ptr e) && noexcept {
        st->exception = std::move(e);
        finish();
    }

    void set_stopped() && noexcept { finish(); }
};

template <class Sndr>
auto sync_wait_impl(io_context& ctx, stdexec::inplace_stop_source* stop_source,
                    Sndr&& sndr) {
    // A live batch_scope on this same thread is holding every submission:
    // run() refuses to pump behind it, so this call could never complete —
    // it would spin forever (stress a2e). Reject BEFORE connecting/starting
    // the operation: once started, its SQE is already in the ring and the
    // op state must stay alive until the CQE (red-team t8 class).
    if (ctx.in_batch()) {
        using values_t = typename sync_wait_state<Sndr>::values_t;
        return sync_wait_result<values_t>{std::nullopt, iox::error::from_errno(EDEADLK),
                                        false};
    }
    sync_wait_state<Sndr> st{&ctx};
    if (stop_source != nullptr) {
        st.stop_source = stop_source;
    }
    auto op = stdexec::connect(std::forward<Sndr>(sndr),
                               sync_wait_receiver<Sndr>{&st});
    stdexec::start(op);
    // Drive until THIS sender completes — not merely until run() returns.
    // Anything on the loop may call ctx.stop() (watchdogs, unrelated
    // completions); treating that as completion destroyed the still-in-flight
    // op with this frame and the next pump dispatched its CQE into dead
    // memory (red-team t8: stack-use-after-return). A stop() here just ends
    // one run() pass; we restart and keep pumping. Cancellation of the
    // operation itself goes through the stop token, which completes ops.
    while (!st.done) {
        ctx.restart();
        ctx.run();
    }
    ctx.restart();

    if (st.exception) {
        std::rethrow_exception(st.exception);
    }
    using values_t = typename sync_wait_state<Sndr>::values_t;
    // A completion that carries neither a value nor an error completed
    // set_stopped (every channel runs finish()). An engaged empty tuple is
    // still a VALUE — void-completing senders are not "stopped".
    const bool stopped = !st.value.has_value() && !st.error.has_value();
    return sync_wait_result<values_t>{std::move(st.value), std::move(st.error), stopped};
}

} // namespace detail

/// Start `sndr` and drive `ctx` on this thread until it completes. The
/// context is left in a runnable (restarted) state on return.
///
/// Requires a monomorphic value channel (exactly one set_value signature) —
/// true for all iox vocabulary senders. If the sender errors out, the result
/// carries the error instead of throwing; stdexec algorithm exceptions are
/// rethrown after the loop exits.
template <class Sndr>
auto sync_wait(io_context& ctx, Sndr&& sndr) {
    return detail::sync_wait_impl(ctx, static_cast<stdexec::inplace_stop_source*>(nullptr),
                                  std::forward<Sndr>(sndr));
}

/// Cancellable variant: `stop_source` is exposed to the sender tree as the stop
/// token — request_stop() on it (from a completion on the io thread) cancels
/// in-flight vocabulary operations through IORING_OP_ASYNC_CANCEL.
template <class Sndr>
auto sync_wait(io_context& ctx, stdexec::inplace_stop_source& stop_source, Sndr&& sndr) {
    return detail::sync_wait_impl(ctx, &stop_source, std::forward<Sndr>(sndr));
}

} // namespace iox::exec
