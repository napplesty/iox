// iox — unified async IO for Linux
// tests/test_signal.cc — signal::watcher (signalfd), io::signal (siginfo completion, cancellation).
#include <doctest/doctest.h>

#include <signal.h>
#include <unistd.h>

#include <chrono>

#include <iox/core/exec.h>
#include <iox/ops.h>
#include <iox/signal/set.h>
#include <iox/signal/watcher.h>

using namespace std::chrono_literals;
using namespace iox;
namespace ex = iox::exec;

TEST_CASE("signal: raise → siginfo completion, signal is consumed") {
    io_context ctx;
    signal::set set{SIGUSR1, SIGUSR2};
    auto w = signal::watcher::create(set);
    REQUIRE(w);

    ::raise(SIGUSR1);
    auto r = ex::sync_wait(ctx, io::signal(ctx, *w));
    REQUIRE(r);
    CHECK(std::get<0>(*r).ssi_signo == SIGUSR1);

    ::raise(SIGUSR2);
    auto r2 = ex::sync_wait(ctx, io::signal(ctx, *w));
    REQUIRE(r2);
    CHECK(std::get<0>(*r2).ssi_signo == SIGUSR2);

}

TEST_CASE("signal: io::read also works (raw bytes of a siginfo record)") {
    io_context ctx;
    signal::set set{SIGUSR1};
    auto w = signal::watcher::create(set);
    REQUIRE(w);

    ::raise(SIGUSR1);
    std::byte raw[sizeof(::signalfd_siginfo)]{};
    auto r = ex::sync_wait(ctx, io::read(ctx, *w, wbytes{raw, sizeof(raw)}));
    REQUIRE(r);
    CHECK(std::get<0>(*r) == sizeof(::signalfd_siginfo));
}

TEST_CASE("signal: stop token cancels a pending wait") {
    io_context ctx;
    signal::set set{SIGUSR1};
    auto w = signal::watcher::create(set);
    REQUIRE(w);

    ex::inplace_stop_source src;
    int stopped = 0;
    struct counting_receiver {
        using receiver_concept = stdexec::receiver_tag;
        ex::inplace_stop_token tok;
        int* stopped;
        auto get_env() const noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}};
        }
        void set_value(::signalfd_siginfo) && noexcept {}
        void set_error(iox::error) && noexcept {}
        void set_error(std::exception_ptr) && noexcept {}
        void set_stopped() && noexcept { ++*stopped; }
    };
    auto op = stdexec::connect(io::signal(ctx, *w),
                               counting_receiver{src.get_token(), &stopped});
    stdexec::start(op);

  // 30ms later, request cancellation from a completion on the io thread.
    auto canceller = io::sleep_for(ctx, 30ms) | ex::then([&] { src.request_stop(); });
    REQUIRE(ex::sync_wait(ctx, src, canceller));
    ctx.run_for(100ms);

    CHECK(stopped == 1);
    ::raise(SIGUSR1);
    auto drain = ex::sync_wait(ctx, io::signal(ctx, *w));
    REQUIRE(drain);
}

TEST_CASE("signal: watcher is a readable handle, nothing more") {
    static_assert(io::readable<iox::signal::watcher>);
    static_assert(!io::writable<iox::signal::watcher>);
    static_assert(!io::seekable<iox::signal::watcher>);
}
