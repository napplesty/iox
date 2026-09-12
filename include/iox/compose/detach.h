// iox — unified async IO for Linux
// include/iox/compose/detach.h — exec::detach: run a sender to completion, fire-and-
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

}

template <class Sndr>
void detach(Sndr&& sndr) {
    auto* d = new detach_detail::detached_op<std::decay_t<Sndr>>(std::forward<Sndr>(sndr));
    stdexec::start(d->op);
}

}
