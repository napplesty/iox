// iox — unified async IO for Linux
// bench/nvme_rw.cc — vs a hand-rolled raw-liburing uring_cmd loop (the M6 gate: >=85% of raw).
#include <fcntl.h>
#include <liburing.h>
#include <linux/nvme_ioctl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>

#include <iox/core/exec.h>
#include <iox/nvme/device.h>
#include <iox/nvme/io.h>
#include <iox/ops.h>
#include <iox/compose/detach.h>
#include <iox/compose/loop.h>

using namespace std::chrono_literals;
namespace ex = iox::exec;
using namespace iox;

namespace {

constexpr unsigned kQD = 16;
constexpr auto kWindow = 2s;
constexpr std::size_t kReadLen = 4096;
constexpr std::uint64_t kSpan = 1ULL << 30;

std::uint64_t now_nanoseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}

int main() {
    const char* path = std::getenv("IOX_NVME_PATH");
    auto device = nvme::device::open(path != nullptr ? path : "/dev/ng0n1");
    if (!device) {
        std::printf("nvme_rw: skip (%s)\n", device.error().message().c_str());
        return 0;
    }
    const unsigned lba_size = device->lba_size();
    const std::size_t length = kReadLen / lba_size * lba_size;
    const std::uint64_t lba_span = kSpan >> static_cast<unsigned>(__builtin_ctz(lba_size));
    std::mt19937_64 random_engine(42);

    {
        io_context context{uring::ring_params{.entries = 512, .sqe128 = true}};
        auto buffer = std::make_unique_for_overwrite<std::byte[]>(length * kQD);
        const unsigned lba_shift = static_cast<unsigned>(__builtin_ctz(lba_size));
        std::uint64_t done = 0;
        const auto deadline = std::chrono::steady_clock::now() + kWindow;

        for (unsigned index = 0; index < kQD; ++index) {
            ex::detach(io::loop(context, [&, slot = index]() {
                const std::uint64_t lba = random_engine() % lba_span;
                return io::read_at(context, *device, wbytes{buffer.get() + length * slot, length},
                                   uoffset_t{lba << lba_shift})
                     | ex::then([&](std::size_t) {
                           ++done;
                           return std::chrono::steady_clock::now() >= deadline;
                       });
            }));
        }
        context.run_for(kWindow + 1s);

        const double iops = static_cast<double>(done) / kWindow.count();
        std::printf("iox  : %.0f IOPS (%.2f MiB/s)\n", iops,
                    iops * length / (1024.0 * 1024.0));
        std::printf("[iox-done=%llu]\n", static_cast<unsigned long long>(done));
    }

    {
        io_uring_params params{};
        params.flags = IORING_SETUP_SQE128;
        io_uring ring;
        if (io_uring_queue_init_params(512, &ring, &params) != 0) {
            std::printf("raw  : ring init failed\n");
            return 1;
        }
        auto buffers = std::make_unique_for_overwrite<std::byte[]>(length * kQD);
        std::uint64_t done = 0;
        const auto deadline = std::chrono::steady_clock::now() + kWindow;

        auto prep_one = [&](unsigned slot) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (sqe == nullptr) {
                return false;
            }
            ::nvme_uring_cmd cmd{};
            cmd.opcode = 0x02;
            cmd.nsid = device->nsid();
            cmd.addr = reinterpret_cast<std::uint64_t>(buffers.get() + length * slot);
            cmd.data_len = static_cast<std::uint32_t>(length);
            const std::uint64_t lba = random_engine() % lba_span;
            cmd.cdw10 = static_cast<std::uint32_t>(lba);
            cmd.cdw11 = static_cast<std::uint32_t>(lba >> 32);
            cmd.cdw12 = static_cast<std::uint32_t>(length / lba_size - 1);
            cmd.timeout_ms = 30'000;
            io_uring_prep_uring_cmd(sqe, NVME_URING_CMD_IO, device->fd_slot()->v);
            std::memcpy(reinterpret_cast<void*>(sqe->cmd), &cmd, sizeof(cmd));
            io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(slot) + 1);
            return true;
        };

        for (unsigned index = 0; index < kQD; ++index) {
            if (!prep_one(index)) {
                break;
            }
        }
        io_uring_submit(&ring);
        while (std::chrono::steady_clock::now() < deadline) {
            io_uring_cqe* cqe = nullptr;
            io_uring_wait_cqe(&ring, &cqe);
            if (cqe == nullptr) {
                break;
            }
            const unsigned slot = static_cast<unsigned>(reinterpret_cast<std::uint64_t>(io_uring_cqe_get_data(cqe)) - 1);
            io_uring_cqe_seen(&ring, cqe);
            ++done;
            if (cqe->res < 0) {
                std::printf("raw  : CQE error %d\n", cqe->res);
                break;
            }
            if (prep_one(slot)) {
                io_uring_submit(&ring);
            }
        }
        const double iops = static_cast<double>(done) / kWindow.count();
        std::printf("raw  : %.0f IOPS (%.2f MiB/s)\n", iops,
                    iops * length / (1024.0 * 1024.0));
        io_uring_queue_exit(&ring);
    }
    return 0;
}
