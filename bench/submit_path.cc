// iox — unified async IO for Linux
// bench/submit_path.cc — (CPO → sender → connect → start → SQE) versus the raw liburing floor
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include <iox/core/exec.h>
#include <iox/runtime/io_context.h>
#include <iox/ops.h>

using namespace std::chrono_literals;

namespace {

using clock_t_ = std::chrono::steady_clock;

struct counting_receiver {
    using receiver_concept = stdexec::receiver_tag;
    std::uint64_t* n = nullptr;
    iox::io_context* ctx = nullptr;
    std::uint64_t stop_target = 0;

    stdexec::env<> get_env() const noexcept { return {}; }
    template <class... As>
    void set_value(As&&...) && noexcept {
        if (++*n == stop_target) {
            ctx->stop();
        }
    }
    void set_error(iox::error) && noexcept { ++*n; }
    void set_error(std::exception_ptr) && noexcept { ++*n; }
    void set_stopped() && noexcept {}
};

double bench_raw(std::uint64_t n) {
    iox::uring::ring ring{iox::uring::ring_params{.entries = 1024,
                                                  .cq_entries = static_cast<unsigned>(n)}};
    if (!ring.ok()) {
        std::fprintf(stderr, "raw bench: ring init failed (try a smaller n)\n");
        return -1.0;
    }

    struct raw_op final : iox::op_base {
        static void thunk(iox::op_base*, iox::io_context&, std::int32_t, std::uint32_t) noexcept {}
        raw_op() noexcept : iox::op_base(&raw_op::thunk) {}
    };
    std::vector<raw_op> ops(n);

    const auto t0 = clock_t_::now();
    for (auto& op : ops) {
        io_uring_sqe* sqe = ring.next_sqe();
        if (sqe == nullptr) {
            ring.flush();
            sqe = ring.next_sqe();
        }
        ::io_uring_prep_nop(sqe);
        ::io_uring_sqe_set_data(sqe, &op);
    }
    std::uint64_t done = 0;
    while (done < n) {
        ring.flush_and_wait(1);
        done += ring.for_each_cqe([](io_uring_cqe*) {});
    }
    const auto t1 = clock_t_::now();

    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n);
}

double bench_iox(std::uint64_t n) {
    iox::io_context ctx{iox::uring::ring_params{
        .entries = 1024, .cq_entries = static_cast<unsigned>(n)}};
    if (!ctx.ok()) {
        std::fprintf(stderr, "iox bench: ring init failed (try a smaller n)\n");
        return -1.0;
    }

    using op_t = decltype(stdexec::connect(
        iox::io::schedule(std::declval<iox::io_context&>()), counting_receiver{}));
    std::vector<std::optional<op_t>> ops(n);
    std::uint64_t count = 0;

    const auto t0 = clock_t_::now();
    {
        iox::batch_scope scope{ctx};
        for (auto& slot : ops) {
            slot.emplace(stdexec::connect(
                iox::io::schedule(ctx),
                counting_receiver{&count, &ctx, n}));
            stdexec::start(*slot);
        }
    }
    ctx.run_for(2s);
    const auto t1 = clock_t_::now();

    if (count != n) {
        std::fprintf(stderr, "iox bench: lost completions (%llu/%llu)\n",
                     static_cast<unsigned long long>(count),
                     static_cast<unsigned long long>(n));
        return -1.0;
    }
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n);
}

}

int main(int argc, char** argv) {
    const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 65536;

    const double raw = bench_raw(n);
    const double ioxd = bench_iox(n);
    if (raw <= 0.0 || ioxd <= 0.0) {
        return 1;
    }

    std::printf("submit+complete path, %llu nops (lower is better)\n", static_cast<unsigned long long>(n));
    std::printf("  raw liburing : %8.1f ns/op  (%.2f Mops/s)\n", raw, 1000.0 / raw);
    std::printf("  iox          : %8.1f ns/op  (%.2f Mops/s)\n", ioxd, 1000.0 / ioxd);
    std::printf("  ratio        : %8.1f %%  of raw throughput\n", 100.0 * raw / ioxd);
    return 0;
}
