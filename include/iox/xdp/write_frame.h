// iox — xdp/write_frame.h: xdp::write_frame(ctx, xsk, f): TX; the owning op recycles its chunk exactly once.
#pragma once

#include <cstdint>
#include <utility>

#include <stdexec/execution.hpp>

#include "iox/core/cpo.h"
#include "iox/core/error.h"
#include "iox/runtime/io_context.h"
#include "iox/xdp/socket.h"

namespace iox::xdp {
namespace detail {

template <class R>
struct tx_op final : iox::op_base {
    socket* xsk;
    io_context* ctx;
    frame f;
    R r;

    tx_op(socket* s, io_context* c, frame frame, R&& recv) noexcept
        : op_base(&tx_op::on_done), xsk(s), ctx(c), f(frame), r(std::move(recv)) {}

    using operation_state_concept = stdexec::operation_state_tag;

    static void on_done(op_base* self, io_context&, std::int32_t res, std::uint32_t) noexcept {
        auto* o = static_cast<tx_op*>(self);
        if (res < 0) {
            stdexec::set_error(std::move(o->r), iox::error::from_negative(res));
            return;
        }
        o->xsk->recycle(o->f);
        stdexec::set_value(std::move(o->r), o->f.len);
    }

    void start() noexcept { xsk->submit_tx(*ctx, this, f.umem_addr, f.len); }
};

}

struct tx_sender {
    socket* xsk;
    io_context* ctx;
    frame f;
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = io::io_signatures<std::uint32_t>;
    template <class Self, class R>
    auto connect(this Self&& self, R&& r) {
        return detail::tx_op<std::remove_cvref_t<R>>{self.xsk, self.ctx, self.f,
                                                     std::forward<R>(r)};
    }
};

struct write_frame_t {
    template <class S>
    requires std::same_as<S, socket>
    auto operator()(io_context& ctx, S& s, frame f) const noexcept {
        return tx_sender{&s, &ctx, f};
    }
};
inline constexpr write_frame_t write_frame{};

}
