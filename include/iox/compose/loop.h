// iox — unified async IO for Linux
// include/iox/compose/loop.h — io::loop: a repeating sender factory.
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

    decltype(stdexec::get_env(std::declval<const R&>())) get_env() const noexcept {
        return stdexec::get_env(self->r);
    }

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

    alignas(child_op_t) std::byte child_raw[sizeof(child_op_t)];
    bool child_live = false;
    std::optional<hop_op_t> hop;

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

}

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
    template <class F>
    loop_sender<std::decay_t<F>> operator()(io_context& ctx, F&& f) const noexcept {
        return {&ctx, std::forward<F>(f)};
    }
} loop{};

}
