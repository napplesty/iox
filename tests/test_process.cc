// Integration tests for the process module: spawn with pipe wiring,
// pidfd-based wait, race-free kill, typed exec errors.
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
    io_context ctx;
    auto to_child = pipe::pair::create(); // parent writes → child stdin
    auto from_child = pipe::pair::create(); // child stdout → parent reads
    REQUIRE(to_child);
    REQUIRE(from_child);

    auto p = process::process::spawn(
        {"/bin/cat"},
        {.in = std::move(to_child->r), .out = std::move(from_child->w)});
    REQUIRE(p);
    CHECK(p->pid() > 0);

    const std::string_view msg = "hello through iox pipes\n";
    auto wr = ex::sync_wait(ctx, io::write_all(ctx, to_child->w, as_rbytes(std::span{msg})));
    REQUIRE(wr);
    to_child->w.reset(); // cat exits on stdin EOF

    std::array<std::byte, 128> buf{};
    std::string got;
    for (;;) {
        auto rd = ex::sync_wait(ctx, io::read(ctx, from_child->r, wbytes{buf.data(), buf.size()}));
        REQUIRE(rd);
        const std::size_t n = std::get<0>(*rd);
        if (n == 0) {
            break;
        }
        got.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    CHECK(got == msg);

    auto st = ex::sync_wait(ctx, io::wait_pid(ctx, *p));
    REQUIRE(st);
    CHECK(std::get<0>(*st).exited);
    CHECK(std::get<0>(*st).success());
}

TEST_CASE("process: wait_pid completes asynchronously when the child dies") {
    io_context ctx;
    auto p = process::process::spawn({"/bin/sleep", "0.1"});
    REQUIRE(p);

    const auto t0 = std::chrono::steady_clock::now();
    auto st = ex::sync_wait(ctx, io::wait_pid(ctx, *p)); // armed before exit
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(st);
    CHECK(std::get<0>(*st).success());
    CHECK(elapsed >= 90ms); // it really waited for the child
}

TEST_CASE("process: kill is race-free and wait reports the signal") {
    io_context ctx;
    auto p = process::process::spawn({"/bin/sleep", "60"});
    REQUIRE(p);
    REQUIRE(p->kill(SIGKILL));

    auto st = ex::sync_wait(ctx, io::wait_pid(ctx, *p));
    REQUIRE(st);
    CHECK(std::get<0>(*st).signaled);
    CHECK(std::get<0>(*st).code == SIGKILL);
    CHECK_FALSE(std::get<0>(*st).exited);
}

TEST_CASE("process: exec failure is a typed error, no child leaks") {
    auto p = process::process::spawn({"/definitely/not/here/iox"});
    REQUIRE_FALSE(p.has_value());
    CHECK(p.error().code() == ENOENT);
    CHECK(p.error().name() == std::string_view{"ENOENT"});
}

TEST_CASE("process: spawn without wiring inherits stdio (no pipes closed)") {
    io_context ctx;
    // true(1) needs no io and exits 0 immediately.
    auto p = process::process::spawn({"/usr/bin/true"});
    REQUIRE(p);
    auto st = ex::sync_wait(ctx, io::wait_pid(ctx, *p));
    REQUIRE(st);
    CHECK(std::get<0>(*st).success());
}
