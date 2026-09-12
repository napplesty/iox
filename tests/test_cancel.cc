// iox — unified async IO for Linux
// tests/test_cancel.cc — All single-threaded: request_stop happens in a completion on the io thread.
#include <doctest/doctest.h>

#include <chrono>

#include <iox/core/exec.h>
#include <iox/ops.h>

using namespace std::chrono_literals;
using namespace iox;
namespace ex = iox::exec;

namespace {

struct stop_counting_receiver {
    using receiver_concept = stdexec::receiver_tag;
    stdexec::inplace_stop_token tok;
    int* stopped = nullptr;
    int* values = nullptr;

    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}};
    }
    void set_value() && noexcept { ++*values; }
    void set_error(iox::error) && noexcept { ++*values; }
    void set_error(std::exception_ptr) && noexcept { ++*values; }
    void set_stopped() && noexcept { ++*stopped; }
};

}

TEST_CASE("cancel: in-flight sleep is canceled via stop token") {
    io_context ctx;
    ex::inplace_stop_source src;
    int stopped = 0;
    int values = 0;

    auto op = stdexec::connect(
        io::sleep_for(ctx, 10s),
        stop_counting_receiver{src.get_token(), &stopped, &values});
    stdexec::start(op);

    const auto t0 = std::chrono::steady_clock::now();
    auto canceller = io::sleep_for(ctx, 50ms) | ex::then([&] { src.request_stop(); });
    auto r = ex::sync_wait(ctx, src, canceller);
    REQUIRE(r);
    ctx.run_for(100ms);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(stopped == 1);
    CHECK(values == 0);
    CHECK(elapsed < 1s);
}

TEST_CASE("cancel: already-stopped token completes synchronously") {
    io_context ctx;
    ex::inplace_stop_source src;
    src.request_stop();

    int stopped = 0;
    int values = 0;
    auto op = stdexec::connect(
        io::sleep_for(ctx, 1h),
        stop_counting_receiver{src.get_token(), &stopped, &values});
    stdexec::start(op); // must complete set_stopped without submitting

    CHECK(stopped == 1);
    CHECK(values == 0);
    CHECK(ctx.ring().enters() == 0);
}

TEST_CASE("cancel: when_all tree with external stop source") {
    io_context ctx;
    ex::inplace_stop_source src;
    int stopped = 0;

    auto flow = ex::when_all(
        io::sleep_for(ctx, 1h) | ex::upon_stopped([&] { ++stopped; }),
        io::sleep_for(ctx, 50ms) | ex::then([&] { src.request_stop(); }));

    const auto t0 = std::chrono::steady_clock::now();
    auto r = ex::sync_wait(ctx, src, flow);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(r.stopped);
    CHECK_FALSE(r.error.has_value());
    CHECK(stopped == 1);
    CHECK(elapsed < 1s);
}
