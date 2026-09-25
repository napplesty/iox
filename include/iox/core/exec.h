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

template <class Sender>
struct sync_wait_state {
    io_context* context;
    stdexec::inplace_stop_source internal_stop_source;
    stdexec::inplace_stop_source* stop_source = &internal_stop_source;
    bool done = false;
    using values_variant = stdexec::value_types_of_t<Sender, stdexec::env<>>;
    static_assert(std::variant_size_v<values_variant> == 1,
                  "iox::exec::sync_wait requires a monomorphic value channel");
    using values_t = std::variant_alternative_t<0, values_variant>;
    std::optional<values_t> value;
    std::optional<iox::error> error;
    std::exception_ptr exception;
};

template <class Sender>
struct sync_wait_receiver {
    using receiver_concept = stdexec::receiver_tag;
    using state_t = sync_wait_state<Sender>;
    state_t* state;

    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, state->stop_source->get_token()}};
    }

    void finish() noexcept {
        state->done = true;
        state->context->stop();
    }

    template <class... Args>
    void set_value(Args&&... args) && noexcept {
        state->value.emplace(std::forward<Args>(args)...);
        finish();
    }

    void set_error(iox::error error) && noexcept {
        state->error = error;
        finish();
    }

    void set_error(std::exception_ptr exception) && noexcept {
        state->exception = std::move(exception);
        finish();
    }

    void set_stopped() && noexcept { finish(); }
};

template <class Sender>
auto sync_wait_impl(io_context& context, stdexec::inplace_stop_source* stop_source,
                    Sender&& sender) {
    if (context.in_batch()) {
        using values_t = typename sync_wait_state<Sender>::values_t;
        return sync_wait_result<values_t>{std::nullopt, iox::error::from_errno(EDEADLK),
                                        false};
    }
    sync_wait_state<Sender> state{&context};
    if (stop_source != nullptr) {
        state.stop_source = stop_source;
    }
    auto operation = stdexec::connect(std::forward<Sender>(sender),
                                      sync_wait_receiver<Sender>{&state});
    stdexec::start(operation);
    while (!state.done) {
        context.restart();
        context.run();
    }
    context.restart();

    if (state.exception) {
        std::rethrow_exception(state.exception);
    }
    using values_t = typename sync_wait_state<Sender>::values_t;
    const bool stopped = !state.value.has_value() && !state.error.has_value();
    return sync_wait_result<values_t>{std::move(state.value), std::move(state.error), stopped};
}

}

template <class Sender>
auto sync_wait(io_context& context, Sender&& sender) {
    return detail::sync_wait_impl(context, static_cast<stdexec::inplace_stop_source*>(nullptr),
                                  std::forward<Sender>(sender));
}

template <class Sender>
auto sync_wait(io_context& context, stdexec::inplace_stop_source& stop_source, Sender&& sender) {
    return detail::sync_wait_impl(context, &stop_source, std::forward<Sender>(sender));
}

}
