// examples/eventfd_device.cc — M5 acceptance: a custom device driver in
// one file, ZERO core changes.
//
// What a driver is in iox (design §六):
//   1. a HANDLE type you define (counterdev::device::handle below) —
//   2. tag_invoke overloads for the vocabulary operations it supports
//      (this one: io::read) —
//   3. completions through the op_base thunk protocol, delivered by
//      io_context::dispatch(op_address, res, flags) — usually funneled
//      through a completion_source (three attachment modes; this device
//      uses the fd-mounted mode) —
//   4. optional one-liners: io::supports capability answers, the
//      registered_driver trait.
//
// The eventfd here plays the role an IRQ fd plays for real hardware: the
// device OWNS it, the loop polls it through the completion_source bridge,
// and on_ready() turns "fd readable" into typed completions for parked
// operations. From the caller's side, io::read on the device handle looks
// exactly like io::read on a pipe — same CPO, same composition.
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdio>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include <iox/core/exec.h>
#include <iox/driver/capabilities.h>
#include <iox/driver/completion_source.h>
#include <iox/driver/registry.h>
#include <iox/ops.h>

namespace ex = iox::exec;
using namespace iox;
using namespace std::chrono_literals;

namespace counterdev {

// ---------------------------------------------------------------------------
// (3) the device: owns the eventfd, parks pending ops, bridges completions
// ---------------------------------------------------------------------------
class device;

struct handle {
    device* dev = nullptr;
};

class device final : public iox::completion_source {
public:
    device() : efd_(::eventfd(0, EFD_CLOEXEC)) {}
    ~device() override { ::close(efd_); }
    device(const device&) = delete;
    device& operator=(const device&) = delete;

    // fd-mounted mode: the eventfd IS the readiness fd. io_context arms one
    // readiness poll on it at attach — no core changes, no busy CPU.
    iox::fd completion_fd() const noexcept override { return iox::fd{efd_}; }

    // The fd is readable → drain the doorbell and dispatch every parked op.
    // Runs on the io thread; `ctx.dispatch` re-enters the loop's normal
    // completion path, so a device completion is indistinguishable from a
    // CQE once armed (§三.① — no vtable on the data path).
    void on_ready(io_context& ctx) noexcept override {
        std::uint64_t sample = 0;
        const ssize_t n = ::read(efd_, &sample, sizeof(sample)); // sums + resets
        (void)n;
        for (iox::op_base* op : pending_) {
            ctx.dispatch(reinterpret_cast<std::uint64_t>(op),
                         static_cast<std::int32_t>(sample), 0);
        }
        pending_.clear();
    }

    // The "hardware": add `v` to the counter and raise the interrupt.
    void interrupt(std::uint64_t v) noexcept {
        const std::uint64_t one = v;
        const ssize_t rang = ::write(efd_, &one, sizeof(one));
        (void)rang;
    }

    // Called from the operation's start() on the io thread.
    void submit(iox::op_base* op) noexcept { pending_.push_back(op); }

private:
    int efd_;
    std::vector<iox::op_base*> pending_; // io thread only
};

// ---------------------------------------------------------------------------
// (3) the operation: op_base thunk protocol, payload rides in the op state.
// res is just a signal — here it carries the counter sample itself.
// ---------------------------------------------------------------------------
template <class R>
struct read_op final : iox::op_base {
    R r;

    explicit read_op(R&& recv) noexcept
        : op_base(&read_op::on_done), r(std::move(recv)) {}

    using operation_state_concept = stdexec::operation_state_tag;

    static void on_done(op_base* self, io_context&, std::int32_t res,
                        std::uint32_t) noexcept {
        auto* o = static_cast<read_op*>(self);
        if (res < 0) {
            stdexec::set_error(std::move(o->r), iox::error::from_negative(res));
        } else {
            // domain-typed value, like io::accept completing with a Socket
            stdexec::set_value(std::move(o->r), static_cast<std::uint64_t>(res));
        }
    }

    void start() noexcept { dev->submit(this); }

    device* dev = nullptr; // set by the sender's connect
};

// ---------------------------------------------------------------------------
// (2) the sender io::read returns for this handle — same shape fd senders
// have: context-free, connected where the caller composes it.
// ---------------------------------------------------------------------------
struct read_sender {
    device* dev;

    using sender_concept = stdexec::sender_tag;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(std::uint64_t),
                                       stdexec::set_error_t(iox::error)>;

    template <class Self, class R>
    auto connect(this Self&& self, R&& r) {
        read_op<std::remove_cvref_t<R>> op{std::forward<R>(r)};
        op.dev = self.dev;
        return op;
    }
};

// (2) vocabulary customization: io::read on our handle. Found by ADL; beats
// the fd-driver default (the handle is not fd-backed at all).
inline read_sender tag_invoke(io::read_t, io_context&, handle& h, wbytes) noexcept {
    return read_sender{h.dev};
}

// (4) capability answers: this device keeps no user buffers, so zero-copy
// is off (fd handles answer "true" by default — this override wins).
inline bool tag_invoke(io::detail::supports_t, io::zero_copy_t, const handle&) noexcept {
    return false;
}

} // namespace counterdev

// (4) compile-time driver registration — the core never changes.
namespace iox::driver {
template <>
inline constexpr bool registered_driver<counterdev::device> = true;
} // namespace iox::driver

static_assert(iox::driver::registered_driver<counterdev::device>);

// ---------------------------------------------------------------------------

int main() {
    using namespace iox;
    io_context ctx;

    counterdev::device dev;
    ctx.attach_source(dev); // fd-mounted: one line, zero core changes
    counterdev::handle h{&dev};

    // The "hardware": two interrupts, 30 ms apart, from another thread —
    // the shape a completion IRQ arriving from a device queue has.
    std::thread producer([&] {
        std::this_thread::sleep_for(30ms);
        dev.interrupt(7);
        std::this_thread::sleep_for(30ms);
        dev.interrupt(9);
    });

    // Same vocabulary, same composition, as any readable handle:
    const auto first = ex::sync_wait(ctx, io::read(ctx, h, {}));
    const auto second = ex::sync_wait(ctx, io::read(ctx, h, {}));
    producer.join();

    if (!first || !second) {
        std::fprintf(stderr, "eventfd_device: read failed\n");
        return 1;
    }
    const auto a = std::get<0>(*first.value);
    const auto b = std::get<0>(*second.value);

    ctx.detach_source(dev);
    ctx.run_for(20ms); // retire the readiness poll before teardown

    std::printf("eventfd_device: OK (samples %llu, %llu; zero_copy=%d dma=%d)\n",
                static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                io::supports(io::zero_copy, h) ? 1 : 0,
                io::supports(io::dma, h) ? 1 : 0);
    return (a == 7 && b == 9) ? 0 : 1;
}
