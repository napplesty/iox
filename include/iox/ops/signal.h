// iox — ops/signal.h: io::signal(context, signalfd handle) → the consumed signalfd_siginfo.
#pragma once

#include <sys/signalfd.h>

#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct siginfo_complete {
    template <class Receiver, class Args>
    static void complete(Receiver& receiver, std::int32_t result, const Args& args) noexcept {
        if (result < 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_negative(result));
        } else {
            stdexec::set_value(std::move(receiver), args.info);
        }
    }
};

struct signal_args {
    ::signalfd_siginfo info{};
};

inline void prep_signal(io_uring_sqe* sqe, iox::fd handle, signal_args& args) noexcept {
    ::io_uring_prep_read(sqe, handle.v, &args.info, sizeof(args.info), 0);
}

struct signal_policy : basic_policy<signal_args, prep_signal, siginfo_complete> {
    using signatures = io_signatures<::signalfd_siginfo>;
};

}

struct signal_tag {};
using signal_t = cpo<signal_tag>;
inline constexpr signal_t signal{};

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd> || readable<std::remove_cvref_t<Handle>>
auto tag_invoke(signal_t, io_context& context, Handle&& handle) noexcept {
    return detail::fd_sender<detail::signal_policy>{&context, detail::reader_fd(handle), {}};
}

}
