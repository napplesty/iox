// iox — tests/test_cancel.cc: stop-token cancellation on the io thread and the cross-thread handoff.
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include <iox/core/exec.h>
#include <iox/ops.h>

#include "support/receivers.h"

using namespace std::chrono_literals;
using namespace iox;
namespace ex = iox::exec;

using test_rx::stop_counting_receiver;

TEST_CASE("cancel: in-flight sleep is canceled via stop token") {
    io_context context;
    ex::inplace_stop_source stop_source;
    int stopped = 0;
    int values = 0;

    auto operation = stdexec::connect(
        io::sleep_for(context, 10s),
        stop_counting_receiver{stop_source.get_token(), &stopped, &values});
    stdexec::start(operation);

    const auto start_time = std::chrono::steady_clock::now();
    auto canceller = io::sleep_for(context, 50ms) | ex::then([&] { stop_source.request_stop(); });
    auto result = ex::sync_wait(context, stop_source, canceller);
    REQUIRE(result);
    context.run_for(100ms);
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    CHECK(stopped == 1);
    CHECK(values == 0);
    CHECK(elapsed < 1s);
}

TEST_CASE("cancel: already-stopped token completes synchronously") {
    io_context context;
    ex::inplace_stop_source stop_source;
    stop_source.request_stop();

    int stopped = 0;
    int values = 0;
    auto operation = stdexec::connect(
        io::sleep_for(context, 1h),
        stop_counting_receiver{stop_source.get_token(), &stopped, &values});
    stdexec::start(operation); // must complete set_stopped without submitting

    CHECK(stopped == 1);
    CHECK(values == 0);
    CHECK(context.ring().enters() == 0);
}

TEST_CASE("cancel: when_all tree with external stop source") {
    io_context context;
    ex::inplace_stop_source stop_source;
    int stopped = 0;

    auto flow = ex::when_all(
        io::sleep_for(context, 1h) | ex::upon_stopped([&] { ++stopped; }),
        io::sleep_for(context, 50ms) | ex::then([&] { stop_source.request_stop(); }));

    const auto start_time = std::chrono::steady_clock::now();
    auto result = ex::sync_wait(context, stop_source, flow);
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    CHECK(result.stopped);
    CHECK_FALSE(result.error.has_value());
    CHECK(stopped == 1);
    CHECK(elapsed < 1s);
}

namespace {

struct foreign_stop_receiver {
    using receiver_concept = stdexec::receiver_tag;
    stdexec::inplace_stop_token token;
    io_context* context = nullptr;
    int* stopped = nullptr;

    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, token}};
    }
    void set_value() && noexcept {}
    void set_error(iox::error) && noexcept {}
    void set_error(std::exception_ptr) && noexcept {}
    void set_stopped() && noexcept {
        ++*stopped;
        context->stop(); // on the io thread: lets run() return right away
    }
};

void record_tid(io_context& context, std::uint64_t address) {
    *reinterpret_cast<std::thread::id*>(address) = std::this_thread::get_id();
    context.stop();
}

}

TEST_CASE("cancel: request_stop from a foreign thread reaches the io thread") {
    io_context context;
    ex::inplace_stop_source stop_source;
    int stopped = 0;

    auto operation = stdexec::connect(io::sleep_for(context, 1h),
                               foreign_stop_receiver{stop_source.get_token(), &context, &stopped});
    stdexec::start(operation);

    std::jthread kicker([&] {
        std::this_thread::sleep_for(50ms);
        stop_source.request_stop(); // the stop callback runs here, off the io thread
    });
    const auto start_time = std::chrono::steady_clock::now();
    context.run();
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    CHECK(stopped == 1);
    CHECK(elapsed < 5s); // used to hang: ASYNC_CANCEL was armed from the wrong thread
}

TEST_CASE("cancel: ctx.stop() from a foreign thread wakes a blocked run()") {
    io_context context;
    std::jthread kicker([&] {
        std::this_thread::sleep_for(50ms);
        context.stop();
    });
    const auto start_time = std::chrono::steady_clock::now();
    context.run(); // no work at all: used to sleep until a CQE happened to arrive
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    CHECK(elapsed < 5s);
}

TEST_CASE("cancel: ctx.post() runs the task on the io thread") {
    io_context context;
    std::thread::id ran_on;
    context.post(&record_tid, reinterpret_cast<std::uint64_t>(&ran_on)); // before run(): drained at entry
    context.run();
    CHECK(ran_on == std::this_thread::get_id());
}
