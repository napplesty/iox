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
    net::tcp::acceptor acceptor;
    net::tcp::socket client;
    net::tcp::socket server;

    static tcp_pair create(io_context& context) {
        auto acceptor = net::tcp::acceptor::listen(*net::endpoint::ipv4_any(0));
        REQUIRE(acceptor);
        sockaddr_storage storage{};
        socklen_t address_length = sizeof(storage);
        REQUIRE(::getsockname(acceptor->accept_handle().v, reinterpret_cast<sockaddr*>(&storage), &address_length) == 0);
        const auto port = ntohs(reinterpret_cast<sockaddr_in*>(&storage)->sin_port);

        auto client = net::tcp::socket::unconnected(net::endpoint::family::ipv4);
        REQUIRE(client);
        auto connected = ex::sync_wait(context, ex::when_all(
                                           io::accept(context, *acceptor),
                                           io::connect(context, *client,
                                                       *net::endpoint::ipv4("127.0.0.1", port))));
        REQUIRE(connected);
        return tcp_pair{std::move(*acceptor), std::move(*client),
                        std::get<0>(std::move(*connected))};
    }
};

}

TEST_CASE("fault: the Nth submission fails with the injected errno") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);

    auto first_write = ex::sync_wait(context, io::write(context, pair->w, rbytes{payload, sizeof(payload)}));
    REQUIRE(first_write);

    context.arm_failpoint(/*nth=*/1, -ENOMEM);
    auto hit = ex::sync_wait(context, io::write(context, pair->w, rbytes{payload, sizeof(payload)}));
    REQUIRE_FALSE(hit);
    REQUIRE(hit.error);
    CHECK(hit.error->code() == ENOMEM);

    context.clear_failpoint();
    auto second_write = ex::sync_wait(context, io::write(context, pair->w, rbytes{payload, sizeof(payload)}));
    REQUIRE(second_write);

    std::array<std::byte, 8> buffer{};
    auto read_result = ex::sync_wait(context, io::read(context, pair->r, wbytes{buffer.data(), buffer.size()}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == 8);
}

TEST_CASE("fault: injected EMFILE on io::open is a typed error") {
    io_context context;
    const std::string path = "/tmp/iox_test_fault_open_" + std::to_string(::getpid());
    ::unlink(path.c_str());

    context.arm_failpoint(1, -EMFILE);
    auto denied = ex::sync_wait(context, io::open(context, path.c_str(),
                                              fs::mode::rw | fs::mode::create));
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error);
    CHECK(denied.error->code() == EMFILE);
    context.clear_failpoint();

    auto made = ex::sync_wait(context, io::open(context, path.c_str(),
                                            fs::mode::rw | fs::mode::create | fs::mode::truncate));
    REQUIRE(made);
    CHECK(std::get<0>(*made));

    auto write_result = ex::sync_wait(context, io::write(context, std::get<0>(*made), rbytes{payload, 4}));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == 4);
    ::unlink(path.c_str());
}

TEST_CASE("fault: a detached op under an injected failure frees itself") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);

    context.arm_failpoint(1, -ENOMEM);
    int reaped = 0;
    ex::detach(io::write(context, pair->w, rbytes{payload, 4}) | ex::upon_error([&](iox::error) {
        ++reaped;
    }));
    context.run_for(50ms);
    CHECK(reaped == 1);
    context.clear_failpoint();
}

TEST_CASE("fault: io::open surfaces real kernel errors (ENOENT)") {
    io_context context;
    auto open_result = ex::sync_wait(context, io::open(context, "/nonexistent/iox/definitely/absent",
                                         fs::mode::read));
    REQUIRE_FALSE(open_result);
    REQUIRE(open_result.error);
    CHECK(open_result.error->code() == ENOENT);
}

TEST_CASE("fault: mid-operation disconnect — blocked read ends in EOF or reset") {
    io_context context;
    auto pair = tcp_pair::create(context);

    std::array<std::byte, 16> buffer{};
    auto result = ex::sync_wait(context, ex::when_all(
                                     io::read(context, pair.client, wbytes{buffer.data(), buffer.size()})
                                         | ex::then([](std::size_t byte_count) { return byte_count == 0; }),
                                     io::sleep_for(context, 30ms) | ex::then([&] {
                                         pair.server.reset();
                                         return true;
                                     })));
    REQUIRE(result);
    const bool eof = std::get<0>(*result);
    CHECK(eof); // loopback close delivers EOF promptly (no RST raced here)

    auto write_result = ex::sync_wait(context, io::write(context, pair.client, rbytes{payload, 4}));
    if (write_result) {
        context.run_for(50ms);
        write_result = ex::sync_wait(context, io::write(context, pair.client, rbytes{payload, 4}));
    }
    REQUIRE_FALSE(write_result);
    REQUIRE(write_result.error);
    CHECK(write_result.error->code() == EPIPE);
}

TEST_CASE("fault: cancel race — stop fires while a pipe read is blocked") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);

    ex::inplace_stop_source stop_source;
    std::array<std::byte, 8> buffer{};
    ex::detach(io::sleep_for(context, 30ms) | ex::then([&] { stop_source.request_stop(); }));

    const auto start_time = std::chrono::steady_clock::now();
    auto result = ex::sync_wait(context, stop_source, io::read(context, pair->r, wbytes{buffer.data(), buffer.size()}));
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    REQUIRE_FALSE(result);
    REQUIRE_FALSE(result.error.has_value()); // set_stopped, never an error
    CHECK(result.stopped);
    CHECK(elapsed < 1s);
}
