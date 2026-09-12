// iox — unified async IO for Linux
// examples/eventfd_device.cc — one file, ZERO core changes.
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

    iox::fd completion_fd() const noexcept override { return iox::fd{efd_}; }

    void on_ready(io_context& ctx) noexcept override {
        std::uint64_t sample = 0;
        const ssize_t n = ::read(efd_, &sample, sizeof(sample));
        (void)n;
        for (iox::op_base* op : pending_) {
            ctx.dispatch(reinterpret_cast<std::uint64_t>(op),
                         static_cast<std::int32_t>(sample), 0);
        }
        pending_.clear();
    }

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
            stdexec::set_value(std::move(o->r), static_cast<std::uint64_t>(res));
        }
    }

    void start() noexcept { dev->submit(this); }

    device* dev = nullptr;
};

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

inline read_sender tag_invoke(io::read_t, io_context&, handle& h, wbytes) noexcept {
    return read_sender{h.dev};
}

inline bool tag_invoke(io::detail::supports_t, io::zero_copy_t, const handle&) noexcept {
    return false;
}

}

  // (4) compile-time driver registration — the core never changes.
namespace iox::driver {
template <>
inline constexpr bool registered_driver<counterdev::device> = true;
}

static_assert(iox::driver::registered_driver<counterdev::device>);

int main() {
    using namespace iox;
    io_context ctx;

    counterdev::device dev;
    ctx.attach_source(dev);
    counterdev::handle h{&dev};

    std::thread producer([&] {
        std::this_thread::sleep_for(30ms);
        dev.interrupt(7);
        std::this_thread::sleep_for(30ms);
        dev.interrupt(9);
    });

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
    ctx.run_for(20ms);

    std::printf("eventfd_device: OK (samples %llu, %llu; zero_copy=%d dma=%d)\n",
                static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                io::supports(io::zero_copy, h) ? 1 : 0,
                io::supports(io::dma, h) ? 1 : 0);
    return (a == 7 && b == 9) ? 0 : 1;
}
