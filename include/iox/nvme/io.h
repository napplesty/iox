// iox — nvme/io.h: read_at / write_at / fsync on the NVMe device.
#pragma once

#include <linux/nvme_ioctl.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "iox/core/cpo.h"
#include "iox/nvme/device.h"
#include "iox/ops/fsync.h"
#include "iox/ops/read_at.h"
#include "iox/ops/write_at.h"

namespace iox::nvme {

inline constexpr std::uint8_t op_flush = 0x00;
inline constexpr std::uint8_t op_write = 0x01;
inline constexpr std::uint8_t op_read = 0x02;

namespace detail {

struct cmd_complete {
    template <class R, class A>
    static void complete(R& r, std::int32_t res, const A& a) noexcept {
        if (res == 0) {
            stdexec::set_value(std::move(r), a.len);
        } else if (res < 0) {
            stdexec::set_error(std::move(r), iox::error::from_negative(res));
        } else {
            stdexec::set_error(std::move(r), iox::error::from_errno(EIO));
        }
    }
};

struct io_policy {
    struct args_t {
        ::nvme_uring_cmd cmd{};
        std::size_t len = 0;
        unsigned lba_size = 0;
        std::uint64_t offset = 0;
        bool ring_ok = false;
    };
    using signatures = io::io_signatures<std::size_t>;
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (a.len == 0) {
            stdexec::set_value(std::move(r), std::size_t{0});
            return true;
        }
        if (!a.ring_ok) {
            stdexec::set_error(std::move(r),
                               iox::error::from_errno(EOPNOTSUPP)); // needs sqe128 ring
            return true;
        }
        if (a.lba_size == 0 || a.offset % a.lba_size != 0 || a.len % a.lba_size != 0) {
            stdexec::set_error(std::move(r), iox::error::from_errno(EINVAL));
            return true;
        }
        return false;
    }
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_uring_cmd(sqe, NVME_URING_CMD_IO, f.v);
        std::memcpy(reinterpret_cast<void*>(sqe->cmd), &a.cmd, sizeof(a.cmd));
    }
    using complete = cmd_complete;
};

struct flush_policy {
    struct args_t {
        ::nvme_uring_cmd cmd{};
        bool ring_ok = false;
    };
    template <class R>
    static bool immediate(R& r, args_t& a) noexcept {
        if (!a.ring_ok) {
            stdexec::set_error(std::move(r), iox::error::from_errno(EOPNOTSUPP));
            return true;
        }
        return false;
    }
    using signatures = io::io_signatures<>;
    static void prep(io_uring_sqe* sqe, iox::fd f, args_t& a) noexcept {
        ::io_uring_prep_uring_cmd(sqe, NVME_URING_CMD_IO, f.v);
        std::memcpy(reinterpret_cast<void*>(sqe->cmd), &a.cmd, sizeof(a.cmd));
    }
    struct flush_complete {
        template <class R, class A>
        static void complete(R& r, std::int32_t res, const A&) noexcept {
            if (res == 0) {
                stdexec::set_value(std::move(r));
            } else if (res < 0) {
                stdexec::set_error(std::move(r), iox::error::from_negative(res));
            } else {
                stdexec::set_error(std::move(r), iox::error::from_errno(EIO));
            }
        }
    };
    using complete = flush_complete;
};

inline ::nvme_uring_cmd make_io_cmd(const device& dev, std::uint8_t opcode, void* data,
                                    std::size_t len, std::uint64_t offset) noexcept {
    ::nvme_uring_cmd cmd{};
    cmd.opcode = opcode;
    cmd.nsid = dev.nsid();
    cmd.addr = reinterpret_cast<std::uint64_t>(data);
    cmd.data_len = static_cast<std::uint32_t>(len);
    const std::uint64_t lba = offset >> static_cast<unsigned>(__builtin_ctz(dev.lba_size()));
    const std::uint32_t nlb = static_cast<std::uint32_t>(len / dev.lba_size() - 1);
    cmd.cdw10 = static_cast<std::uint32_t>(lba);
    cmd.cdw11 = static_cast<std::uint32_t>(lba >> 32);
    cmd.cdw12 = nlb;
    cmd.timeout_ms = 30'000;
    return cmd;
}

}

inline auto tag_invoke(io::read_at_t, io_context& ctx, device& dev, wbytes dest,
                       uoffset_t at) noexcept {
    return io::detail::fd_sender<detail::io_policy>{
        &ctx, *dev.fd_slot(),
        {detail::make_io_cmd(dev, op_read, dest.data(), dest.size(), at.v),
         dest.size(), dev.lba_size(), at.v, ctx.ring().sqe128()}};
}

inline auto tag_invoke(io::write_at_t, io_context& ctx, device& dev, rbytes source,
                       uoffset_t at) noexcept {
    return io::detail::fd_sender<detail::io_policy>{
        &ctx, *dev.fd_slot(),
        {detail::make_io_cmd(dev, op_write, const_cast<void*>(static_cast<const void*>(source.data())),
                             source.size(), at.v),
         source.size(), dev.lba_size(), at.v, ctx.ring().sqe128()}};
}

inline auto tag_invoke(io::fsync_t, io_context& ctx, device& dev) noexcept {
    ::nvme_uring_cmd cmd{};
    cmd.opcode = op_flush;
    cmd.nsid = dev.nsid();
    cmd.timeout_ms = 30'000;
    return io::detail::fd_sender<detail::flush_policy>{&ctx, *dev.fd_slot(), {cmd, ctx.ring().sqe128()}};
}

}
