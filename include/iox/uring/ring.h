// iox — unified async IO for Linux
// include/iox/uring/ring.h — thin RAII wrapper around liburing's io_uring.
#pragma once

#include <liburing.h>

#include <cstdint>
#include <utility>

namespace iox::uring {

struct ring_params {
    unsigned entries = 256;
    unsigned cq_entries = 0;
    bool sq_poll = false;
    bool sqe128 = false;
};

class ring {
public:
    ring() noexcept = default;

    explicit ring(const ring_params& p) : sqe128_(p.sqe128) {
        io_uring_params params{};
        if (p.sq_poll) {
            params.flags |= IORING_SETUP_SQPOLL;
        }
        if (p.cq_entries != 0) {
            params.cq_entries = p.cq_entries;
            params.flags |= IORING_SETUP_CQSIZE;
        }
        if (p.sqe128) {
            params.flags |= IORING_SETUP_SQE128;
        }
        ok_ = ::io_uring_queue_init_params(p.entries, &ring_, &params) == 0;
    }

    ~ring() {
        if (ok_) {
            ::io_uring_queue_exit(&ring_);
        }
    }

    ring(ring&& other) noexcept : ring_(other.ring_), ok_(other.ok_) {
        other.ok_ = false;
    }

    ring& operator=(ring&& other) noexcept {
        if (this != &other) {
            if (ok_) {
                ::io_uring_queue_exit(&ring_);
            }
            ring_ = other.ring_;
            ok_ = other.ok_;
            other.ok_ = false;
        }
        return *this;
    }

    ring(const ring&) = delete;
    ring& operator=(const ring&) = delete;

    bool ok() const noexcept { return ok_; }

    bool sqe128() const noexcept { return sqe128_; }

    io_uring* native() noexcept { return &ring_; }
    const io_uring* native() const noexcept { return &ring_; }

    io_uring_sqe* next_sqe() noexcept { return ::io_uring_get_sqe(&ring_); }

    int flush() noexcept {
        if (!ok_) {
            return -EINVAL;
        }
        if (::io_uring_sq_ready(&ring_) == 0) {
            return 0;
        }
        ++enters_;
        return ::io_uring_submit(&ring_);
    }

    int flush_and_wait(unsigned wait_nr) noexcept {
        if (!ok_) {
            return -EINVAL;
        }
        ++enters_;
        return ::io_uring_submit_and_wait(&ring_, static_cast<int>(wait_nr));
    }

    unsigned cq_ready() const noexcept { return ::io_uring_cq_ready(&ring_); }

    template <class F>
    unsigned for_each_cqe(F&& f) noexcept(noexcept(f(static_cast<io_uring_cqe*>(nullptr)))) {
        unsigned n = 0;
        for (;;) {
            io_uring_cqe* cqe = nullptr;
            if (::io_uring_peek_cqe(&ring_, &cqe) != 0 || cqe == nullptr) {
                break;
            }
            const io_uring_cqe copy = *cqe;
            ::io_uring_cqe_seen(&ring_, cqe);
            f(const_cast<io_uring_cqe*>(&copy));
            ++n;
        }
        return n;
    }

    std::uint64_t enters() const noexcept { return enters_; }

private:
    io_uring ring_{};
    bool ok_ = false;
    bool sqe128_ = false;
    std::uint64_t enters_ = 0;
};

}
