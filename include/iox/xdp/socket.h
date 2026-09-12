// iox — unified async IO for Linux
// include/iox/xdp/socket.h — the AF_XDP (XSK) driver handle (M6, design §六).
#pragma once

#include <linux/if_xdp.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <vector>

#include "iox/core/fd.h"
#include "iox/driver/capabilities.h"
#include "iox/driver/completion_source.h"
#include "iox/driver/registry.h"
#include "iox/runtime/io_context.h"
#include "iox/xdp/umem.h"

namespace iox::xdp {

namespace detail {

template <class D>
struct ring_view {
    std::uint32_t* producer = nullptr;
    std::uint32_t* consumer = nullptr;
    const std::uint32_t* flags = nullptr;
    D* desc = nullptr;
    std::uint32_t mask = 0;

    std::uint32_t cached_prod = 0;

    std::uint32_t prod() const noexcept { return __atomic_load_n(producer, __ATOMIC_ACQUIRE); }
    std::uint32_t cons() const noexcept { return __atomic_load_n(consumer, __ATOMIC_ACQUIRE); }
    void publish(std::uint32_t p) noexcept { __atomic_store_n(producer, p, __ATOMIC_RELEASE); }
    std::uint32_t free() const noexcept { return mask + 1 - (cached_prod - cons()); }
};

}

class socket final : public iox::completion_source {
public:
    socket() noexcept = default;
    ~socket() override { reset(); }
    socket(socket&& offsets) noexcept { move_from(offsets); }
    socket& operator=(socket&& offsets) noexcept {
        if (this != &offsets) {
            reset();
            move_from(offsets);
        }
        return *this;
    }
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    static std::expected<socket, error> create(const umem& mem, const char* ifname,
                                               std::uint32_t queue_id,
                                               std::size_t ring_size) noexcept {
        int fd = ::socket(AF_XDP, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::unexpected(error::from_errno(errno)); // often EPERM: no CAP_NET_RAW
        }
        socket s;
        s.fd_ = iox::fd{fd};
        s.mem_ = &mem;
        s.chunk_size_ = mem.chunk_size();

        const int ring_size_int = static_cast<int>(ring_size);
        if (::setsockopt(fd, SOL_XDP, XDP_UMEM_FILL_RING, &ring_size_int, sizeof ring_size_int) < 0 ||
            ::setsockopt(fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring_size_int, sizeof ring_size_int) < 0 ||
            ::setsockopt(fd, SOL_XDP, XDP_RX_RING, &ring_size_int, sizeof ring_size_int) < 0 ||
            ::setsockopt(fd, SOL_XDP, XDP_TX_RING, &ring_size_int, sizeof ring_size_int) < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        xdp_mmap_offsets off{};
        socklen_t optlen = sizeof off;
        if (::getsockopt(fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen) < 0) {
            return std::unexpected(error::from_errno(errno));
        }
        s.ring_maps_[0] = map_ring(fd, XDP_UMEM_PGOFF_FILL_RING, off.fr, ring_size, s.fill_);
        s.ring_maps_[1] = map_ring(fd, XDP_UMEM_PGOFF_COMPLETION_RING, off.cr, ring_size, s.comp_);
        s.ring_maps_[2] = map_ring(fd, XDP_PGOFF_RX_RING, off.rx, ring_size, s.rx_);
        s.ring_maps_[3] = map_ring(fd, XDP_PGOFF_TX_RING, off.tx, ring_size, s.tx_);
        if (s.ring_maps_[0] == nullptr || s.ring_maps_[1] == nullptr ||
            s.ring_maps_[2] == nullptr || s.ring_maps_[3] == nullptr) {
            return std::unexpected(error::from_errno(errno));
        }
        ::xdp_umem_reg umem_reg{};
        umem_reg.addr = reinterpret_cast<std::uintptr_t>(mem.data());
        umem_reg.len = mem.chunk_count() * mem.chunk_size();
        umem_reg.chunk_size = static_cast<std::uint32_t>(mem.chunk_size());
        if (::setsockopt(fd, SOL_XDP, XDP_UMEM_REG, &umem_reg, sizeof umem_reg) < 0) {
            return std::unexpected(error::from_errno(errno));
        }

        struct sockaddr_xdp address{};
        address.sxdp_family = AF_XDP;
        address.sxdp_ifindex = ::if_nametoindex(ifname);
        address.sxdp_queue_id = queue_id;
        address.sxdp_flags = XDP_COPY;
        if (address.sxdp_ifindex == 0 ||
            ::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof address) < 0) {
            return std::unexpected(error::from_errno(errno));
        }

        for (std::size_t i = 0; i < mem.chunk_count(); ++i) {
            s.free_chunks_.push_back(i * mem.chunk_size());
        }
        s.push_fill_all();
        return std::expected<socket, error>{std::move(s)};
    }

    bool valid() const noexcept { return fd_.valid(); }

    iox::fd completion_fd() const noexcept override { return fd_; }

    void on_ready(io_context& ctx) noexcept override {
        drain_tx_completions(ctx);
        drain_rx(ctx);
    }

    frame tx_frame() noexcept {
        const std::uint64_t address = free_chunks_.back();
        free_chunks_.pop_back();
        return frame{mem_->chunk(address / chunk_size_), 0, address};
    }

    std::byte* frame_data(std::uint64_t address) const noexcept {
        return mem_->chunk(address / chunk_size_);
    }

    void submit_tx(iox::op_base* op, std::uint64_t address, std::uint32_t len) noexcept {
        tx_waiters_.push_back(op);
        if (tx_.free() != 0) {
            const std::uint32_t p = tx_.cached_prod;
            tx_.desc[p & tx_.mask] = ::xdp_desc{address, len};
            tx_.cached_prod = p + 1;
            tx_.publish(p + 1);
            (void)::sendto(fd_.v, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
        } else {
            tx_backlog_.push_back(::xdp_desc{address, len});
        }
    }

    void submit_rx(iox::op_base* op) noexcept { rx_waiters_.push_back(op); }

    ::xdp_desc take_pending_frame() noexcept {
        const ::xdp_desc d = pending_frames_.front();
        pending_frames_.erase(pending_frames_.begin());
        return d;
    }

    void recycle(const frame& f) noexcept { free_chunks_.push_back(f.umem_addr); }

    void reset() noexcept {
        for (void* m : ring_maps_) {
            if (m != nullptr) {
                ::munmap(m, 0);
            }
        }
        for (void*& m : ring_maps_) {
            m = nullptr;
        }
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    template <class D>
    static void* map_ring(int fd, std::uint64_t pgoff, const xdp_ring_offset& offsets,
                          std::size_t count, detail::ring_view<D>& out) noexcept {
        const std::size_t bytes = offsets.desc + count * sizeof(D);
        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         fd, pgoff);
        if (p == MAP_FAILED) {
            return nullptr;
        }
        out.producer = reinterpret_cast<std::uint32_t*>(static_cast<char*>(p) + offsets.producer);
        out.consumer = reinterpret_cast<std::uint32_t*>(static_cast<char*>(p) + offsets.consumer);
        out.flags = reinterpret_cast<const std::uint32_t*>(static_cast<char*>(p) + offsets.flags);
        out.desc = reinterpret_cast<D*>(static_cast<char*>(p) + offsets.desc);
        out.mask = static_cast<std::uint32_t>(count - 1);
        out.cached_prod = 0;
        return p;
    }

    void move_from(socket& offsets) noexcept {
        fd_ = offsets.fd_;
        mem_ = offsets.mem_;
        chunk_size_ = offsets.chunk_size_;
        fill_ = offsets.fill_;
        comp_ = offsets.comp_;
        rx_ = offsets.rx_;
        tx_ = offsets.tx_;
        free_chunks_ = std::move(offsets.free_chunks_);
        tx_waiters_ = std::move(offsets.tx_waiters_);
        rx_waiters_ = std::move(offsets.rx_waiters_);
        tx_backlog_ = std::move(offsets.tx_backlog_);
        pending_frames_ = std::move(offsets.pending_frames_);
        for (unsigned i = 0; i < 4; ++i) {
            ring_maps_[i] = offsets.ring_maps_[i];
            offsets.ring_maps_[i] = nullptr;
        }
        offsets.fd_ = iox::fd{};
    }

    void push_fill_all() noexcept {
        std::uint32_t p = fill_.cached_prod;
        for (const std::uint64_t address : free_chunks_) {
            if (fill_.free() == 0) {
                break;
            }
            *reinterpret_cast<std::uint64_t*>(&fill_.desc[p & fill_.mask]) = address;
            ++p;
        }
        fill_.cached_prod = p;
        fill_.publish(p);
        free_chunks_.clear();
    }

    void drain_tx_completions(io_context& ctx) noexcept {
        const std::uint32_t prod = comp_.prod();
        std::uint32_t cons = comp_.cons();
        while (cons != prod) {
            const std::uint64_t address =
                *reinterpret_cast<std::uint64_t*>(&comp_.desc[cons & comp_.mask]);
            ++cons;
            free_chunks_.push_back(address);
            if (!tx_waiters_.empty()) { // FIFO: copy mode completes in order
                iox::op_base* op = tx_waiters_.front();
                tx_waiters_.erase(tx_waiters_.begin());
                ctx.dispatch(reinterpret_cast<std::uint64_t>(op), 0, 0);
            }
            if (!tx_backlog_.empty()) {
                const ::xdp_desc d = tx_backlog_.front();
                tx_backlog_.erase(tx_backlog_.begin());
                const std::uint32_t p = tx_.cached_prod;
                tx_.desc[p & tx_.mask] = d;
                tx_.cached_prod = p + 1;
                tx_.publish(p + 1);
                (void)::sendto(fd_.v, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
            }
        }
        __atomic_store_n(comp_.consumer, cons, __ATOMIC_RELEASE);
    }

    void drain_rx(io_context& ctx) noexcept {
        const std::uint32_t prod = rx_.prod();
        std::uint32_t cons = rx_.cons();
        while (cons != prod && !rx_waiters_.empty()) {
            const ::xdp_desc d = rx_.desc[cons & rx_.mask];
            ++cons;
            iox::op_base* op = rx_waiters_.front();
            rx_waiters_.erase(rx_waiters_.begin());
            pending_frames_.push_back(d);
            ctx.dispatch(reinterpret_cast<std::uint64_t>(op), 0, 0);
        }
        __atomic_store_n(rx_.consumer, cons, __ATOMIC_RELEASE);
    }

    iox::fd fd_{};
    const umem* mem_ = nullptr;
    std::size_t chunk_size_ = 0;
    detail::ring_view<std::uint64_t> fill_{};
    detail::ring_view<std::uint64_t> comp_{};
    detail::ring_view<::xdp_desc> rx_{};
    detail::ring_view<::xdp_desc> tx_{};
    std::vector<std::uint64_t> free_chunks_;
    std::vector<iox::op_base*> tx_waiters_;
    std::vector<iox::op_base*> rx_waiters_;
    std::vector<::xdp_desc> tx_backlog_;
    std::vector<::xdp_desc> pending_frames_;
    void* ring_maps_[4]{};
};

inline bool tag_invoke(io::detail::supports_t, io::zero_copy_t, const socket&) noexcept {
    return false;
}
inline bool tag_invoke(io::detail::supports_t, io::dma_t, const socket&) noexcept {
    return true;
}

}

namespace iox::driver {
template <>
inline constexpr bool registered_driver<xdp::socket> = true;
}
