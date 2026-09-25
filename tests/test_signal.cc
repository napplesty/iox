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
    io_context context;
    signal::set set{SIGUSR1, SIGUSR2};
    auto watcher = signal::watcher::create(set);
    REQUIRE(watcher);

    ::raise(SIGUSR1);
    auto result = ex::sync_wait(context, io::signal(context, *watcher));
    REQUIRE(result);
    CHECK(std::get<0>(*result).ssi_signo == SIGUSR1);

    ::raise(SIGUSR2);
    auto second_result = ex::sync_wait(context, io::signal(context, *watcher));
    REQUIRE(second_result);
    CHECK(std::get<0>(*second_result).ssi_signo == SIGUSR2);

}

TEST_CASE("signal: io::read also works (raw bytes of a siginfo record)") {
    io_context context;
    signal::set set{SIGUSR1};
    auto watcher = signal::watcher::create(set);
    REQUIRE(watcher);

    ::raise(SIGUSR1);
    std::byte raw[sizeof(::signalfd_siginfo)]{};
    auto result = ex::sync_wait(context, io::read(context, *watcher, wbytes{raw, sizeof(raw)}));
    REQUIRE(result);
    CHECK(std::get<0>(*result) == sizeof(::signalfd_siginfo));
}

TEST_CASE("signal: stop token cancels a pending wait") {
    io_context context;
    signal::set set{SIGUSR1};
    auto watcher = signal::watcher::create(set);
    REQUIRE(watcher);

    ex::inplace_stop_source stop_source;
    int stopped = 0;
    struct counting_receiver {
        using receiver_concept = stdexec::receiver_tag;
        ex::inplace_stop_token token;
        int* stopped;
        auto get_env() const noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, token}};
        }
        void set_value(::signalfd_siginfo) && noexcept {}
        void set_error(iox::error) && noexcept {}
        void set_error(std::exception_ptr) && noexcept {}
        void set_stopped() && noexcept { ++*stopped; }
    };
    auto operation = stdexec::connect(io::signal(context, *watcher),
                               counting_receiver{stop_source.get_token(), &stopped});
    stdexec::start(operation);

  // 30ms later, request cancellation from a completion on the io thread.
    auto canceller = io::sleep_for(context, 30ms) | ex::then([&] { stop_source.request_stop(); });
    REQUIRE(ex::sync_wait(context, stop_source, canceller));
    context.run_for(100ms);

    CHECK(stopped == 1);
    ::raise(SIGUSR1);
    auto drain = ex::sync_wait(context, io::signal(context, *watcher));
    REQUIRE(drain);
}

TEST_CASE("signal: watcher is a readable handle, nothing more") {
    static_assert(io::readable<iox::signal::watcher>);
    static_assert(!io::writable<iox::signal::watcher>);
    static_assert(!io::seekable<iox::signal::watcher>);
}
