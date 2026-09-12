// iox — unified async IO for Linux
// include/iox/core/exec.h — the execution layer seam (design §风险: stdexec 隔离).
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
    stdexec::inplace_stop_source internal_stop_source;
    stdexec::inplace_stop_source* stop_source = &internal_stop_source;
    bool done = false;
    using values_variant = stdexec::value_types_of_t<Sndr, stdexec::env<>>;
    static_assert(std::variant_size_v<values_variant> == 1,
                  "iox::exec::sync_wait requires a monomorphic value channel");
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
    while (!st.done) {
        ctx.restart();
        ctx.run();
    }
    ctx.restart();

    if (st.exception) {
        std::rethrow_exception(st.exception);
    }
    using values_t = typename sync_wait_state<Sndr>::values_t;
    const bool stopped = !st.value.has_value() && !st.error.has_value();
    return sync_wait_result<values_t>{std::move(st.value), std::move(st.error), stopped};
}

}

template <class Sndr>
auto sync_wait(io_context& ctx, Sndr&& sndr) {
    return detail::sync_wait_impl(ctx, static_cast<stdexec::inplace_stop_source*>(nullptr),
                                  std::forward<Sndr>(sndr));
}

template <class Sndr>
auto sync_wait(io_context& ctx, stdexec::inplace_stop_source& stop_source, Sndr&& sndr) {
    return detail::sync_wait_impl(ctx, &stop_source, std::forward<Sndr>(sndr));
}

}
