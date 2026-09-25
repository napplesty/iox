// iox — xdp/read.h: xdp::read(ctx, xsk): one RX frame.
#pragma once

#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/cpo.h"
#include "iox/core/error.h"
#include "iox/runtime/io_context.h"
#include "iox/xdp/socket.h"

namespace iox::xdp {
namespace detail {

template <class R>
struct rx_op final : iox::op_base {
    socket* xsk;
    io_context* ctx;
    R r;

    rx_op(socket* s, io_context* c, R&& recv) noexcept
        : op_base(&rx_op::on_done), xsk(s), ctx(c), r(std::move(recv)) {}

    using operation_state_concept = stdexec::operation_state_tag;

    static void on_done(op_base* self, io_context&, std::int32_t res, std::uint32_t) noexcept {
        auto* o = static_cast<rx_op*>(self);
        if (res < 0) {
            stdexec::set_error(std::move(o->r), iox::error::from_negative(res));
            return;
        }
        const ::xdp_desc d = o->xsk->take_pending_frame();
        stdexec::set_value(std::move(o->r),
                           frame{o->xsk->frame_data(d.addr), d.len, d.addr});
    }

    void start() noexcept { xsk->submit_rx(*ctx, this); }
};

}

struct rx_sender {
    socket* xsk;
    io_context* ctx;
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = io::io_signatures<frame>;
    template <class R>
    auto connect(this const rx_sender& self, R&& r) {
        return detail::rx_op<std::remove_cvref_t<R>>{self.xsk, self.ctx, std::forward<R>(r)};
    }
};

inline constexpr struct read_t {
    template <class S>
    requires std::same_as<S, socket>
    auto operator()(io_context& ctx, S& s) const noexcept {
        return rx_sender{&s, &ctx};
    }
} read{};

}
