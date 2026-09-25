// iox — unified async IO for Linux
// tests/test_process.cc — pidfd-based wait, race-free kill, typed exec errors.
#include <doctest/doctest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

#include <iox/compose/write_all.h>
#include <iox/core/buffer.h>
#include <iox/core/exec.h>
#include <iox/ops.h>
#include <iox/process/process.h>

using namespace std::chrono_literals;
using namespace iox;
namespace ex = iox::exec;

TEST_CASE("process: spawn cat, pipe in/out round trip, clean exit") {
    io_context context;
    auto to_child = pipe::pair::create();
    auto from_child = pipe::pair::create();
    REQUIRE(to_child);
    REQUIRE(from_child);

    auto child = process::process::spawn(
        {"/bin/cat"},
        {.in = std::move(to_child->r), .out = std::move(from_child->w)});
    REQUIRE(child);
    CHECK(child->pid() > 0);

    const std::string_view message = "hello through iox pipes\n";
    auto write_result = ex::sync_wait(context, io::write_all(context, to_child->w, as_rbytes(std::span{message})));
    REQUIRE(write_result);
    to_child->w.reset();

    std::array<std::byte, 128> buffer{};
    std::string received;
    for (;;) {
        auto read_result = ex::sync_wait(context, io::read(context, from_child->r, wbytes{buffer.data(), buffer.size()}));
        REQUIRE(read_result);
        const std::size_t byte_count = std::get<0>(*read_result);
        if (byte_count == 0) {
            break;
        }
        received.append(reinterpret_cast<const char*>(buffer.data()), byte_count);
    }
    CHECK(received == message);

    auto status = ex::sync_wait(context, io::wait_pid(context, *child));
    REQUIRE(status);
    CHECK(std::get<0>(*status).exited);
    CHECK(std::get<0>(*status).success());
}

TEST_CASE("process: wait_pid completes asynchronously when the child dies") {
    io_context context;
    auto child = process::process::spawn({"/bin/sleep", "0.1"});
    REQUIRE(child);

    const auto start_time = std::chrono::steady_clock::now();
    auto status = ex::sync_wait(context, io::wait_pid(context, *child));
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    REQUIRE(status);
    CHECK(std::get<0>(*status).success());
    CHECK(elapsed >= 90ms);
}

TEST_CASE("process: kill is race-free and wait reports the signal") {
    io_context context;
    auto child = process::process::spawn({"/bin/sleep", "60"});
    REQUIRE(child);
    REQUIRE(child->kill(SIGKILL));

    auto status = ex::sync_wait(context, io::wait_pid(context, *child));
    REQUIRE(status);
    CHECK(std::get<0>(*status).signaled);
    CHECK(std::get<0>(*status).code == SIGKILL);
    CHECK_FALSE(std::get<0>(*status).exited);
}

TEST_CASE("process: exec failure is a typed error, no child leaks") {
    auto child = process::process::spawn({"/definitely/not/here/iox"});
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error().code() == ENOENT);
    CHECK(child.error().name() == std::string_view{"ENOENT"});
}

TEST_CASE("process: spawn without wiring inherits stdio (no pipes closed)") {
    io_context context;
    auto child = process::process::spawn({"/usr/bin/true"});
    REQUIRE(child);
    auto status = ex::sync_wait(context, io::wait_pid(context, *child));
    REQUIRE(status);
    CHECK(std::get<0>(*status).success());
}
