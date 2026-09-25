// iox — unified async IO for Linux
// tests/support/softdev.h — the software test device for the Driver SPI
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

class device;

struct handle {
    device* dev = nullptr;
};

class device final : public iox::completion_source {
public:
    enum class mode { fd_mounted, busy_slot, active };

    static constexpr std::int32_t record_bytes = 8;

    explicit device(mode initial_mode, bool self_detach_after_drain = false)
        : mode_(initial_mode), self_detach_(self_detach_after_drain),
          efd_(initial_mode == mode::fd_mounted ? ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) : -1) {}

    ~device() override {
        if (efd_ >= 0) {
            ::close(efd_);
        }
    }
    device(const device&) = delete;
    device& operator=(const device&) = delete;

    iox::fd completion_fd() const noexcept override {
        return mode_ == mode::fd_mounted ? iox::fd{efd_} : iox::fd{};
    }

  // busy mode only — io thread; no cross-thread state in this mode.
    bool has_work() const noexcept override {
        const auto now = std::chrono::steady_clock::now();
        for (const auto& entry : pending_) {
            if (entry.ready_at <= now) {
                return true;
            }
        }
        return false;
    }

    void on_ready(io_context& context) noexcept override {
        if (mode_ == mode::fd_mounted) {
            std::uint64_t counter = 0;
            const ssize_t drained = ::read(efd_, &counter, sizeof(counter));
            (void)drained;
        }
        drain_ready(context);
        if (self_detach_) {
            context.detach_source(*this);
        }
    }

    void submit(iox::op_base* operation) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        switch (mode_) {
        case mode::fd_mounted:
            pending_.push_back({operation, record_ready_ ? now : kNever});
            break;
        case mode::busy_slot:
            pending_.push_back({operation, now + 8ms});
            break;
        case mode::active:
            pending_.push_back({operation, now});
            break;
        }
    }

    void produce() {
        {
            std::lock_guard lock(mutex_);
            record_ready_ = true;
            const auto now = std::chrono::steady_clock::now();
            for (auto& entry : pending_) {
                entry.ready_at = now;
            }
        }
        const std::uint64_t one = 1;
        const ssize_t rang = ::write(efd_, &one, sizeof(one));
        (void)rang;
    }

    void complete_now(io_context& context) noexcept {
        {
            std::lock_guard lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            for (auto& entry : pending_) {
                entry.ready_at = now;
            }
        }
        drain_ready(context);
    }

  // Called from the op thunk on the io thread.
    std::uint64_t take_record() noexcept { return next_record_++; }

private:
    void drain_ready(io_context& context) noexcept {
        const auto now = std::chrono::steady_clock::now();
        std::vector<iox::op_base*> ready;
        {
            std::lock_guard lock(mutex_);
            for (std::size_t index = 0; index < pending_.size();) {
                if (pending_[index].ready_at <= now) {
                    ready.push_back(pending_[index].operation);
                    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
                } else {
                    ++index;
                }
            }
        }
        for (iox::op_base* operation : ready) {
            context.dispatch(reinterpret_cast<std::uint64_t>(operation), record_bytes, 0);
        }
    }

    static constexpr auto kNever = std::chrono::steady_clock::time_point::max();

    struct pend {
        iox::op_base* operation;
        std::chrono::steady_clock::time_point ready_at;
    };

    mode mode_;
    bool self_detach_;
    int efd_;
    std::mutex mutex_; // fd_mounted only: producer thread vs io thread
    std::vector<pend> pending_;
    bool record_ready_ = false;
    std::uint64_t next_record_ = 0; // io thread only
};

template <class Receiver>
struct read_op final : op_base {
    device* dev;
    wbytes destination;
    Receiver receiver;

    read_op(device* dev, wbytes destination, Receiver&& receiver) noexcept
        : op_base(&read_op::on_done), dev(dev), destination(destination), receiver(std::move(receiver)) {}

    using operation_state_concept = stdexec::operation_state_tag;

    static void on_done(op_base* self, io_context&, std::int32_t result,
                        std::uint32_t) noexcept {
        auto* operation = static_cast<read_op*>(self);
        if (result < 0) {
            stdexec::set_error(std::move(operation->receiver), iox::error::from_negative(result));
            return;
        }
        const std::uint64_t record = operation->dev->take_record();
        if (operation->destination.size() >= sizeof(record)) {
            std::memcpy(operation->destination.data(), &record, sizeof(record));
        }
        stdexec::set_value(std::move(operation->receiver), static_cast<std::size_t>(result));
    }

    void start() noexcept { dev->submit(this); }
};

struct read_sender {
    device* dev;
    wbytes destination;

    using sender_concept = stdexec::sender_tag;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                       stdexec::set_error_t(iox::error)>;

    template <class Self, class Receiver>
    auto connect(this Self&& self, Receiver&& receiver) {
        return read_op<std::remove_cvref_t<Receiver>>{self.dev, self.destination, std::forward<Receiver>(receiver)};
    }
};

inline read_sender tag_invoke(io::read_t, io_context&, handle& handle, wbytes destination) noexcept {
    return read_sender{handle.dev, destination};
}

inline bool tag_invoke(io::detail::supports_t, io::mmap_t, const handle&) noexcept {
    return true;
}
inline bool tag_invoke(io::detail::supports_t, io::zero_copy_t, const handle&) noexcept {
    return true;
}

static_assert(!iox::driver::registered_driver<iox::io_context>);

}

namespace iox::driver {
template <>
inline constexpr bool registered_driver<softdev::device> = true;
}

static_assert(iox::driver::registered_driver<softdev::device>);
