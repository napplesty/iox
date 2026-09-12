// iox — unified async IO for Linux
// ops/completers.h — the completion helpers policies plug into
// fd_sender's skeleton: res<0 → set_error, res>=0 → a value shape per
// operation family.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/error.h"

namespace iox::io::detail {

struct void_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_value(std::move(r));
        }
    }
};

struct transfer_complete { // read/write families: value = byte count
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_value(std::move(r), static_cast<std::size_t>(res));
        }
    }
};

struct revents_complete { // poll: value = revents bitmask
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_value(std::move(r), static_cast<std::uint32_t>(res));
        }
    }
};

struct deadline_complete { // timeout: -ETIME means "fired", i.e. success
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res == -ETIME || res == 0) {
            stdexec::set_value(std::move(r));
        } else {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        }
    }
};

} // namespace iox::io::detail
