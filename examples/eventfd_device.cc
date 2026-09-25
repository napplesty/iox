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
    device* device_pointer = nullptr;
};

class device final : public iox::completion_source {
public:
    device() : event_fd_(::eventfd(0, EFD_CLOEXEC)) {}
    ~device() override { ::close(event_fd_); }
    device(const device&) = delete;
    device& operator=(const device&) = delete;

    iox::fd completion_fd() const noexcept override { return iox::fd{event_fd_}; }

    void on_ready(io_context& context) noexcept override {
        std::uint64_t sample = 0;
        const ssize_t count = ::read(event_fd_, &sample, sizeof(sample));
        (void)count;
        for (iox::op_base* operation : pending_) {
            context.dispatch(reinterpret_cast<std::uint64_t>(operation),
                             static_cast<std::int32_t>(sample), 0);
        }
        pending_.clear();
    }

    void interrupt(std::uint64_t value) noexcept {
        const std::uint64_t one = value;
        const ssize_t written = ::write(event_fd_, &one, sizeof(one));
        (void)written;
    }

  // Called from the operation's start() on the io thread.
    void submit(iox::op_base* operation) noexcept { pending_.push_back(operation); }

private:
    int event_fd_;
    std::vector<iox::op_base*> pending_; // io thread only
};

template <class Receiver>
struct read_op final : iox::op_base {
    Receiver receiver;

    explicit read_op(Receiver&& receiver) noexcept
        : op_base(&read_op::on_done), receiver(std::move(receiver)) {}

    using operation_state_concept = stdexec::operation_state_tag;

    static void on_done(op_base* self, io_context&, std::int32_t result,
                        std::uint32_t) noexcept {
        auto* operation = static_cast<read_op*>(self);
        if (result < 0) {
            stdexec::set_error(std::move(operation->receiver), iox::error::from_negative(result));
        } else {
            stdexec::set_value(std::move(operation->receiver), static_cast<std::uint64_t>(result));
        }
    }

    void start() noexcept { device_pointer->submit(this); }

    device* device_pointer = nullptr;
};

struct read_sender {
    device* device_pointer;

    using sender_concept = stdexec::sender_tag;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(std::uint64_t),
                                       stdexec::set_error_t(iox::error)>;

    template <class Self, class Receiver>
    auto connect(this Self&& self, Receiver&& receiver) {
        read_op<std::remove_cvref_t<Receiver>> operation{std::forward<Receiver>(receiver)};
        operation.device_pointer = self.device_pointer;
        return operation;
    }
};

inline read_sender tag_invoke(io::read_t, io_context&, handle& handle, wbytes) noexcept {
    return read_sender{handle.device_pointer};
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
    io_context context;

    counterdev::device device;
    context.attach_source(device);
    counterdev::handle handle{&device};

    std::thread producer([&] {
        std::this_thread::sleep_for(30ms);
        device.interrupt(7);
        std::this_thread::sleep_for(30ms);
        device.interrupt(9);
    });

    const auto first = ex::sync_wait(context, io::read(context, handle, iox::wbytes{}));
    const auto second = ex::sync_wait(context, io::read(context, handle, iox::wbytes{}));
    producer.join();

    if (!first || !second) {
        std::fprintf(stderr, "eventfd_device: read failed\n");
        return 1;
    }
    const auto first_sample = std::get<0>(*first.value);
    const auto second_sample = std::get<0>(*second.value);

    context.detach_source(device);
    context.run_for(20ms);

    std::printf("eventfd_device: OK (samples %llu, %llu; zero_copy=%d dma=%d)\n",
                static_cast<unsigned long long>(first_sample), static_cast<unsigned long long>(second_sample),
                io::supports(io::zero_copy, handle) ? 1 : 0,
                io::supports(io::dma, handle) ? 1 : 0);
    return (first_sample == 7 && second_sample == 9) ? 0 : 1;
}
