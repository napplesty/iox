// iox — unified async IO for Linux
// tests/test_net.cc — stream (path + abstract), UDP send_to/recv_from, endpoint parse/format,
#include <doctest/doctest.h>

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include <iox/core/exec.h>
#include <iox/compose/loop.h>
#include <iox/compose/write_all.h>
#include <iox/compose/detach.h>
#include <iox/net/tcp.h>
#include <iox/net/udp.h>
#include <iox/net/unix.h>
#include <iox/ops.h>

using namespace iox;
namespace ex = iox::exec;
using namespace std::chrono_literals;

namespace {
std::uint16_t free_port() {
    auto a = net::tcp::acceptor::listen(net::endpoint::ipv4_any(0).value());
    REQUIRE(a);
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    (void)!::getsockname(a->accept_handle().v, reinterpret_cast<sockaddr*>(&ss), &len);
    return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
}
}

TEST_CASE("endpoint: parse and format round trips") {
    auto v4 = net::endpoint::parse("127.0.0.1:8080");
    REQUIRE(v4);
    CHECK(v4->to_string() == "127.0.0.1:8080");
    CHECK(v4->port() == 8080);

    auto v6 = net::endpoint::parse("[::1]:9999");
    REQUIRE(v6);
    CHECK(v6->family() == net::endpoint::family_t::ipv6);
    CHECK(v6->port() == 9999);

    auto path = net::endpoint::parse("unix:/tmp/ioxsock");
    REQUIRE(path);
    CHECK(path->to_string() == "/tmp/ioxsock");

    auto abs = net::endpoint::parse("@iox-abstract");
    REQUIRE(abs);
    CHECK(abs->to_string() == "@iox-abstract");

    CHECK_FALSE(net::endpoint::parse("no-port-here"));
    CHECK_FALSE(net::endpoint::parse("1.2.3.4:99999"));
    CHECK_FALSE(net::endpoint::parse("1.2.3.4:notaport"));
}

TEST_CASE("tcp: accept, connect, echo over loopback") {
    io_context ctx;
    const auto port = free_port();

    auto acc = net::tcp::acceptor::listen(*net::endpoint::ipv4_any(port));
    REQUIRE(acc);

    auto client = net::tcp::socket::unconnected(net::endpoint::family_t::ipv4);
    REQUIRE(client);

    std::byte buffer[64];
    auto setup = ex::when_all(io::accept(ctx, *acc), io::connect(ctx, *client, *net::endpoint::ipv4("127.0.0.1", port)));
    auto done = ex::sync_wait(ctx, setup);
    REQUIRE(done);
    auto server_side = std::get<0>(std::move(*done));
    CHECK(server_side.valid());

    auto peer = client->peer();
    REQUIRE(peer);
    CHECK(peer->port() == port);

    const std::string_view msg = "tcp echo";
    auto wr = ex::sync_wait(ctx, io::write(ctx, *client, as_rbytes(std::span{msg})));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == msg.size());

    auto rd = ex::sync_wait(ctx, io::read(ctx, server_side, wbytes{buffer, sizeof(buffer)}));
    REQUIRE(rd);
    CHECK(std::string_view{reinterpret_cast<const char*>(buffer), std::get<0>(*rd)} == msg);
}

TEST_CASE("tcp: connect to a dead port is a typed error") {
    io_context ctx;
    const auto port = free_port();

    auto client = net::tcp::socket::unconnected(net::endpoint::family_t::ipv4);
    REQUIRE(client);
    auto r = ex::sync_wait(ctx, io::connect(ctx, *client,
                                            *net::endpoint::ipv4("127.0.0.1", port)));
    REQUIRE_FALSE(r);
    REQUIRE(r.error.has_value());
    CHECK(r.error->code() == ECONNREFUSED);
}

TEST_CASE("unix: stream acceptor, connect and echo (path and abstract)") {
    for (const std::string_view spec : {"unix:/tmp/iox_test_stream.sock", "@iox-test-abs"}) {
        io_context ctx;
        auto ep = net::endpoint::parse(spec);
        REQUIRE(ep);

        auto acc = net::unix_dom::acceptor::listen(*ep);
        REQUIRE(acc);

        auto client = net::unix_dom::socket::unconnected();
        REQUIRE(client);

        auto setup = ex::when_all(io::accept(ctx, *acc), io::connect(ctx, *client, *ep));
        auto done = ex::sync_wait(ctx, setup);
        REQUIRE(done);
        auto server_side = std::get<0>(std::move(*done));

        const std::string_view msg = "unix echo";
        auto wr = ex::sync_wait(ctx, io::write(ctx, *client, as_rbytes(std::span{msg})));
        REQUIRE(wr);
        std::byte buffer[64];
        auto rd = ex::sync_wait(ctx, io::read(ctx, server_side, wbytes{buffer, sizeof(buffer)}));
        REQUIRE(rd);
        CHECK(std::string_view{reinterpret_cast<const char*>(buffer), std::get<0>(*rd)} == msg);
    }
}

TEST_CASE("udp: send_to / recv_from with source endpoint") {
    io_context ctx;
    const auto port_a = free_port();
    const auto port_b = free_port();

    auto a = net::udp::socket::open(*net::endpoint::ipv4_any(port_a));
    auto b = net::udp::socket::open(*net::endpoint::ipv4_any(port_b));
    REQUIRE(a);
    REQUIRE(b);

    const std::string_view msg = "udp datagram";
    auto wr = ex::sync_wait(
        ctx, io::send_to(ctx, *a, as_rbytes(std::span{msg}),
                         *net::endpoint::ipv4("127.0.0.1", port_b)));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == msg.size());

    std::byte buffer[128];
    auto rd = ex::sync_wait(ctx, io::recv_from(ctx, *b, wbytes{buffer, sizeof(buffer)}));
    REQUIRE(rd);
    const auto n = std::get<0>(*rd);
    const auto& from = std::get<1>(*rd);
    CHECK(std::string_view{reinterpret_cast<const char*>(buffer), n} == msg);
    CHECK(from.port() == port_a);
}

TEST_CASE("io::loop: repeats until the body says stop") {
    io_context ctx;
    int iterations = 0;

    auto countdown = io::loop(ctx, [&]() {
        ++iterations;
        return io::sleep_for(ctx, 1ms) | ex::then([&]() { return iterations >= 5; });
    });
    auto r = ex::sync_wait(ctx, countdown);
    REQUIRE(r);
    CHECK(iterations == 5);
}

TEST_CASE("io::loop + exec::detach: full echo session lifecycle to EOF") {
    io_context ctx;
    auto pr = net::unix_dom::pair::create();
    REQUIRE(pr);
    net::unix_dom::socket& a = pr->a;
    net::unix_dom::socket& b = pr->b;

    auto buffer = std::make_unique<std::byte[]>(64);
    bool session_finished = false;
    auto session = io::loop(ctx, [&]() {
                  return io::read(ctx, a, wbytes{buffer.get(), 64})
                       | ex::let_value([&](std::size_t n) {
                             return io::write(ctx, a, rbytes{buffer.get(), n});
                         })
                       | ex::then([](std::size_t w) { return w == 0; });
              })
                  | ex::then([&]() { session_finished = true; });
    ex::detach(std::move(session));

    const std::string_view msg = "session lifecycle";
    auto wr = ex::sync_wait(ctx, io::write(ctx, b, as_rbytes(std::span{msg})));
    REQUIRE(wr);

    std::byte echo_buf[64];
    auto rd = ex::sync_wait(ctx, io::read(ctx, b, wbytes{echo_buf, sizeof(echo_buf)}));
    REQUIRE(rd);
    CHECK(std::string_view{reinterpret_cast<const char*>(echo_buf),
                           std::get<0>(*rd)} == msg);

    b.reset();
    ctx.run_for(100ms);

    CHECK(session_finished);
}

TEST_CASE("io::write_all: drains a payload larger than the socket buffer") {
    io_context ctx;
    auto pr = net::unix_dom::pair::create();
    REQUIRE(pr);

    constexpr std::size_t kSize = 1u << 20;
    auto source = std::make_unique_for_overwrite<std::byte[]>(kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        source[i] = static_cast<std::byte>(i * 31u + 7u);
    }
    auto dest = std::make_unique_for_overwrite<std::byte[]>(kSize);

    ex::detach(io::write_all(ctx, pr->b, rbytes{source.get(), kSize})
                   | ex::then([&] { pr->b.reset(); }));

    std::size_t got = 0;
    auto reader = io::loop(ctx, [&]() {
        return io::read(ctx, pr->a, wbytes{dest.get() + got, kSize - got})
             | ex::then([&](std::size_t n) {
                   got += n;
                   return n == 0;
               });
    });
    REQUIRE(ex::sync_wait(ctx, reader));

    CHECK(got == kSize);
    CHECK(std::memcmp(source.get(), dest.get(), kSize) == 0);
}
