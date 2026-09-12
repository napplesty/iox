// iox — unified async IO for Linux
// tests/test_fault.cc — Two flavors:
#include <doctest/doctest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <string>

#include <iox/compose/detach.h>
#include <iox/core/exec.h>
#include <iox/fs/file.h>
#include <iox/net/tcp.h>
#include <iox/ops.h>
#include <iox/pipe/channel.h>

using namespace std::chrono_literals;
namespace ex = iox::exec;
using namespace iox;

namespace {

const std::byte payload[4]{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

struct tcp_pair {
    net::tcp::acceptor acc;
    net::tcp::socket client;
    net::tcp::socket server;

    static tcp_pair create(io_context& ctx) {
        auto acc = net::tcp::acceptor::listen(*net::endpoint::ipv4_any(0));
        REQUIRE(acc);
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        REQUIRE(::getsockname(acc->accept_handle().v, reinterpret_cast<sockaddr*>(&ss), &len) == 0);
        const auto port = ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);

        auto client = net::tcp::socket::unconnected(net::endpoint::family::ipv4);
        REQUIRE(client);
        auto done = ex::sync_wait(ctx, ex::when_all(
                                           io::accept(ctx, *acc),
                                           io::connect(ctx, *client,
                                                       *net::endpoint::ipv4("127.0.0.1", port))));
        REQUIRE(done);
        return tcp_pair{std::move(*acc), std::move(*client),
                        std::get<0>(std::move(*done))};
    }
};

}

TEST_CASE("fault: the Nth submission fails with the injected errno") {
    io_context ctx;
    auto p = pipe::pair::create();
    REQUIRE(p);

    auto ok1 = ex::sync_wait(ctx, io::write(ctx, p->w, rbytes{payload, sizeof(payload)}));
    REQUIRE(ok1);

    ctx.arm_failpoint(/*nth=*/1, -ENOMEM);
    auto hit = ex::sync_wait(ctx, io::write(ctx, p->w, rbytes{payload, sizeof(payload)}));
    REQUIRE_FALSE(hit);
    REQUIRE(hit.error);
    CHECK(hit.error->code() == ENOMEM);

    ctx.clear_failpoint();
    auto ok2 = ex::sync_wait(ctx, io::write(ctx, p->w, rbytes{payload, sizeof(payload)}));
    REQUIRE(ok2);

    std::array<std::byte, 8> buf{};
    auto rd = ex::sync_wait(ctx, io::read(ctx, p->r, wbytes{buf.data(), buf.size()}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == 8);
}

TEST_CASE("fault: injected EMFILE on io::open is a typed error") {
    io_context ctx;
    const std::string path = "/tmp/iox_test_fault_open_" + std::to_string(::getpid());
    ::unlink(path.c_str());

    ctx.arm_failpoint(1, -EMFILE);
    auto denied = ex::sync_wait(ctx, io::open(ctx, path.c_str(),
                                              fs::mode::rw | fs::mode::create));
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error);
    CHECK(denied.error->code() == EMFILE);
    ctx.clear_failpoint();

    auto made = ex::sync_wait(ctx, io::open(ctx, path.c_str(),
                                            fs::mode::rw | fs::mode::create | fs::mode::truncate));
    REQUIRE(made);
    CHECK(std::get<0>(*made));

    auto wr = ex::sync_wait(ctx, io::write(ctx, std::get<0>(*made), rbytes{payload, 4}));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == 4);
    ::unlink(path.c_str());
}

TEST_CASE("fault: a detached op under an injected failure frees itself") {
    io_context ctx;
    auto p = pipe::pair::create();
    REQUIRE(p);

    ctx.arm_failpoint(1, -ENOMEM);
    int reaped = 0;
    ex::detach(io::write(ctx, p->w, rbytes{payload, 4}) | ex::upon_error([&](iox::error) {
        ++reaped;
    }));
    ctx.run_for(50ms);
    CHECK(reaped == 1);
    ctx.clear_failpoint();
}

TEST_CASE("fault: io::open surfaces real kernel errors (ENOENT)") {
    io_context ctx;
    auto r = ex::sync_wait(ctx, io::open(ctx, "/nonexistent/iox/definitely/absent",
                                         fs::mode::read));
    REQUIRE_FALSE(r);
    REQUIRE(r.error);
    CHECK(r.error->code() == ENOENT);
}

TEST_CASE("fault: mid-operation disconnect — blocked read ends in EOF or reset") {
    io_context ctx;
    auto pair = tcp_pair::create(ctx);

    std::array<std::byte, 16> buf{};
    auto rd = ex::sync_wait(ctx, ex::when_all(
                                     io::read(ctx, pair.client, wbytes{buf.data(), buf.size()})
                                         | ex::then([](std::size_t n) { return n == 0; }),
                                     io::sleep_for(ctx, 30ms) | ex::then([&] {
                                         pair.server.reset();
                                         return true;
                                     })));
    REQUIRE(rd);
    const bool eof = std::get<0>(*rd);
    CHECK(eof); // loopback close delivers EOF promptly (no RST raced here)

    auto wr = ex::sync_wait(ctx, io::write(ctx, pair.client, rbytes{payload, 4}));
    if (wr) {
        ctx.run_for(50ms);
        wr = ex::sync_wait(ctx, io::write(ctx, pair.client, rbytes{payload, 4}));
    }
    REQUIRE_FALSE(wr);
    REQUIRE(wr.error);
    CHECK(wr.error->code() == EPIPE);
}

TEST_CASE("fault: cancel race — stop fires while a pipe read is blocked") {
    io_context ctx;
    auto p = pipe::pair::create();
    REQUIRE(p);

    ex::inplace_stop_source src;
    std::array<std::byte, 8> buf{};
    ex::detach(io::sleep_for(ctx, 30ms) | ex::then([&] { src.request_stop(); }));

    const auto t0 = std::chrono::steady_clock::now();
    auto r = ex::sync_wait(ctx, src, io::read(ctx, p->r, wbytes{buf.data(), buf.size()}));
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE_FALSE(r);
    REQUIRE_FALSE(r.error.has_value()); // set_stopped, never an error
    CHECK(r.stopped);
    CHECK(elapsed < 1s);
}
