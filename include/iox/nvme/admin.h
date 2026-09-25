// iox — nvme/admin.h: raw ADMIN passthru (IDENTIFY, GET_LOG_PAGE, firmware …).
#pragma once

#include <linux/nvme_ioctl.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "iox/core/cpo.h"
#include "iox/nvme/device.h"
#include "iox/ops/fd_sender.h"

namespace iox::io::detail {
struct admin_passthru_t final {};
}

namespace iox::nvme {

struct admin_policy {
    struct args_t {
        ::nvme_uring_cmd cmd{};
        bool ring_ok = false;
    };
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (!a.ring_ok) { // 72-byte cmd must NOT memcpy into a 64-byte SQE
            stdexec::set_error(std::move(r), iox::error::from_errno(EOPNOTSUPP));
            return true;
        }
        return false;
    }
    using signatures = io::io_signatures<std::int32_t>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_uring_cmd(sqe, NVME_URING_CMD_ADMIN, f.v);
        std::memcpy(reinterpret_cast<void*>(sqe->cmd), &a.cmd, sizeof(a.cmd));
    }
    struct admin_complete {
        template <class R, class A>
        static void complete(R& r, std::int32_t res, const A&) noexcept {
            if (res < 0) {
                stdexec::set_error(std::move(r), iox::error::from_negative(res));
            } else {
                stdexec::set_value(std::move(r), res);
            }
        }
    };
    using complete = admin_complete;
};

inline auto tag_invoke(io::detail::admin_passthru_t, io_context& ctx, device& d,
                       const ::nvme_uring_cmd& cmd) noexcept {
    return io::detail::fd_sender<admin_policy>{&ctx, *d.fd_slot(),
        {cmd, ctx.ring().sqe128()}};
}

inline constexpr struct admin_t {
    template <class D>
    requires std::same_as<D, device>
    auto operator()(io_context& ctx, D& d, const ::nvme_uring_cmd& cmd) const noexcept {
        return tag_invoke(io::detail::admin_passthru_t{}, ctx, d, cmd);
    }
} admin{};

}
