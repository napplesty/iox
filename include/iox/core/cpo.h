// iox — core/cpo.h: the vocabulary customization protocol + shared signature algebra.
#pragma once

#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/error.h"

namespace iox::io {

template <class Tag, class... Args>
concept tag_invocable = requires(Tag&& tag, Args&&... args) {
    tag_invoke(std::forward<Tag>(tag), std::forward<Args>(args)...);
};

template <class Tag>
struct cpo {
    template <class... Args>
    requires tag_invocable<cpo, Args...>
    constexpr decltype(auto) operator()(Args&&... args) const
        noexcept(noexcept(tag_invoke(*this, std::forward<Args>(args)...))) {
        return tag_invoke(*this, std::forward<Args>(args)...);
    }
};

template <class... Values>
using io_signatures =
    stdexec::completion_signatures<stdexec::set_value_t(Values...),
                                   stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;

}
