// iox — unified async IO for Linux
// nvme/device.h — the NVMe driver HANDLE: open the namespace's passthru
// character device, load geometry, declare capabilities (M6, design §六).
// Vocabulary customizations live in nvme/io.h (read_at/write_at/fsync) and
// nvme/admin.h (admin passthru) — include those to use the device.
//
//     nvme::device dev = *nvme::device::open("/dev/ng0n1");
//     io::read_at(ctx, dev, buf, uoffset_t{1 << 20});   // byte offsets, LBA-mapped
//     io::write_at(ctx, dev, src, uoffset_t{...});
//     io::fsync(ctx, dev);                              // NVMe FLUSH
//
// Requirements & permissions (honest matrix):
//   * the ring must be created with uring::ring_params::sqe128 = true —
//     nvme_uring_cmd (72 B) does not fit a 64-byte SQE; ops on a plain ring
//     complete with EOPNOTSUPP up front.
//   * opening /dev/ngXnY needs read permission on the node (it is 0600 root
//     by default; an ACL or a dedicated user grants it).
//   * IO opcodes (read/write/flush) are allowed with the open alone; admin
//     passthru additionally requires CAP_SYS_ADMIN — geometry therefore
//     comes from sysfs, not from an IDENTIFY passthru.
//
// Geometry (lba size/count, nsid) is read once at open — control path, like
// fs::file::open. WRITE passthru targets whatever LBA you compute: writing
// a live disk's blocks WILL destroy data. Tests and benches default to
// read-only; write coverage requires IOX_NVME_TEST_WRITE=1 and lands on the
// LAST LBA of the namespace (still: do not point this at your system disk).
#pragma once

#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <utility>

#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/driver/capabilities.h"
#include "iox/driver/registry.h"

namespace iox::nvme {

class device {
public:
    device() noexcept = default;
    ~device() { reset(); }
    device(device&& other) noexcept
        : fd_(std::exchange(other.fd_, iox::fd{})), nsid_(other.nsid_),
          lba_shift_(other.lba_shift_), lba_count_(other.lba_count_) {}
    device& operator=(device&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, iox::fd{});
            nsid_ = other.nsid_;
            lba_shift_ = other.lba_shift_;
            lba_count_ = other.lba_count_;
        }
        return *this;
    }
    device(const device&) = delete;
    device& operator=(const device&) = delete;

    /// Open the namespace's passthru character device and load geometry.
    /// O_RDWR so write_at is usable when the node grants it; no IO happens
    /// at open. `path` is typically "/dev/ng0n1" (see /sys/class/nvme/…).
    static std::expected<device, error> open(const char* path) noexcept {
        const int raw = ::open(path, O_RDWR | O_CLOEXEC);
        if (raw < 0) {
            // Read-only nodes still support READ/FLUSH passthu; try again.
            const int ro = ::open(path, O_RDONLY | O_CLOEXEC);
            if (ro < 0) {
                return std::unexpected(error::from_errno(errno));
            }
            return from_raw(ro, path);
        }
        return from_raw(raw, path);
    }

    bool valid() const noexcept { return fd_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    // geometry — immutable after open
    std::uint32_t nsid() const noexcept { return nsid_; }
    unsigned lba_size() const noexcept { return 1u << lba_shift_; }
    std::uint64_t lba_count() const noexcept { return lba_count_; }
    std::uint64_t size_bytes() const noexcept { return lba_count_ << lba_shift_; }

    /// io::close (borrowing form) clears this slot on completion.
    iox::fd* fd_slot() noexcept { return &fd_; }

    void reset() noexcept {
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    static std::expected<device, error> from_raw(int raw, const char* path) noexcept {
        device dev;
        dev.fd_ = iox::fd{raw};

        // nsid: the char device knows its namespace (plain ioctl, no caps).
        const int nsid = static_cast<std::uint32_t>(::ioctl(raw, NVME_IOCTL_ID));
        if (nsid <= 0) {
            dev.reset();
            return std::unexpected(error::from_errno(nsid == 0 ? ENODEV : errno));
        }
        dev.nsid_ = static_cast<std::uint32_t>(nsid);

        // geometry from the sysfs twin of the node ("ng0n1" -> "nvme0n1"):
        // admin IDENTIFY passthru is CAP_SYS_ADMIN-gated, sysfs is not.
        const char* base = std::strrchr(path, '/');
        base = base != nullptr ? base + 1 : path;
        char sysfs[128];
        std::snprintf(sysfs, sizeof(sysfs), "/sys/block/nvme%s/queue/logical_block_size",
                      std::strncmp(base, "ng", 2) == 0 ? base + 1 : base);
        unsigned block_size = 0;
        if (std::FILE* f = std::fopen(sysfs, "r")) {
            (void)std::fscanf(f, "%u", &block_size);
            std::fclose(f);
        }
        if (block_size < 512 || block_size > 16384 || (block_size & (block_size - 1)) != 0) {
            dev.reset();
            return std::unexpected(error::from_errno(ENODEV)); // no sysfs twin
        }
        dev.lba_shift_ = static_cast<unsigned>(__builtin_ctz(block_size));

        std::snprintf(sysfs, sizeof(sysfs), "/sys/block/nvme%s/size",
                      std::strncmp(base, "ng", 2) == 0 ? base + 1 : base);
        unsigned long long sectors = 0;
        if (std::FILE* f = std::fopen(sysfs, "r")) {
            (void)std::fscanf(f, "%llu", &sectors);
            std::fclose(f);
        }
        if (sectors == 0) {
            dev.reset();
            return std::unexpected(error::from_errno(ENODEV));
        }
        dev.lba_count_ = sectors * 512 >> dev.lba_shift_;
        return dev;
    }

    iox::fd fd_{};
    std::uint32_t nsid_ = 0;
    unsigned lba_shift_ = 9;
    std::uint64_t lba_count_ = 0;
};

// ---- capabilities + registration -------------------------------------------

inline bool tag_invoke(io::detail::supports_t, io::zero_copy_t, const device&) noexcept {
    return true; // kernel DMAs straight into the caller's buffer
}
inline bool tag_invoke(io::detail::supports_t, io::dma_t, const device&) noexcept {
    return true;
}

} // namespace iox::nvme

namespace iox::driver {
template <>
inline constexpr bool registered_driver<nvme::device> = true;
} // namespace iox::driver
