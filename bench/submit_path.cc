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
    std::uint64_t* count = nullptr;
    iox::io_context* context = nullptr;
    std::uint64_t stop_target = 0;

    stdexec::env<> get_env() const noexcept { return {}; }
    template <class... As>
    void set_value(As&&...) && noexcept {
        if (++*count == stop_target) {
            context->stop();
        }
    }
    void set_error(iox::error) && noexcept { ++*count; }
    void set_error(std::exception_ptr) && noexcept { ++*count; }
    void set_stopped() && noexcept {}
};

double bench_raw(std::uint64_t count) {
    iox::uring::ring ring{iox::uring::ring_params{.entries = 1024,
                                                  .cq_entries = static_cast<unsigned>(count)}};
    if (!ring.ok()) {
        std::fprintf(stderr, "raw bench: ring init failed (try a smaller n)\n");
        return -1.0;
    }

    struct raw_op final : iox::op_base {
        static void thunk(iox::op_base*, iox::io_context&, std::int32_t, std::uint32_t) noexcept {}
        raw_op() noexcept : iox::op_base(&raw_op::thunk) {}
    };
    std::vector<raw_op> operations(count);

    const auto start_time = clock_t_::now();
    for (auto& operation : operations) {
        io_uring_sqe* sqe = ring.next_sqe();
        if (sqe == nullptr) {
            ring.flush();
            sqe = ring.next_sqe();
        }
        ::io_uring_prep_nop(sqe);
        ::io_uring_sqe_set_data(sqe, &operation);
    }
    std::uint64_t done = 0;
    while (done < count) {
        ring.flush_and_wait(1);
        done += ring.for_each_cqe([](io_uring_cqe*) {});
    }
    const auto end_time = clock_t_::now();

    return std::chrono::duration<double, std::nano>(end_time - start_time).count() / static_cast<double>(count);
}

double bench_iox(std::uint64_t target) {
    iox::io_context context{iox::uring::ring_params{
        .entries = 1024, .cq_entries = static_cast<unsigned>(target)}};
    if (!context.ok()) {
        std::fprintf(stderr, "iox bench: ring init failed (try a smaller n)\n");
        return -1.0;
    }

    using op_t = decltype(stdexec::connect(
        iox::io::schedule(std::declval<iox::io_context&>()), counting_receiver{}));
    std::vector<std::optional<op_t>> operations(target);
    std::uint64_t count = 0;

    const auto start_time = clock_t_::now();
    {
        iox::batch_scope scope{context};
        for (auto& slot : operations) {
            slot.emplace(stdexec::connect(
                iox::io::schedule(context),
                counting_receiver{&count, &context, target}));
            stdexec::start(*slot);
        }
    }
    context.run_for(2s);
    const auto end_time = clock_t_::now();

    if (count != target) {
        std::fprintf(stderr, "iox bench: lost completions (%llu/%llu)\n",
                     static_cast<unsigned long long>(count),
                     static_cast<unsigned long long>(target));
        return -1.0;
    }
    return std::chrono::duration<double, std::nano>(end_time - start_time).count() / static_cast<double>(target);
}

}

int main(int argc, char** argv) {
    const std::uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 65536;

    const double raw_ns_per_op = bench_raw(count);
    const double iox_ns_per_op = bench_iox(count);
    if (raw_ns_per_op <= 0.0 || iox_ns_per_op <= 0.0) {
        return 1;
    }

    std::printf("submit+complete path, %llu nops (lower is better)\n", static_cast<unsigned long long>(count));
    std::printf("  raw liburing : %8.1f ns/op  (%.2f Mops/s)\n", raw_ns_per_op, 1000.0 / raw_ns_per_op);
    std::printf("  iox          : %8.1f ns/op  (%.2f Mops/s)\n", iox_ns_per_op, 1000.0 / iox_ns_per_op);
    std::printf("  ratio        : %8.1f %%  of raw throughput\n", 100.0 * raw_ns_per_op / iox_ns_per_op);
    return 0;
}
