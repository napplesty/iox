// iox — unified async IO for Linux
// xdp/read.h — xdp::read(ctx, xsk): RX. A hand-rolled op_base sender — the
// completion path IS the source bridge, no per-op SQE (same shape as the M5
// test device / example).
#pragma once

#include <utility>

#include <stdexec/execution.hpp>

#include "iox/runtime/io_context.h"
#include "iox/xdp/socket.h"

namespace iox::xdp {
namespace detail {

template <class R>
struct rx_op final : iox::op_base {
    socket* xsk;
    R r;

    rx_op(socket* s, R&& recv) noexcept : op_base(&rx_op::on_done), xsk(s), r(std::move(recv)) {}

    using operation_state_concept = stdexec::operation_state_tag;

    static void on_done(op_base* self, io_context&, std::int32_t, std::uint32_t) noexcept {
        auto* o = static_cast<rx_op*>(self);
        const ::xdp_desc d = o->xsk->take_pending_frame();
        stdexec::set_value(std::move(o->r),
                           frame{o->xsk->frame_data(d.addr), d.len, d.addr});
    }

    void start() noexcept { xsk->submit_rx(this); }
};

} // namespace detail

struct rx_sender {
    socket* xsk;
    using sender_concept = stdexec::sender_tag;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(frame)>;
    template <class R>
    auto connect(this const rx_sender& self, R&& r) {
        return detail::rx_op<std::remove_cvref_t<R>>{self.xsk, std::forward<R>(r)};
    }
};

/// xdp::read(ctx, xsk) — RX; completes with the received frame.
inline constexpr struct read_t {
    template <class S>
    requires std::same_as<S, socket>
    auto operator()(io_context&, S& s) const noexcept {
        return rx_sender{&s};
    }
} read{};

} // namespace iox::xdp
