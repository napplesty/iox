// iox — unified async IO for Linux
// include/iox/ops/completers.h — the completion helpers policies plug into
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

struct transfer_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_value(std::move(r), static_cast<std::size_t>(res));
        }
    }
};

struct revents_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_value(std::move(r), static_cast<std::uint32_t>(res));
        }
    }
};

struct deadline_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A&) noexcept {
        if (res == -ETIME || res == 0) {
            stdexec::set_value(std::move(r));
        } else {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        }
    }
};

}
