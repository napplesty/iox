// iox — nvme/device.h: NVMe namespace passthru device handle.
#pragma once

#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <type_traits>
#include <utility>

#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/driver/capabilities.h"
#include "iox/driver/registry.h"

namespace iox::nvme {

// /dev/ng0n1 and /dev/nvme0n1 both map to the sysfs block entry nvme0n1.
inline const char* block_suffix(const char* base) noexcept {
    if (std::strncmp(base, "ng", 2) == 0) {
        return base + 2;
    }
    if (std::strncmp(base, "nvme", 4) == 0) {
        return base + 4;
    }
    return base;
}

class device {
public:
    device() noexcept = default;

    static std::expected<device, error> open(const char* path) noexcept {
        const int raw = ::open(path, O_RDWR | O_CLOEXEC);
        if (raw < 0) {
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

    std::uint32_t nsid() const noexcept { return nsid_; }
    unsigned lba_size() const noexcept { return 1u << lba_shift_; }
    std::uint64_t lba_count() const noexcept { return lba_count_; }
    std::uint64_t size_bytes() const noexcept { return lba_count_ << lba_shift_; }

    iox::fd* fd_slot() noexcept { return fd_.slot(); }

    void reset() noexcept { fd_.reset(); }

private:
    static std::expected<device, error> from_raw(int raw, const char* path) noexcept {
        device dev;
        dev.fd_ = iox::unique_fd{raw};

        const int nsid = static_cast<std::uint32_t>(::ioctl(raw, NVME_IOCTL_ID));
        if (nsid <= 0) {
            dev.reset();
            return std::unexpected(error::from_errno(nsid == 0 ? ENODEV : errno));
        }
        dev.nsid_ = static_cast<std::uint32_t>(nsid);

        const char* base = std::strrchr(path, '/');
        base = base != nullptr ? base + 1 : path;
        char sysfs[128];
        std::snprintf(sysfs, sizeof(sysfs), "/sys/block/nvme%s/queue/logical_block_size",
                      block_suffix(base));
        unsigned block_size = 0;
        if (std::FILE* f = std::fopen(sysfs, "r")) {
            (void)std::fscanf(f, "%u", &block_size);
            std::fclose(f);
        }
        if (block_size < 512 || block_size > 16384 || (block_size & (block_size - 1)) != 0) {
            dev.reset();
            return std::unexpected(error::from_errno(ENODEV));
        }
        dev.lba_shift_ = static_cast<unsigned>(__builtin_ctz(block_size));

        std::snprintf(sysfs, sizeof(sysfs), "/sys/block/nvme%s/size", block_suffix(base));
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

    iox::unique_fd fd_{};
    std::uint32_t nsid_ = 0;
    unsigned lba_shift_ = 9;
    std::uint64_t lba_count_ = 0;
};

static_assert(std::is_nothrow_move_constructible_v<device> &&
              !std::is_copy_constructible_v<device>);

inline bool tag_invoke(io::detail::supports_t, io::zero_copy_t, const device&) noexcept {
    return true;
}
inline bool tag_invoke(io::detail::supports_t, io::dma_t, const device&) noexcept {
    return true;
}

}

namespace iox::driver {
template <>
inline constexpr bool registered_driver<nvme::device> = true;
}
