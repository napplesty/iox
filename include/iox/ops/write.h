// iox — ops/write.h: io::write(context, handle, rbytes | registered_buffer); message-based
// handles submit SEND|MSG_NOSIGNAL instead of WRITE.
#pragma once

#include <sys/socket.h>
#include <type_traits>

#include "iox/core/buffer.h"
#include "iox/ops/fd_sender.h"
#include "iox/core/cpo.h"

namespace iox::io {

namespace detail {

struct write_policy {
    struct args_t {
        rbytes source{};
        bool use_send = false;
        int fail = 0; // write_all injects a stalled-iteration error here
    };
    using signatures = io_signatures<std::size_t>;
    template <class Receiver>
    static bool immediate(Receiver& receiver, args_t& args) noexcept {
        if (args.fail != 0) {
            stdexec::set_error(std::move(receiver), iox::error::from_errno(args.fail));
            return true;
        }
        if (args.source.size() == 0) {
            stdexec::set_value(std::move(receiver), std::size_t{0});
            return true;
        }
        return false;
    }
    static void prep(io_uring_sqe* sqe, iox::fd fd, args_t& args) noexcept {
        if (args.use_send) {
            ::io_uring_prep_send(sqe, fd.v, args.source.data(), args.source.size(), MSG_NOSIGNAL);
        } else {
            ::io_uring_prep_write(sqe, fd.v, args.source.data(), args.source.size(), -1);
        }
    }
    using complete = transfer_complete;
};

struct write_fixed_args {
    registered_buffer buffer{};
};
inline void prep_write_fixed(io_uring_sqe* sqe, iox::fd fd, write_fixed_args& args) noexcept {
    ::io_uring_prep_write_fixed(sqe, fd.v, args.buffer.data, args.buffer.size, -1,
                                static_cast<int>(args.buffer.index));
}
struct write_fixed_policy : basic_policy<write_fixed_args, prep_write_fixed> {
    template <class Receiver>
    static bool immediate(Receiver& receiver, args_t& args) noexcept {
        if (args.buffer.size == 0) {
            stdexec::set_value(std::move(receiver), std::size_t{0});
            return true;
        }
        return false;
    }
};

template <class Handle>
concept message_based = requires { std::remove_cvref_t<Handle>::message_based; } &&
                        std::remove_cvref_t<Handle>::message_based;

}

struct write_tag {};
using write_t = cpo<write_tag>;
inline constexpr write_t write{};

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd> || writable<std::remove_cvref_t<Handle>>
auto tag_invoke(write_t, io_context& context, Handle&& handle, rbytes source) noexcept {
    return detail::fd_sender<detail::write_policy>{
        &context, detail::writer_fd(handle), {source, detail::message_based<Handle>}};
}

template <class Handle>
requires std::same_as<std::remove_cvref_t<Handle>, iox::fd> || writable<std::remove_cvref_t<Handle>>
auto tag_invoke(write_t, io_context& context, Handle&& handle, registered_buffer& buffer) noexcept {
    return detail::fd_sender<detail::write_fixed_policy>{&context, detail::writer_fd(handle), {buffer}};
}

}
