// iox — unified async IO for Linux
// compose/loop.h — io::loop: a repeating sender factory.
//
// io::loop(ctx, f) runs `f()` — a nullary callable returning a sender whose
// single value is convertible to bool — repeatedly on the io thread until an
// iteration yields true. The re-arm hops through io::schedule(ctx): the next
// iteration is connected from the event loop, after the previous operation
// state has fully unwound. Destroying an operation state from inside its own
// completion is a lifetime trap; the hop makes it structurally impossible.
//
// Cross-iteration state must live in `f`'s capture (it rides inside the loop
// op): a variable declared in the body dangles once the body returns, and
// the chain completes asynchronously afterwards (ADR-005).
#pragma once

#include <optional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/error.h"
#include "iox/ops/schedule.h"
#include "iox/runtime/io_context.h"

namespace iox::io {

namespace loop_detail {

template <class F, class R>
struct loop_op;

template <class F, class R>
struct child_receiver {
    using receiver_concept = stdexec::receiver_tag;
    loop_op<F, R>* self;

    // Forward the downstream environment: stop tokens (cancellation) and any
    // other queries must reach the operations INSIDE the loop — an empty env
    // here would make loop bodies uncancellable (SIGINT graceful exit, M4).
    // Explicit (dependent) return type, not auto: this body is instantiated
    // only at use, when loop_op is complete.
    decltype(stdexec::get_env(std::declval<const R&>())) get_env() const noexcept {
        return stdexec::get_env(self->r);
    }

    // Defensive: the loop sender declares set_value_t() in its completion
    // signatures (that is how IT completes upstream); stdexec's connect-time
    // checks require this receiver shape to accept it too. Iteration
    // completions always arrive via the bool overload below.
    void set_value() && noexcept { stdexec::set_value(std::move(self->r)); }
    template <class V>
    void set_value(V&& done) && noexcept;
    void set_error(iox::error e) && noexcept;
    void set_error(std::exception_ptr e) && noexcept;
    void set_stopped() && noexcept;
};

template <class F, class R>
struct hop_receiver {
    using receiver_concept = stdexec::receiver_tag;
    loop_op<F, R>* self;

    stdexec::env<> get_env() const noexcept { return {}; }
    void set_value() && noexcept { self->arm(); }
    void set_error(iox::error) && noexcept { /* hop cannot fail */ }
    void set_error(std::exception_ptr) && noexcept {}
    void set_stopped() && noexcept {}
};

template <class F, class R>
struct loop_op {
    using child_sender_t = decltype(std::declval<F&>()());
    using child_op_t =
        decltype(stdexec::connect(std::declval<child_sender_t>(),
                                  std::declval<child_receiver<F, R>>()));
    using hop_op_t =
        decltype(stdexec::connect(std::declval<io::detail::schedule_sender>(),
                                  std::declval<hop_receiver<F, R>>()));

    io_context* ctx;
    F f;
    R r;

    // stdexec adaptor op states are immovable (their move constructors are
    // left undefined — they are only ever constructed in place). Placement
    // storage + a prvalue connect() constructs the child without any move;
    // std::optional<child_op_t>::emplace would require the move ctor.
    alignas(child_op_t) std::byte child_raw[sizeof(child_op_t)];
    bool child_live = false;
    std::optional<hop_op_t> hop; // iox's own op states are movable

    using operation_state_concept = stdexec::operation_state_tag;

    loop_op(io_context* ctx, F func, R recv) noexcept
        : ctx(ctx), f(std::move(func)), r(std::move(recv)) {}

    loop_op(const loop_op&) = delete;
    loop_op& operator=(const loop_op&) = delete;

    child_op_t& child() noexcept {
        return *reinterpret_cast<child_op_t*>(&child_raw);
    }

    void destroy_child() noexcept {
        if (child_live) {
            child().~child_op_t();
            child_live = false;
        }
    }

    ~loop_op() { destroy_child(); }

    void arm() noexcept {
        destroy_child();
        ::new (static_cast<void*>(&child_raw))
            child_op_t(stdexec::connect(f(), child_receiver<F, R>{this}));
        child_live = true;
        stdexec::start(child());
    }

    void start() noexcept { arm(); }
};

template <class F, class R>
template <class V>
void child_receiver<F, R>::set_value(V&& done) && noexcept {
    static_assert(std::convertible_to<V&&, bool>,
                  "io::loop: iteration senders must complete with a single "
                  "bool-like value (true = stop looping)");
    if (static_cast<bool>(done)) {
        stdexec::set_value(std::move(self->r));
        return;
    }
    // Not done: hop through the ring; the next arm() runs after this child
    // (and the entire stdexec adaptor chain above it) has unwound.
    self->hop.emplace(stdexec::connect(io::schedule(*self->ctx),
                                       hop_receiver<F, R>{self}));
    stdexec::start(*self->hop);
}

template <class F, class R>
void child_receiver<F, R>::set_error(iox::error e) && noexcept {
    stdexec::set_error(std::move(self->r), e);
}

template <class F, class R>
void child_receiver<F, R>::set_error(std::exception_ptr e) && noexcept {
    stdexec::set_error(std::move(self->r), e);
}

template <class F, class R>
void child_receiver<F, R>::set_stopped() && noexcept {
    stdexec::set_stopped(std::move(self->r));
}

} // namespace loop_detail

template <class F>
struct loop_sender {
    io_context* ctx;
    F f;

    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_error_t(iox::error),
        stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

    template <class Self, class R>
    auto connect(this Self&& self, R&& r) {
        return loop_detail::loop_op<std::decay_t<F>, std::remove_cvref_t<R>>(
            self.ctx, std::forward<Self>(self).f, std::forward<R>(r));
    }
};

inline constexpr struct loop_t {
    /// Repeat `f()` on the io thread until an iteration yields true.
    /// `f` must not throw; iteration senders complete with one bool-like
    /// value, or error/stop (which ends the loop by propagation).
    template <class F>
    loop_sender<std::decay_t<F>> operator()(io_context& ctx, F&& f) const noexcept {
        return {&ctx, std::forward<F>(f)};
    }
} loop{};

} // namespace iox::io
