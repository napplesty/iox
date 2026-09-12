// iox — unified async IO for Linux
// tests/test_driver.cc — softdev::device is a counter device: every io::read on its handle
#include <doctest/doctest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <iox/core/exec.h>
#include <iox/driver/capabilities.h>
#include <iox/driver/completion_source.h>
#include <iox/driver/registry.h>
#include <iox/ops.h>

#include "support/softdev.h"

using namespace std::chrono_literals;
namespace ex = iox::exec;
using namespace iox;

TEST_CASE("driver: fd-mounted completion source — eventfd doorbell wakes the loop") {
    softdev::device dev(softdev::device::mode::fd_mounted);
    io_context ctx;
    ctx.attach_source(dev);
    REQUIRE(ctx.source_attached(dev));

    softdev::handle h{&dev};
    std::byte buf[8];

    std::thread prod([&] {
        std::this_thread::sleep_for(3ms);
        dev.produce();
    });

    auto r = ex::sync_wait(ctx, io::read(ctx, h, wbytes{buf, sizeof(buf)}));
    prod.join();

    REQUIRE(r);
    CHECK(std::get<0>(*r.value) == 8);
    std::uint64_t rec = 0;
    std::memcpy(&rec, buf, sizeof(rec));
    CHECK(rec == 0);

    ctx.detach_source(dev);
    CHECK_FALSE(ctx.source_attached(dev));
    ctx.run_for(20ms);
}

TEST_CASE("driver: busy-slot completion source — ~1ms tick polls has_work") {
    softdev::device dev(softdev::device::mode::busy_slot);
    io_context ctx;
    ctx.attach_source(dev);

    softdev::handle h{&dev};
    std::byte buf[8];
    const auto t0 = std::chrono::steady_clock::now();

    auto r = ex::sync_wait(ctx, io::read(ctx, h, wbytes{buf, sizeof(buf)}));

    const auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(r);
    CHECK(std::get<0>(*r.value) == 8);
    std::uint64_t rec = 0;
    std::memcpy(&rec, buf, sizeof(rec));
    CHECK(rec == 0);
    CHECK(elapsed >= 7ms);
    CHECK(elapsed < 2s);

    ctx.detach_source(dev);
    ctx.run_for(20ms);
}

TEST_CASE("driver: active injection — an io-thread continuation dispatches, no attachment") {
    softdev::device dev(softdev::device::mode::active);
    io_context ctx;
    CHECK_FALSE(ctx.source_attached(dev));

    softdev::handle h{&dev};
    std::byte buf[8];
    std::optional<std::size_t> got;

    auto flow = ex::when_all(
        io::sleep_for(ctx, 10ms) | ex::then([&] { dev.complete_now(ctx); }),
        io::read(ctx, h, wbytes{buf, sizeof(buf)}) | ex::then([&](std::size_t n) { got = n; }));

    auto r = ex::sync_wait(ctx, flow);
    REQUIRE(r);
    REQUIRE(got.has_value());
    CHECK(*got == 8);
    std::uint64_t rec = 0;
    std::memcpy(&rec, buf, sizeof(rec));
    CHECK(rec == 0);
}

TEST_CASE("driver: fd-mounted and busy sources run side by side") {
    softdev::device a(softdev::device::mode::fd_mounted);
    softdev::device b(softdev::device::mode::busy_slot);
    io_context ctx;
    ctx.attach_source(a);
    ctx.attach_source(b);
    REQUIRE(ctx.source_attached(a));
    REQUIRE(ctx.source_attached(b));

    softdev::handle first{&a};
    softdev::handle second{&b};
    std::byte first_buffer[8];
    std::byte second_buffer[8];
    std::optional<std::size_t> got_a;
    std::optional<std::size_t> got_b;

    std::thread prod([&] {
        std::this_thread::sleep_for(3ms);
        a.produce();
    });

    auto flow = ex::when_all(
        io::read(ctx, first, wbytes{first_buffer, sizeof(first_buffer)}) | ex::then([&](std::size_t n) { got_a = n; }),
        io::read(ctx, second, wbytes{second_buffer, sizeof(second_buffer)}) | ex::then([&](std::size_t n) { got_b = n; }));
    auto r = ex::sync_wait(ctx, flow);
    prod.join();

    REQUIRE(r);
    CHECK(got_a == 8);
    CHECK(got_b == 8);
    std::uint64_t first_result = 0;
    std::uint64_t second_result = 0;
    std::memcpy(&first_result, first_buffer, sizeof(first_result));
    std::memcpy(&second_result, second_buffer, sizeof(second_result));
    CHECK(first_result == 0);
    CHECK(second_result == 0);

    ctx.detach_source(a);
    ctx.detach_source(b);
    ctx.run_for(20ms);
}

TEST_CASE("driver: detaching from inside on_ready is legal") {
    softdev::device dev(softdev::device::mode::busy_slot, /*self_detach_after_drain=*/true);
    io_context ctx;
    ctx.attach_source(dev);

    softdev::handle h{&dev};
    std::byte buf[8];
    auto r = ex::sync_wait(ctx, io::read(ctx, h, wbytes{buf, sizeof(buf)}));

    REQUIRE(r);
    CHECK(std::get<0>(*r.value) == 8);
    CHECK_FALSE(ctx.source_attached(dev));

    ctx.run_for(20ms);
}

TEST_CASE("driver: default fd path is unchanged through the CPO seam") {
    io_context ctx;
    int pidfd[2] = {};
    REQUIRE(::pipe(pidfd) == 0);

    auto w = ex::sync_wait(ctx, io::write(ctx, iox::fd{pidfd[1]}, rbytes{
                                                                reinterpret_cast<const std::byte*>("hi"),
                                                                2}));
    REQUIRE(w);
    CHECK(std::get<0>(*w.value) == 2);

    std::byte buf[4];
    auto r = ex::sync_wait(ctx, io::read(ctx, iox::fd{pidfd[0]}, wbytes{buf, sizeof(buf)}));
    REQUIRE(r);
    CHECK(std::get<0>(*r.value) == 2);
    CHECK(std::memcmp(buf, "hi", 2) == 0);

    ::close(pidfd[0]);
    ::close(pidfd[1]);
}

TEST_CASE("driver: io::supports capability query with driver overrides") {
    const int event_fd = ::eventfd(0, EFD_CLOEXEC);
    REQUIRE(event_fd >= 0);
    const iox::fd raw{event_fd};

    CHECK(io::supports(io::zero_copy, raw));
    CHECK_FALSE(io::supports(io::mmap, raw));
    CHECK_FALSE(io::supports(io::dma, raw));

    const softdev::handle h{};
    CHECK(io::supports(io::zero_copy, h));
    CHECK(io::supports(io::mmap, h));
    CHECK_FALSE(io::supports(io::dma, h));

    ::close(event_fd);
}
