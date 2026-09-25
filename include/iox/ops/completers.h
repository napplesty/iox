// iox — ops/completers.h: CQE result → receiver completion, policies plug into these.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/error.h"

namespace iox::io::detail {

template <auto OnOk>
struct completer {
    template <class Receiver, class Args>
    static void complete(Receiver& receiver, std::int32_t result, const Args&) noexcept {
        if (result < 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_negative(result));
        } else {
            OnOk(std::move(receiver), result);
        }
    }
};

struct as_void {
    template <class Receiver>
    void operator()(Receiver&& receiver, std::int32_t) const noexcept {
        stdexec::set_value(std::forward<Receiver>(receiver));
    }
};
struct as_size {
    template <class Receiver>
    void operator()(Receiver&& receiver, std::int32_t count) const noexcept {
        stdexec::set_value(std::forward<Receiver>(receiver), static_cast<std::size_t>(count));
    }
};
struct as_revents {
    template <class Receiver>
    void operator()(Receiver&& receiver, std::int32_t revents) const noexcept {
        stdexec::set_value(std::forward<Receiver>(receiver), static_cast<std::uint32_t>(revents));
    }
};

using void_complete = completer<as_void{}>;
using transfer_complete = completer<as_size{}>;
using revents_complete = completer<as_revents{}>;

struct deadline_complete {
    template <class Receiver, class Args>
    static void complete(Receiver& receiver, std::int32_t result, const Args&) noexcept {
        if (result == -ETIME || result == 0) {
            stdexec::set_value(std::move(receiver));
        } else {
            stdexec::set_error(std::move(receiver), iox::error::from_negative(result));
        }
    }
};

}
