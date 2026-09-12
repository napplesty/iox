// iox — unified async IO for Linux
// xdp/umem.h — the shared packet memory an AF_XDP socket moves chunks
// through: `frame` (a chunk reference) + `umem` (the mmap'd region the
// kernel DMAs into). Driver handle lives in xdp/socket.h, vocabulary in
// xdp/read.h and xdp/write_frame.h.
#pragma once

#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "iox/core/error.h"

namespace iox::xdp {

/// One umem chunk reference: the packet's bytes at `data`, its umem-relative
/// address for the rings. Returned by xdp::read, consumed by
/// xdp::write_frame; the socket recycles the chunk when the op completes.
struct frame {
    std::byte* data = nullptr;
    std::uint32_t len = 0;
    std::uint64_t umem_addr = 0;
};

/// The shared packet memory + the kernel-side fill/completion rings.
class umem {
public:
    umem() noexcept = default;
    ~umem() { reset(); }
    umem(umem&& o) noexcept { move_from(o); }
    umem& operator=(umem&& o) noexcept {
        if (this != &o) {
            reset();
            move_from(o);
        }
        return *this;
    }
    umem(const umem&) = delete;
    umem& operator=(const umem&) = delete;

    /// `chunks` × `chunk_size` bytes (chunk_size ≥ 2048; 4096 is the sane
    /// default).
    static std::expected<umem, error> create(std::size_t chunks, std::size_t chunk_size) noexcept {
        umem m;
        m.chunk_size_ = chunk_size;
        m.chunk_count_ = chunks;
        m.len_ = chunks * chunk_size;
        m.mem_ = ::mmap(nullptr, m.len_, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (m.mem_ == MAP_FAILED) {
            return std::unexpected(error::from_errno(errno));
        }
        return m;
    }

    std::byte* data() const noexcept { return static_cast<std::byte*>(mem_); }
    std::size_t chunk_size() const noexcept { return chunk_size_; }
    std::size_t chunk_count() const noexcept { return chunk_count_; }
    std::byte* chunk(std::size_t i) const noexcept { return data() + i * chunk_size_; }

    void reset() noexcept {
        if (mem_ != nullptr) {
            ::munmap(mem_, len_);
            mem_ = nullptr;
        }
    }

private:
    void move_from(umem& o) noexcept {
        mem_ = o.mem_;
        len_ = o.len_;
        chunk_size_ = o.chunk_size_;
        chunk_count_ = o.chunk_count_;
        o.mem_ = nullptr;
    }
    void* mem_ = nullptr;
    std::size_t len_ = 0;
    std::size_t chunk_size_ = 0;
    std::size_t chunk_count_ = 0;
};

} // namespace iox::xdp
