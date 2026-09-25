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
    io_context context;
    context.attach_source(dev);
    REQUIRE(context.source_attached(dev));

    softdev::handle handle{&dev};
    std::byte buffer[8];

    std::thread producer([&] {
        std::this_thread::sleep_for(3ms);
        dev.produce();
    });

    auto read_result = ex::sync_wait(context, io::read(context, handle, wbytes{buffer, sizeof(buffer)}));
    producer.join();

    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result.value) == 8);
    std::uint64_t record = 0;
    std::memcpy(&record, buffer, sizeof(record));
    CHECK(record == 0);

    context.detach_source(dev);
    CHECK_FALSE(context.source_attached(dev));
    context.run_for(20ms);
}

TEST_CASE("driver: busy-slot completion source — ~1ms tick polls has_work") {
    softdev::device dev(softdev::device::mode::busy_slot);
    io_context context;
    context.attach_source(dev);

    softdev::handle handle{&dev};
    std::byte buffer[8];
    const auto start_time = std::chrono::steady_clock::now();

    auto read_result = ex::sync_wait(context, io::read(context, handle, wbytes{buffer, sizeof(buffer)}));

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result.value) == 8);
    std::uint64_t record = 0;
    std::memcpy(&record, buffer, sizeof(record));
    CHECK(record == 0);
    CHECK(elapsed >= 7ms);
    CHECK(elapsed < 2s);

    context.detach_source(dev);
    context.run_for(20ms);
}

TEST_CASE("driver: active injection — an io-thread continuation dispatches, no attachment") {
    softdev::device dev(softdev::device::mode::active);
    io_context context;
    CHECK_FALSE(context.source_attached(dev));

    softdev::handle handle{&dev};
    std::byte buffer[8];
    std::optional<std::size_t> received;

    auto flow = ex::when_all(
        io::sleep_for(context, 10ms) | ex::then([&] { dev.complete_now(context); }),
        io::read(context, handle, wbytes{buffer, sizeof(buffer)}) | ex::then([&](std::size_t byte_count) { received = byte_count; }));

    auto result = ex::sync_wait(context, flow);
    REQUIRE(result);
    REQUIRE(received.has_value());
    CHECK(*received == 8);
    std::uint64_t record = 0;
    std::memcpy(&record, buffer, sizeof(record));
    CHECK(record == 0);
}

TEST_CASE("driver: fd-mounted and busy sources run side by side") {
    softdev::device fd_device(softdev::device::mode::fd_mounted);
    softdev::device busy_device(softdev::device::mode::busy_slot);
    io_context context;
    context.attach_source(fd_device);
    context.attach_source(busy_device);
    REQUIRE(context.source_attached(fd_device));
    REQUIRE(context.source_attached(busy_device));

    softdev::handle first{&fd_device};
    softdev::handle second{&busy_device};
    std::byte first_buffer[8];
    std::byte second_buffer[8];
    std::optional<std::size_t> received_a;
    std::optional<std::size_t> received_b;

    std::thread producer([&] {
        std::this_thread::sleep_for(3ms);
        fd_device.produce();
    });

    auto flow = ex::when_all(
        io::read(context, first, wbytes{first_buffer, sizeof(first_buffer)}) | ex::then([&](std::size_t byte_count) { received_a = byte_count; }),
        io::read(context, second, wbytes{second_buffer, sizeof(second_buffer)}) | ex::then([&](std::size_t byte_count) { received_b = byte_count; }));
    auto result = ex::sync_wait(context, flow);
    producer.join();

    REQUIRE(result);
    CHECK(received_a == 8);
    CHECK(received_b == 8);
    std::uint64_t first_result = 0;
    std::uint64_t second_result = 0;
    std::memcpy(&first_result, first_buffer, sizeof(first_result));
    std::memcpy(&second_result, second_buffer, sizeof(second_result));
    CHECK(first_result == 0);
    CHECK(second_result == 0);

    context.detach_source(fd_device);
    context.detach_source(busy_device);
    context.run_for(20ms);
}

TEST_CASE("driver: detaching from inside on_ready is legal") {
    softdev::device dev(softdev::device::mode::busy_slot, /*self_detach_after_drain=*/true);
    io_context context;
    context.attach_source(dev);

    softdev::handle handle{&dev};
    std::byte buffer[8];
    auto read_result = ex::sync_wait(context, io::read(context, handle, wbytes{buffer, sizeof(buffer)}));

    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result.value) == 8);
    CHECK_FALSE(context.source_attached(dev));

    context.run_for(20ms);
}

TEST_CASE("driver: default fd path is unchanged through the CPO seam") {
    io_context context;
    int pipe_fds[2] = {};
    REQUIRE(::pipe(pipe_fds) == 0);

    auto write_result = ex::sync_wait(context, io::write(context, iox::fd{pipe_fds[1]}, rbytes{
                                                                reinterpret_cast<const std::byte*>("hi"),
                                                                2}));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result.value) == 2);

    std::byte buffer[4];
    auto read_result = ex::sync_wait(context, io::read(context, iox::fd{pipe_fds[0]}, wbytes{buffer, sizeof(buffer)}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result.value) == 2);
    CHECK(std::memcmp(buffer, "hi", 2) == 0);

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
}

TEST_CASE("driver: io::supports capability query with driver overrides") {
    const int event_fd = ::eventfd(0, EFD_CLOEXEC);
    REQUIRE(event_fd >= 0);
    const iox::fd raw{event_fd};

    CHECK(io::supports(io::zero_copy, raw));
    CHECK_FALSE(io::supports(io::mmap, raw));
    CHECK_FALSE(io::supports(io::dma, raw));

    const softdev::handle handle{};
    CHECK(io::supports(io::zero_copy, handle));
    CHECK(io::supports(io::mmap, handle));
    CHECK_FALSE(io::supports(io::dma, handle));

    ::close(event_fd);
}
