// iox — unified async IO for Linux
// include/iox/compose/write_all.h — io::write_all: the streaming-write combinator.
#pragma once

#include <concepts>

#include "iox/compose/loop.h"
#include "iox/core/buffer.h"
#include "iox/core/concepts.h"
#include "iox/core/fd.h"
#include "iox/ops/write.h"

namespace iox::io {

namespace detail {
inline auto write_all_impl(io_context* ctx, iox::fd f, rbytes source, bool use_send) {
    return loop(*ctx, [ctx, f, source, use_send, off = std::size_t{0}]() mutable {
        const std::size_t remain = source.size() - off;
        return detail::fd_sender<detail::write_policy>{
                   ctx, f, {rbytes{source.data() + off, remain}, use_send}}
             | stdexec::then([&off, source](std::size_t w) {
                   off += w;
                   return off >= source.size();
               });
    });
}
}

inline constexpr struct write_all_t {
    template <class H>
    requires std::same_as<std::remove_cvref_t<H>, iox::fd> || writable<std::remove_cvref_t<H>>
    auto operator()(io_context& ctx, H&& h, rbytes source) const noexcept {
        return detail::write_all_impl(&ctx, detail::writer_fd(h), source,
                                      detail::is_message_based<H>);
    }
} write_all{};

}
