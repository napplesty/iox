// iox — unified async IO for Linux
// tests/test_io_context.cc — batching (enter counts), run_for, and sync_wait integration.
#include <doctest/doctest.h>

#include <chrono>
#include <array>
#include <optional>

#include <iox/core/exec.h>
#include <iox/ops.h>

using namespace std::chrono_literals;
namespace ex = iox::exec;
using namespace iox;

namespace {

struct counting_receiver {
    using receiver_concept = stdexec::receiver_tag;
    int* count = nullptr;

    stdexec::env<> get_env() const noexcept { return {}; }
    template <class... As>
    void set_value(As&&...) && noexcept { ++*count; }
    void set_error(iox::error) && noexcept { ++*count; }
    void set_error(std::exception_ptr) && noexcept { ++*count; }
    void set_stopped() && noexcept {}
};

using schedule_op_t = decltype(stdexec::connect(io::schedule(std::declval<io_context&>()),
                                                counting_receiver{}));

}

TEST_CASE("dispatch routes a completion to its op") {
    io_context context;
    struct spy_op final : op_base {
        std::int32_t last_result = -999;
        std::uint32_t last_flags = 999;
        int fired = 0;

        static void thunk(op_base* self, io_context&, std::int32_t result, std::uint32_t flags) noexcept {
            auto* operation = static_cast<spy_op*>(self);
            operation->last_result = result;
            operation->last_flags = flags;
            ++operation->fired;
        }

        spy_op() noexcept : op_base(&spy_op::thunk) {}
    };

    spy_op operation;
    context.dispatch(reinterpret_cast<std::uint64_t>(&operation), 42, 7);
    CHECK(operation.fired == 1);
    CHECK(operation.last_result == 42);
    CHECK(operation.last_flags == 7);

    CHECK(context.ring().enters() == 0);
}

TEST_CASE("run returns when a completion calls stop") {
    io_context context;
    auto flow = io::schedule(context) | ex::then([&] { context.stop(); });
    auto result = ex::sync_wait(context, flow);
    CHECK(result);
}

TEST_CASE("batch_scope merges flushes into one enter") {
    io_context context;
    int count = 0;

    std::array<std::optional<schedule_op_t>, 4> operations;
    {
        batch_scope scope{context};
        for (auto& slot : operations) {
            slot.emplace(ex::connect(io::schedule(context), counting_receiver{&count}));
        }
        for (auto& slot : operations) {
            ex::start(*slot);
        }
        context.flush();
        CHECK(context.ring().enters() == 0);
    }

    CHECK(context.ring().enters() == 1);

    context.run_for(20ms);
    CHECK(count == 4);
}

TEST_CASE("flush without a scope goes straight to the kernel") {
    io_context context;
    int count = 0;
    auto operation = ex::connect(io::schedule(context), counting_receiver{&count});
    ex::start(operation);
    CHECK(context.ring().enters() == 0);
    context.flush();
    CHECK(context.ring().enters() == 1);
    context.run_for(20ms);
    CHECK(count == 1);
}

TEST_CASE("run_for honors its deadline") {
    io_context context;
    const auto start_time = std::chrono::steady_clock::now();
    context.run_for(30ms);
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    CHECK(elapsed >= 25ms);
    CHECK(elapsed < 5s);
}

TEST_CASE("run_for returns early when stopped by a completion") {
    io_context context;
    int count = 0;
    auto operation = ex::connect(io::schedule(context) | ex::then([&] { context.stop(); }),
                            counting_receiver{&count});
    ex::start(operation);
    const auto start_time = std::chrono::steady_clock::now();
    context.run_for(5s);
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    CHECK(elapsed < 1s);
    CHECK(count == 1);
}

TEST_CASE("stale run_for deadline does not stop a later run") {
    io_context context;
    context.run_for(10ms);

    int count = 0;
    auto operation = ex::connect(io::schedule(context), counting_receiver{&count});
    ex::start(operation);
    context.run_for(100ms);
    CHECK(count == 1);
}

TEST_CASE("run_for leaves the loop runnable for a later run()") {
    io_context context;
    context.run_for(10ms);

    int count = 0;
    auto operation = ex::connect(io::schedule(context), counting_receiver{&count});
    ex::start(operation);
    context.run_for(5s);
    CHECK(count == 1);

    auto again = ex::connect(io::schedule(context) | ex::then([&] { context.stop(); }),
                             counting_receiver{&count});
    ex::start(again);
    context.run();
    CHECK(count == 2);
}

TEST_CASE("sync_wait drives pure-algorithm senders without blocking") {
    io_context context;
    auto result = ex::sync_wait(context, ex::just(42) | ex::then([](int value) { return value + 1; }));
    REQUIRE(result);
    CHECK(std::get<0>(*result) == 43);
    CHECK(context.ring().enters() == 0);
}

TEST_CASE("sync_wait runs the loop for uring-backed senders") {
    io_context context;
    const auto start_time = std::chrono::steady_clock::now();
    auto result = ex::sync_wait(context, io::sleep_for(context, 20ms));
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    REQUIRE(result);
    CHECK(elapsed >= 18ms);
}
