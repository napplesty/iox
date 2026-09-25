// iox — unified async IO for Linux
// tests/test_ops.cc — timers. Real kernel objects: pipes and eventfds. Also covers error
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

struct fd_guard {
    int raw = -1;
    explicit fd_guard(int raw) : raw(raw) {}
    ~fd_guard() {
        if (raw >= 0) {
            ::close(raw);
        }
    }
    fd_guard(const fd_guard&) = delete;
    fd_guard& operator=(const fd_guard&) = delete;
};

std::string_view trim_to(std::span<const std::byte> bytes, std::size_t length) {
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), length};
}

}

TEST_CASE("pipe: write then read round trip") {
    io_context context;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    fd_guard read_guard{fds[0]};
    fd_guard write_guard{fds[1]};

    const std::string_view message = "hello iox";
    auto write_result = ex::sync_wait(context, io::write(context, fd{write_guard.raw}, as_rbytes(std::span{message})));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == message.size());

    std::array<std::byte, 64> buffer{};
    auto read_result = ex::sync_wait(context, io::read(context, fd{read_guard.raw}, wbytes{buffer.data(), buffer.size()}));
    REQUIRE(read_result);
    const auto byte_count = std::get<0>(*read_result);
    CHECK(byte_count == message.size());
    CHECK(trim_to(buffer, byte_count) == message);
}

TEST_CASE("pipe: read returns 0 at EOF") {
    io_context context;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    fd_guard read_guard{fds[0]};
    {
        fd_guard write_guard{fds[1]};
    }

    std::array<std::byte, 8> buffer{};
    auto read_result = ex::sync_wait(context, io::read(context, fd{read_guard.raw}, wbytes{buffer.data(), buffer.size()}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == 0);
}

TEST_CASE("pipe: read on an invalid fd reports an error") {
    io_context context;
    std::array<std::byte, 8> buffer{};
    auto read_result = ex::sync_wait(context, io::read(context, fd{-1}, wbytes{buffer.data(), buffer.size()}));
    REQUIRE_FALSE(read_result);
    REQUIRE(read_result.error.has_value());
    CHECK(read_result.error->code() == EBADF);
}

TEST_CASE("poll: eventfd readiness") {
    io_context context;
    fd_guard efd{::eventfd(0, EFD_CLOEXEC)};
    REQUIRE(efd.raw >= 0);

  // Signal first, then poll: POLLIN must come back set.
    const std::uint64_t one = 1;
    REQUIRE(::write(efd.raw, &one, sizeof(one)) == sizeof(one));

    auto poll_result = ex::sync_wait(context, io::poll(context, fd{efd.raw}, POLLIN));
    REQUIRE(poll_result);
    const auto revents = std::get<0>(*poll_result);
    CHECK((revents & POLLIN) != 0);
}

TEST_CASE("poll: bad fd reports an error, not a hang") {
    io_context context;
    auto poll_result = ex::sync_wait(context, io::poll(context, fd{-1}, POLLIN));
    REQUIRE_FALSE(poll_result);
    REQUIRE(poll_result.error.has_value());
    CHECK(poll_result.error->code() == EBADF);
}

TEST_CASE("sleep_for fires approximately on time") {
    io_context context;
    const auto start_time = std::chrono::steady_clock::now();
    auto result = ex::sync_wait(context, io::sleep_for(context, 25ms));
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    REQUIRE(result);
    CHECK(elapsed >= 23ms);
    CHECK(elapsed < 2s);
}

TEST_CASE("sleep_until accepts an absolute deadline") {
    io_context context;
    const auto deadline = std::chrono::steady_clock::now() + 15ms;
    auto result = ex::sync_wait(context, io::sleep_until(context, deadline));
    REQUIRE(result);
    CHECK(std::chrono::steady_clock::now() >= deadline);
}

TEST_CASE("when_all runs a write and a read concurrently") {
    io_context context;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    fd_guard read_guard{fds[0]};
    fd_guard write_guard{fds[1]};

    const std::string_view message = "concurrent";
    std::array<std::byte, 64> buffer{};

    auto write_and_read = ex::when_all(
        io::write(context, fd{write_guard.raw}, as_rbytes(std::span{message})),
        io::read(context, fd{read_guard.raw}, wbytes{buffer.data(), buffer.size()}));

    auto result = ex::sync_wait(context, write_and_read);
    REQUIRE(result);
    CHECK(std::get<0>(*result) == message.size());
    CHECK(std::get<1>(*result) == message.size());
    CHECK(trim_to(buffer, std::get<1>(*result)) == message);
}

TEST_CASE("error flows through pipelines into sync_wait") {
    io_context context;
    std::array<std::byte, 8> buffer{};
    auto flow = io::read(context, fd{-1}, wbytes{buffer.data(), buffer.size()})
              | ex::then([](std::size_t) { return 1; });
    auto result = ex::sync_wait(context, flow);
    REQUIRE_FALSE(result);
    REQUIRE(result.error.has_value());
    CHECK(result.error->code() == EBADF);
}
