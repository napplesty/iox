// iox — unified async IO for Linux
// compose/detach.h — exec::detach: run a sender to completion, fire-and-
// forget.
//
// The operation state is heap-owned and frees itself on any completion.
// Keep all referenced state alive until the sender completes (session
// objects own themselves). Session-per-connection servers are the intended
// use: one allocation per connection on the control path — the data path
// inside each loop iteration stays allocation-free.
#pragma once

#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/error.h"

namespace iox::exec {
namespace detach_detail {

template <class Sndr>
struct detached_op;

template <class Sndr>
struct done_receiver {
    using receiver_concept = stdexec::receiver_tag;
    detached_op<Sndr>* self;

    stdexec::env<> get_env() const noexcept { return {}; }
    void set_value() && noexcept { delete self; }
    template <class... As>
    void set_value(As&&...) && noexcept {
        delete self;
    }
    void set_error(iox::error) && noexcept { delete self; }
    void set_error(std::exception_ptr) && noexcept { delete self; }
    void set_stopped() && noexcept { delete self; }
};

template <class Sndr>
struct detached_op {
    using op_t = decltype(stdexec::connect(std::declval<Sndr>(),
                                           std::declval<done_receiver<Sndr>>()));
    op_t op;
    explicit detached_op(Sndr&& s)
        : op(stdexec::connect(std::move(s), done_receiver<Sndr>{this})) {}
};

} // namespace detach_detail

/// Start `sndr` detached on the calling thread's loop; the operation state
/// is heap-owned and self-deletes on any completion. Keep all referenced
/// state alive until the sender completes (session objects own themselves).
template <class Sndr>
void detach(Sndr&& sndr) {
    auto* d = new detach_detail::detached_op<std::decay_t<Sndr>>(std::forward<Sndr>(sndr));
    stdexec::start(d->op);
}

} // namespace iox::exec
