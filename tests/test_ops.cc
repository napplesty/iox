// Integration tests for the M1 vocabulary: fd-generic read/write, poll,
// timers. Real kernel objects: pipes and eventfds. Also covers error
// channels and sender composition (when_all).
#include <doctest/doctest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>

#include <iox/core/exec.h>
#include <iox/ops.h>

using namespace std::chrono_literals;
namespace ex = iox::exec;
using namespace iox;

namespace {

/// RAII guard for raw fds created by tests.
struct fd_guard {
    int v = -1;
    explicit fd_guard(int raw) : v(raw) {}
    ~fd_guard() {
        if (v >= 0) {
            ::close(v);
        }
    }
    fd_guard(const fd_guard&) = delete;
    fd_guard& operator=(const fd_guard&) = delete;
};

std::string_view trim_to(std::span<const std::byte> bytes, std::size_t n) {
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), n};
}

} // namespace

TEST_CASE("pipe: write then read round trip") {
    io_context ctx;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    fd_guard r{fds[0]};
    fd_guard w{fds[1]};

    const std::string_view msg = "hello iox";
    auto wr = ex::sync_wait(ctx, io::write(ctx, fd{w.v}, as_rbytes(std::span{msg})));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == msg.size());

    std::array<std::byte, 64> buf{};
    auto rd = ex::sync_wait(ctx, io::read(ctx, fd{r.v}, wbytes{buf.data(), buf.size()}));
    REQUIRE(rd);
    const auto n = std::get<0>(*rd);
    CHECK(n == msg.size());
    CHECK(trim_to(buf, n) == msg);
}

TEST_CASE("pipe: read returns 0 at EOF") {
    io_context ctx;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    fd_guard r{fds[0]};
    {
        fd_guard w{fds[1]}; // write end closes at scope exit, arming EOF
    }

    std::array<std::byte, 8> buf{};
    auto rd = ex::sync_wait(ctx, io::read(ctx, fd{r.v}, wbytes{buf.data(), buf.size()}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == 0); // EOF
}

TEST_CASE("pipe: read on an invalid fd reports an error") {
    io_context ctx;
    std::array<std::byte, 8> buf{};
    auto rd = ex::sync_wait(ctx, io::read(ctx, fd{-1}, wbytes{buf.data(), buf.size()}));
    REQUIRE_FALSE(rd);
    REQUIRE(rd.error.has_value());
    CHECK(rd.error->code() == EBADF);
}

TEST_CASE("poll: eventfd readiness") {
    io_context ctx;
    fd_guard efd{::eventfd(0, EFD_CLOEXEC)};
    REQUIRE(efd.v >= 0);

    // Signal first, then poll: POLLIN must come back set.
    const std::uint64_t one = 1;
    REQUIRE(::write(efd.v, &one, sizeof(one)) == sizeof(one));

    auto p = ex::sync_wait(ctx, io::poll(ctx, fd{efd.v}, POLLIN));
    REQUIRE(p);
    const auto revents = std::get<0>(*p);
    CHECK((revents & POLLIN) != 0);
}

TEST_CASE("poll: bad fd reports an error, not a hang") {
    io_context ctx;
    auto p = ex::sync_wait(ctx, io::poll(ctx, fd{-1}, POLLIN));
    REQUIRE_FALSE(p);
    REQUIRE(p.error.has_value());
    CHECK(p.error->code() == EBADF);
}

TEST_CASE("sleep_for fires approximately on time") {
    io_context ctx;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = ex::sync_wait(ctx, io::sleep_for(ctx, 25ms));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(r);
    CHECK(elapsed >= 23ms);
    CHECK(elapsed < 2s);
}

TEST_CASE("sleep_until accepts an absolute deadline") {
    io_context ctx;
    const auto deadline = std::chrono::steady_clock::now() + 15ms;
    auto r = ex::sync_wait(ctx, io::sleep_until(ctx, deadline));
    REQUIRE(r);
    CHECK(std::chrono::steady_clock::now() >= deadline);
}

TEST_CASE("when_all runs a write and a read concurrently") {
    io_context ctx;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    fd_guard r{fds[0]};
    fd_guard w{fds[1]};

    const std::string_view msg = "concurrent";
    std::array<std::byte, 64> buf{};

    // Both operations are armed before either is guaranteed to run: the
    // read waits for the write's data through the ring, no user threading.
    auto both = ex::when_all(
        io::write(ctx, fd{w.v}, as_rbytes(std::span{msg})),
        io::read(ctx, fd{r.v}, wbytes{buf.data(), buf.size()}));

    auto res = ex::sync_wait(ctx, both);
    REQUIRE(res);
    // when_all completes with the values of both children.
    CHECK(std::get<0>(*res) == msg.size());
    CHECK(std::get<1>(*res) == msg.size());
    CHECK(trim_to(buf, std::get<1>(*res)) == msg);
}

TEST_CASE("error flows through pipelines into sync_wait") {
    io_context ctx;
    std::array<std::byte, 8> buf{};
    auto flow = io::read(ctx, fd{-1}, wbytes{buf.data(), buf.size()})
              | ex::then([](std::size_t) { return 1; });
    auto r = ex::sync_wait(ctx, flow);
    REQUIRE_FALSE(r);
    REQUIRE(r.error.has_value());
    CHECK(r.error->code() == EBADF);
}
