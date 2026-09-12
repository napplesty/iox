// iox — unified async IO for Linux
// include/iox/core/mr.h — buffer_pool: registered memory (design §四.②, §三.⑤⑥).
#pragma once

#include <liburing.h>

#include <cstddef>
#include <expected>
#include <new>
#include <vector>

#include "iox/core/buffer.h"
#include "iox/core/error.h"
#include "iox/runtime/io_context.h"

namespace iox {

class buffer_pool {
public:
    buffer_pool() noexcept = default;

    static std::expected<buffer_pool, error> create(io_context& ctx,
                                                    std::size_t slot_size,
                                                    std::size_t slots,
                                                    std::size_t alignment = 4096) noexcept {
        if (slot_size == 0 || slots == 0) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        if (!ctx.ok()) {
            return std::unexpected(error::from_errno(EBADF));
        }
        if (ctx.buffers_registered()) {
            return std::unexpected(error::from_errno(EBUSY));
        }

        buffer_pool p;
        p.ctx_ = &ctx;
        p.slot_size_ = slot_size;
        p.slots_ = slots;
        p.align_ = alignment;

        const std::size_t total = slot_size * slots;
        p.storage_ = static_cast<std::byte*>(::operator new(total, std::align_val_t{alignment},
                                                            std::nothrow));
        if (p.storage_ == nullptr) {
            return std::unexpected(error::from_errno(ENOMEM));
        }

        p.iovs_.resize(slots);
        for (std::size_t i = 0; i < slots; ++i) {
            p.iovs_[i] = iovec{p.storage_ + i * slot_size, slot_size};
        }

        const int result = ::io_uring_register_buffers(ctx.ring().native(), p.iovs_.data(),
                                                   static_cast<unsigned>(slots));
        if (result != 0) {
            ::operator delete(p.storage_, std::align_val_t{alignment}, std::nothrow);
            p.storage_ = nullptr;
            return std::unexpected(error::from_negative(result));
        }
        ctx.set_buffers_registered();

        for (std::size_t i = 0; i < slots; ++i) {
            p.free_.push_back(static_cast<std::uint32_t>(i));
        }
        return p;
    }

    ~buffer_pool() { reset(); }

    buffer_pool(buffer_pool&& other) noexcept
        : ctx_(other.ctx_), storage_(std::exchange(other.storage_, nullptr)),
          slot_size_(other.slot_size_), slots_(other.slots_),
          iovs_(std::move(other.iovs_)), free_(std::move(other.free_)) {
        other.ctx_ = nullptr;
        other.slots_ = 0;
    }

    buffer_pool& operator=(buffer_pool&& other) noexcept {
        if (this != &other) {
            reset();
            ctx_ = std::exchange(other.ctx_, nullptr);
            storage_ = std::exchange(other.storage_, nullptr);
            slot_size_ = other.slot_size_;
            slots_ = other.slots_;
            iovs_ = std::move(other.iovs_);
            free_ = std::move(other.free_);
        }
        return *this;
    }

    buffer_pool(const buffer_pool&) = delete;
    buffer_pool& operator=(const buffer_pool&) = delete;

    std::expected<registered_buffer, error> take() noexcept {
        if (free_.empty()) {
            return std::unexpected(error::from_errno(EBUSY));
        }
        const auto index = free_.back();
        free_.pop_back();
        return registered_buffer{
            index, this, storage_ + static_cast<std::size_t>(index) * slot_size_,
            slot_size_};
    }

    void give_back(const registered_buffer& b) noexcept {
        if (b.owner != this || b.index >= slots_) {
            return;
        }
        for (const auto f : free_) {
            if (f == b.index) {
                return;
            }
        }
        free_.push_back(b.index);
    }

    std::size_t slot_size() const noexcept { return slot_size_; }
    std::size_t capacity() const noexcept { return slots_; }
    std::size_t available() const noexcept { return free_.size(); }

    void reset() noexcept {
        if (ctx_ != nullptr && storage_ != nullptr) {
            ::io_uring_unregister_buffers(ctx_->ring().native());
            ctx_->set_buffers_registered(false);
        }
        if (storage_ != nullptr) {
            ::operator delete(storage_, std::align_val_t{align_}, std::nothrow);
            storage_ = nullptr;
        }
        free_.clear();
        iovs_.clear();
        slots_ = 0;
        ctx_ = nullptr;
    }

private:
    io_context* ctx_ = nullptr;
    std::byte* storage_ = nullptr;
    std::size_t slot_size_ = 0;
    std::size_t slots_ = 0;
    std::size_t align_ = 4096;
    std::vector<iovec> iovs_;
    std::vector<std::uint32_t> free_;
};

}
