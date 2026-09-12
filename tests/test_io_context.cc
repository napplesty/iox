// Tests for io_context: completion dispatch/injection, the event loop,
// batching (enter counts), run_for, and sync_wait integration.
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

} // namespace

TEST_CASE("dispatch routes a completion to its op") {
    io_context ctx;
    struct spy_op final : op_base {
        std::int32_t last_res = -999;
        std::uint32_t last_flags = 999;
        int fired = 0;

        static void thunk(op_base* self, io_context&, std::int32_t res, std::uint32_t flags) noexcept {
            auto* o = static_cast<spy_op*>(self);
            o->last_res = res;
            o->last_flags = flags;
            ++o->fired;
        }

        spy_op() noexcept : op_base(&spy_op::thunk) {}
    };

    spy_op op;
    ctx.dispatch(reinterpret_cast<std::uint64_t>(&op), 42, 7);
    CHECK(op.fired == 1);
    CHECK(op.last_res == 42);
    CHECK(op.last_flags == 7);

    // This is the fake-CQE injection path: no kernel interaction happened.
    CHECK(ctx.ring().enters() == 0);
}

TEST_CASE("run returns when a completion calls stop") {
    io_context ctx;
    auto flow = io::schedule(ctx) | ex::then([&] { ctx.stop(); });
    auto r = ex::sync_wait(ctx, flow);
    CHECK(r);
}

TEST_CASE("batch_scope merges flushes into one enter") {
    io_context ctx;
    int count = 0;

    std::array<std::optional<schedule_op_t>, 4> ops;
    {
        batch_scope scope{ctx};
        for (auto& slot : ops) {
            slot.emplace(ex::connect(io::schedule(ctx), counting_receiver{&count}));
        }
        // Arm everything without leaving the scope.
        for (auto& slot : ops) {
            ex::start(*slot);
        }
        // An explicit flush inside the scope must be deferred.
        ctx.flush();
        CHECK(ctx.ring().enters() == 0);
    } // outermost scope exit flushes once

    CHECK(ctx.ring().enters() == 1);

    ctx.run_for(20ms);
    CHECK(count == 4);
}

TEST_CASE("flush without a scope goes straight to the kernel") {
    io_context ctx;
    int count = 0;
    auto op = ex::connect(io::schedule(ctx), counting_receiver{&count});
    ex::start(op);
    CHECK(ctx.ring().enters() == 0); // start does not submit on its own
    ctx.flush();
    CHECK(ctx.ring().enters() == 1);
    ctx.run_for(20ms);
    CHECK(count == 1);
}

TEST_CASE("run_for honors its deadline") {
    io_context ctx;
    const auto t0 = std::chrono::steady_clock::now();
    ctx.run_for(30ms);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed >= 25ms);
    CHECK(elapsed < 5s); // sanity, generous margin
}

TEST_CASE("run_for returns early when stopped by a completion") {
    io_context ctx;
    int count = 0;
    auto op = ex::connect(io::schedule(ctx) | ex::then([&] { ctx.stop(); }),
                            counting_receiver{&count});
    ex::start(op);
    const auto t0 = std::chrono::steady_clock::now();
    ctx.run_for(5s);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < 1s);
    CHECK(count == 1);
}

TEST_CASE("stale run_for deadline does not stop a later run") {
    io_context ctx;
    ctx.run_for(10ms); // arms and fires deadline #1

    int count = 0;
    auto op = ex::connect(io::schedule(ctx), counting_receiver{&count});
    ex::start(op);
    // If the stale deadline were still effective, run_for would return
    // immediately with count == 0 (the schedule op would not have completed).
    ctx.run_for(100ms);
    CHECK(count == 1);
}

TEST_CASE("run_for leaves the loop runnable for a later run()") {
    io_context ctx;
    ctx.run_for(10ms); // deadline fires → stop() ends the invocation

    // The trap this guards: run() after run_for must pump, not no-op on the
    // stale stopped flag — otherwise in-flight SQEs are stranded and their
    // senders never complete.
    int count = 0;
    auto op = ex::connect(io::schedule(ctx), counting_receiver{&count});
    ex::start(op);
    ctx.run_for(5s);
    CHECK(count == 1);

    // Plain run() (no deadline): the completion itself stops the loop.
    auto again = ex::connect(io::schedule(ctx) | ex::then([&] { ctx.stop(); }),
                             counting_receiver{&count});
    ex::start(again);
    ctx.run();
    CHECK(count == 2);
}

TEST_CASE("sync_wait drives pure-algorithm senders without blocking") {
    io_context ctx;
    auto r = ex::sync_wait(ctx, ex::just(42) | ex::then([](int x) { return x + 1; }));
    REQUIRE(r);
    CHECK(std::get<0>(*r) == 43);
    // No uring op was ever submitted, and sync_wait must not have blocked.
    CHECK(ctx.ring().enters() == 0);
}

TEST_CASE("sync_wait runs the loop for uring-backed senders") {
    io_context ctx;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = ex::sync_wait(ctx, io::sleep_for(ctx, 20ms));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(r);
    CHECK(elapsed >= 18ms);
}
