// iox — unified async IO for Linux
// tests/support/softdev.h — the software test device for the Driver SPI
// suite: pending ops park in a queue; readiness arrives by mode — fd_mounted
// (producer thread rings an eventfd doorbell), busy_slot (io_context polls
// has_work()), or active (io-thread continuation dispatches directly).
// Shared by tests/test_driver.cc; the tutorial-shaped single-file version
// lives in examples/eventfd_device.cc.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <mutex>
#include <thread>
#include <vector>

#include <iox/core/buffer.h>
#include <iox/driver/capabilities.h>
#include <iox/driver/completion_source.h>
#include <iox/driver/registry.h>
#include <iox/ops.h>
#include <iox/runtime/io_context.h>

using namespace std::chrono_literals;
namespace ex = iox::exec;
using namespace iox;


namespace softdev {

// ---------------------------------------------------------------------------
// The device: pending ops park in a queue; readiness arrives by MODE —
//   fd_mounted : a producer thread rings an eventfd doorbell (the "IRQ fd"
//                hardware shape; pending_ is cross-thread, mutex-guarded)
//   busy_slot  : readiness arrives with TIME (a device with neither fd nor
//                thread; io_context polls has_work() every ~1 ms)
//   active     : driver code already on the io thread (a timer continuation
//                here) dispatches directly — no attachment at all
// ---------------------------------------------------------------------------
class device;

struct handle {
    device* dev = nullptr;
};

class device final : public iox::completion_source {
public:
    enum class mode { fd_mounted, busy_slot, active };

    static constexpr std::int32_t record_bytes = 8;

    explicit device(mode m, bool self_detach_after_drain = false)
        : mode_(m), self_detach_(self_detach_after_drain),
          efd_(m == mode::fd_mounted ? ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) : -1) {}

    ~device() override {
        if (efd_ >= 0) {
            ::close(efd_);
        }
    }
    device(const device&) = delete;
    device& operator=(const device&) = delete;

    // ---- completion_source --------------------------------------------------
    iox::fd completion_fd() const noexcept override {
        return mode_ == mode::fd_mounted ? iox::fd{efd_} : iox::fd{};
    }

    // busy mode only — io thread; no cross-thread state in this mode.
    bool has_work() const noexcept override {
        const auto now = std::chrono::steady_clock::now();
        for (const auto& p : pending_) {
            if (p.ready_at <= now) {
                return true;
            }
        }
        return false;
    }

    void on_ready(io_context& ctx) noexcept override {
        if (mode_ == mode::fd_mounted) {
            std::uint64_t n = 0;
            const ssize_t drained = ::read(efd_, &n, sizeof(n)); // drain the doorbell
            (void)drained;
        }
        drain_ready(ctx);
        if (self_detach_) {
            // Documented legal: detaching from inside on_ready retires the
            // watch through the cancel path; the tick's index walk tolerates
            // the mid-loop erase.
            ctx.detach_source(*this);
        }
    }

    // ---- driver API ---------------------------------------------------------
    // Called from the op's start() on the io thread.
    void submit(iox::op_base* op) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lk(mu_);
        switch (mode_) {
        case mode::fd_mounted:
            // Not ready until the producer rings; a produce() that raced
            // ahead of submit still counts.
            pending_.push_back({op, record_ready_ ? now : kNever});
            break;
        case mode::busy_slot:
            pending_.push_back({op, now + 8ms}); // "hardware" finishes by itself
            break;
        case mode::active:
            pending_.push_back({op, now}); // drained by complete_now()
            break;
        }
    }

    // Producer thread (fd mode): one record became ready — ring the doorbell.
    void produce() {
        {
            std::lock_guard lk(mu_);
            record_ready_ = true;
            const auto now = std::chrono::steady_clock::now();
            for (auto& p : pending_) {
                p.ready_at = now;
            }
        }
        const std::uint64_t one = 1;
        const ssize_t rang = ::write(efd_, &one, sizeof(one));
        (void)rang;
    }

    // Io thread, active mode: driver code running on the loop thread
    // dispatches its parked completions directly (no attachment needed).
    void complete_now(io_context& ctx) noexcept {
        {
            std::lock_guard lk(mu_);
            const auto now = std::chrono::steady_clock::now();
            for (auto& p : pending_) {
                p.ready_at = now;
            }
        }
        drain_ready(ctx);
    }

    // Called from the op thunk on the io thread.
    std::uint64_t take_record() noexcept { return next_record_++; }

private:
    void drain_ready(io_context& ctx) noexcept {
        const auto now = std::chrono::steady_clock::now();
        std::vector<iox::op_base*> ready;
        {
            std::lock_guard lk(mu_);
            for (std::size_t i = 0; i < pending_.size();) {
                if (pending_[i].ready_at <= now) {
                    ready.push_back(pending_[i].op);
                    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }
        // Dispatch OUTSIDE the lock: the thunk re-enters the device
        // (take_record) and may complete the whole pipeline.
        for (iox::op_base* op : ready) {
            ctx.dispatch(reinterpret_cast<std::uint64_t>(op), record_bytes, 0);
        }
    }

    static constexpr auto kNever = std::chrono::steady_clock::time_point::max();

    struct pend {
        iox::op_base* op;
        std::chrono::steady_clock::time_point ready_at;
    };

    mode mode_;
    bool self_detach_;
    int efd_;
    std::mutex mu_; // fd_mounted only: producer thread vs io thread
    std::vector<pend> pending_;
    bool record_ready_ = false;
    std::uint64_t next_record_ = 0; // io thread only
};

// ---------------------------------------------------------------------------
// The operation: op_base thunk protocol, payload in the op state. The
// context dispatches (op address, res = record_bytes); the thunk copies the
// record into the caller's buffer and completes set_value(count) — the same
// completion signature io::read has everywhere (the unified front end).
// ---------------------------------------------------------------------------
template <class R>
struct read_op final : op_base {
    device* dev;
    wbytes dest;
    R r;

    read_op(device* d, wbytes w, R&& recv) noexcept
        : op_base(&read_op::on_done), dev(d), dest(w), r(std::move(recv)) {}

    using operation_state_concept = stdexec::operation_state_tag;

    static void on_done(op_base* self, io_context&, std::int32_t res,
                        std::uint32_t) noexcept {
        auto* o = static_cast<read_op*>(self);
        if (res < 0) {
            stdexec::set_error(std::move(o->r), iox::error::from_negative(res));
            return;
        }
        const std::uint64_t rec = o->dev->take_record();
        if (o->dest.size() >= sizeof(rec)) {
            std::memcpy(o->dest.data(), &rec, sizeof(rec));
        }
        stdexec::set_value(std::move(o->r), static_cast<std::size_t>(res));
    }

    void start() noexcept { dev->submit(this); }
};

struct read_sender {
    device* dev;
    wbytes dest;

    using sender_concept = stdexec::sender_tag;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                       stdexec::set_error_t(iox::error)>;

    template <class Self, class R>
    auto connect(this Self&& self, R&& r) {
        return read_op<std::remove_cvref_t<R>>{self.dev, self.dest, std::forward<R>(r)};
    }
};

// ---- vocabulary customization: io::read on the device handle --------------

inline read_sender tag_invoke(io::read_t, io_context&, handle& h, wbytes dest) noexcept {
    return read_sender{h.dev, dest};
}

// ---- capability customization: io::supports (fd defaults are zero_copy=
// true, mmap= false; the device overrides mmap — an mmio BAR, say) ---------

inline bool tag_invoke(io::detail::supports_t, io::mmap_t, const handle&) noexcept {
    return true;
}
inline bool tag_invoke(io::detail::supports_t, io::zero_copy_t, const handle&) noexcept {
    return true;
}

// ---- driver registration (compile-time; the core never changes) -----------

static_assert(!iox::driver::registered_driver<iox::io_context>);

} // namespace softdev

namespace iox::driver {
template <>
inline constexpr bool registered_driver<softdev::device> = true;
} // namespace iox::driver

static_assert(iox::driver::registered_driver<softdev::device>);
