// bench/nvme_rw.cc — random 4 KiB READ throughput through the nvme driver
// vs a hand-rolled raw-liburing uring_cmd loop (the M6 gate: >=85% of raw).
// Self-skips when the passthru node is not accessible.
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
constexpr std::uint64_t kSpan = 1ULL << 30; // random LBAs from the first GiB

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

int main() {
    const char* path = std::getenv("IOX_NVME_PATH");
    auto dev = nvme::device::open(path != nullptr ? path : "/dev/ng0n1");
    if (!dev) {
        std::printf("nvme_rw: skip (%s)\n", dev.error().message().c_str());
        return 0;
    }
    const unsigned lbs = dev->lba_size();
    const std::size_t len = kReadLen / lbs * lbs;
    const std::uint64_t lba_span = kSpan >> static_cast<unsigned>(__builtin_ctz(lbs));
    std::mt19937_64 rng(42);

    // ---- iox side ------------------------------------------------------------
    // One detached io::loop per queue slot: every iteration is one read_at;
    // the loop retires when the window closes. Library composition all the
    // way — this is how a user drives queue depth today.
    {
        io_context ctx{uring::ring_params{.entries = 512, .sqe128 = true}};
        auto buf = std::make_unique_for_overwrite<std::byte[]>(len * kQD);
        const unsigned lba_shift = static_cast<unsigned>(__builtin_ctz(lbs));
        std::uint64_t done = 0;
        const auto deadline = std::chrono::steady_clock::now() + kWindow;

        for (unsigned i = 0; i < kQD; ++i) {
            ex::detach(io::loop(ctx, [&, slot = i]() {
                const std::uint64_t lba = rng() % lba_span;
                return io::read_at(ctx, *dev, wbytes{buf.get() + len * slot, len},
                                   uoffset_t{lba << lba_shift})
                     | ex::then([&](std::size_t) {
                           ++done;
                           return std::chrono::steady_clock::now() >= deadline;
                       });
            }));
        }
        ctx.run_for(kWindow + 1s); // let in-flight iterations retire

        const double iops = static_cast<double>(done) / kWindow.count();
        std::printf("iox  : %.0f IOPS (%.2f MiB/s)\n", iops,
                    iops * len / (1024.0 * 1024.0));
        std::printf("[iox-done=%llu]\n", static_cast<unsigned long long>(done));
    }

    // ---- raw liburing side ---------------------------------------------------
    {
        io_uring_params p{};
        p.flags = IORING_SETUP_SQE128;
        io_uring ring;
        if (io_uring_queue_init_params(512, &ring, &p) != 0) {
            std::printf("raw  : ring init failed\n");
            return 1;
        }
        auto bufs = std::make_unique_for_overwrite<std::byte[]>(len * kQD);
        std::uint64_t done = 0;
        const auto deadline = std::chrono::steady_clock::now() + kWindow;

        auto prep_one = [&](unsigned slot) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (sqe == nullptr) {
                return false;
            }
            ::nvme_uring_cmd cmd{};
            cmd.opcode = 0x02; // read
            cmd.nsid = dev->nsid();
            cmd.addr = reinterpret_cast<std::uint64_t>(bufs.get() + len * slot);
            cmd.data_len = static_cast<std::uint32_t>(len);
            const std::uint64_t lba = rng() % lba_span;
            cmd.cdw10 = static_cast<std::uint32_t>(lba);
            cmd.cdw11 = static_cast<std::uint32_t>(lba >> 32);
            cmd.cdw12 = static_cast<std::uint32_t>(len / lbs - 1);
            cmd.timeout_ms = 30'000;
            io_uring_prep_uring_cmd(sqe, NVME_URING_CMD_IO, dev->fd_slot()->v);
            std::memcpy(reinterpret_cast<void*>(sqe->cmd), &cmd, sizeof(cmd));
            io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(slot) + 1); // 0 = unused
            return true;
        };

        for (unsigned i = 0; i < kQD; ++i) {
            if (!prep_one(i)) {
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
                    iops * len / (1024.0 * 1024.0));
        io_uring_queue_exit(&ring);
    }
    return 0;
}
