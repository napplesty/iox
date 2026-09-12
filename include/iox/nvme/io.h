// iox — unified async IO for Linux
// nvme/io.h — the unified vocabulary on the NVMe device: read_at / write_at
// / fsync as IORING_OP_URING_CMD passthru. The nvme_uring_cmd rides in the
// op state; the data buffer is caller-owned (zero-copy contract — kernel
// DMAs into it direct).
#pragma once

#include <linux/nvme_ioctl.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "iox/nvme/device.h"
#include "iox/ops/fsync.h"
#include "iox/ops/read_at.h"
#include "iox/ops/write_at.h"

namespace iox::nvme {

// NVMe IO opcodes (spec §6); spelled locally to keep the header independent
// of linux/nvme.h (not shipped in linux-libc-dev).
inline constexpr std::uint8_t op_flush = 0x00;
inline constexpr std::uint8_t op_write = 0x01;
inline constexpr std::uint8_t op_read = 0x02;

namespace detail {

struct cmd_complete {
    // CQE res for uring_cmd: 0 = success (byte count is what we requested);
    // negative = -errno; positive = an NVMe status word (admin path).
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
        std::size_t len = 0; // bytes transferred (for set_value)
        unsigned lba_size = 0;
        std::uint64_t offset = 0;
        bool ring_ok = false; // ctx has 128-byte SQEs (captured at tag_invoke)
    };
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
    // The unified vocabulary speaks BYTE offsets; the device speaks LBAs.
    // Misaligned requests are a typed error up front — a positional op must
    // never silently round (same stance as write_at's sentinel guard).
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
    using signatures = stdexec::completion_signatures<stdexec::set_value_t(),
                                                stdexec::set_error_t(iox::error), stdexec::set_stopped_t()>;
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

} // namespace detail

// Concrete overloads next to the handle (ADL) — they beat the fd defaults,
// which are not even viable: the device exposes no read_handle()/
// write_handle() (it is NOT an fd handle; no stream position, no mmap path,
// and no other vocabulary entry has any business compiling against it).

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
    cmd.opcode = op_flush; // NVMe FLUSH — force volatile cache to media
    cmd.nsid = dev.nsid();
    cmd.timeout_ms = 30'000;
    return io::detail::fd_sender<detail::flush_policy>{&ctx, *dev.fd_slot(), {cmd, ctx.ring().sqe128()}};
}

} // namespace iox::nvme
