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
    auto acceptor = net::tcp::acceptor::listen(net::endpoint::ipv4_any(0).value());
    REQUIRE(acceptor);
    sockaddr_storage storage{};
    socklen_t address_length = sizeof(storage);
    (void)!::getsockname(acceptor->accept_handle().v, reinterpret_cast<sockaddr*>(&storage), &address_length);
    return ntohs(reinterpret_cast<sockaddr_in*>(&storage)->sin_port);
}
}

TEST_CASE("endpoint: parse and format round trips") {
    auto ipv4_endpoint = net::endpoint::parse("127.0.0.1:8080");
    REQUIRE(ipv4_endpoint);
    CHECK(ipv4_endpoint->to_string() == "127.0.0.1:8080");
    CHECK(ipv4_endpoint->port() == 8080);

    auto ipv6_endpoint = net::endpoint::parse("[::1]:9999");
    REQUIRE(ipv6_endpoint);
    CHECK(ipv6_endpoint->family() == net::endpoint::family_t::ipv6);
    CHECK(ipv6_endpoint->port() == 9999);

    auto path = net::endpoint::parse("unix:/tmp/ioxsock");
    REQUIRE(path);
    CHECK(path->to_string() == "/tmp/ioxsock");

    auto abstract_endpoint = net::endpoint::parse("@iox-abstract");
    REQUIRE(abstract_endpoint);
    CHECK(abstract_endpoint->to_string() == "@iox-abstract");

    CHECK_FALSE(net::endpoint::parse("no-port-here"));
    CHECK_FALSE(net::endpoint::parse("1.2.3.4:99999"));
    CHECK_FALSE(net::endpoint::parse("1.2.3.4:notaport"));
}

TEST_CASE("tcp: accept, connect, echo over loopback") {
    io_context context;
    const auto port = free_port();

    auto acceptor = net::tcp::acceptor::listen(*net::endpoint::ipv4_any(port));
    REQUIRE(acceptor);

    auto client = net::tcp::socket::unconnected(net::endpoint::family_t::ipv4);
    REQUIRE(client);

    std::byte buffer[64];
    auto setup = ex::when_all(io::accept(context, *acceptor), io::connect(context, *client, *net::endpoint::ipv4("127.0.0.1", port)));
    auto setup_result = ex::sync_wait(context, setup);
    REQUIRE(setup_result);
    auto server_side = std::get<0>(std::move(*setup_result));
    CHECK(server_side.valid());

    auto peer = client->peer();
    REQUIRE(peer);
    CHECK(peer->port() == port);

    const std::string_view message = "tcp echo";
    auto write_result = ex::sync_wait(context, io::write(context, *client, as_rbytes(std::span{message})));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == message.size());

    auto read_result = ex::sync_wait(context, io::read(context, server_side, wbytes{buffer, sizeof(buffer)}));
    REQUIRE(read_result);
    CHECK(std::string_view{reinterpret_cast<const char*>(buffer), std::get<0>(*read_result)} == message);
}

TEST_CASE("tcp: connect to a dead port is a typed error") {
    io_context context;
    const auto port = free_port();

    auto client = net::tcp::socket::unconnected(net::endpoint::family_t::ipv4);
    REQUIRE(client);
    auto connect_result = ex::sync_wait(context, io::connect(context, *client,
                                            *net::endpoint::ipv4("127.0.0.1", port)));
    REQUIRE_FALSE(connect_result);
    REQUIRE(connect_result.error.has_value());
    CHECK(connect_result.error->code() == ECONNREFUSED);
}

TEST_CASE("unix: stream acceptor, connect and echo (path and abstract)") {
    for (const std::string_view spec : {"unix:/tmp/iox_test_stream.sock", "@iox-test-abs"}) {
        io_context context;
        auto endpoint = net::endpoint::parse(spec);
        REQUIRE(endpoint);

        auto acceptor = net::unix_dom::acceptor::listen(*endpoint);
        REQUIRE(acceptor);

        auto client = net::unix_dom::socket::unconnected();
        REQUIRE(client);

        auto setup = ex::when_all(io::accept(context, *acceptor), io::connect(context, *client, *endpoint));
        auto setup_result = ex::sync_wait(context, setup);
        REQUIRE(setup_result);
        auto server_side = std::get<0>(std::move(*setup_result));

        const std::string_view message = "unix echo";
        auto write_result = ex::sync_wait(context, io::write(context, *client, as_rbytes(std::span{message})));
        REQUIRE(write_result);
        std::byte buffer[64];
        auto read_result = ex::sync_wait(context, io::read(context, server_side, wbytes{buffer, sizeof(buffer)}));
        REQUIRE(read_result);
        CHECK(std::string_view{reinterpret_cast<const char*>(buffer), std::get<0>(*read_result)} == message);
    }
}

TEST_CASE("udp: send_to / recv_from with source endpoint") {
    io_context context;
    const auto sender_port = free_port();
    const auto receiver_port = free_port();

    auto sender = net::udp::socket::open(*net::endpoint::ipv4_any(sender_port));
    auto receiver = net::udp::socket::open(*net::endpoint::ipv4_any(receiver_port));
    REQUIRE(sender);
    REQUIRE(receiver);

    const std::string_view message = "udp datagram";
    auto send_result = ex::sync_wait(
        context, io::send_to(context, *sender, as_rbytes(std::span{message}),
                         *net::endpoint::ipv4("127.0.0.1", receiver_port)));
    REQUIRE(send_result);
    CHECK(std::get<0>(*send_result) == message.size());

    std::byte buffer[128];
    auto recv_result = ex::sync_wait(context, io::recv_from(context, *receiver, wbytes{buffer, sizeof(buffer)}));
    REQUIRE(recv_result);
    const auto byte_count = std::get<0>(*recv_result);
    const auto& from = std::get<1>(*recv_result);
    CHECK(std::string_view{reinterpret_cast<const char*>(buffer), byte_count} == message);
    CHECK(from.port() == sender_port);
}

TEST_CASE("io::loop: repeats until the body says stop") {
    io_context context;
    int iterations = 0;

    auto countdown = io::loop(context, [&]() {
        ++iterations;
        return io::sleep_for(context, 1ms) | ex::then([&]() { return iterations >= 5; });
    });
    auto result = ex::sync_wait(context, countdown);
    REQUIRE(result);
    CHECK(iterations == 5);
}

TEST_CASE("io::loop + exec::detach: full echo session lifecycle to EOF") {
    io_context context;
    auto pair = net::unix_dom::pair::create();
    REQUIRE(pair);
    net::unix_dom::socket& session_end = pair->a;
    net::unix_dom::socket& client_end = pair->b;

    auto buffer = std::make_unique<std::byte[]>(64);
    bool session_finished = false;
    auto session = io::loop(context, [&]() {
                  return io::read(context, session_end, wbytes{buffer.get(), 64})
                       | ex::let_value([&](std::size_t byte_count) {
                             return io::write(context, session_end, rbytes{buffer.get(), byte_count});
                         })
                       | ex::then([](std::size_t bytes_written) { return bytes_written == 0; });
              })
                  | ex::then([&]() { session_finished = true; });
    ex::detach(std::move(session));

    const std::string_view message = "session lifecycle";
    auto write_result = ex::sync_wait(context, io::write(context, client_end, as_rbytes(std::span{message})));
    REQUIRE(write_result);

    std::byte echo_buffer[64];
    auto read_result = ex::sync_wait(context, io::read(context, client_end, wbytes{echo_buffer, sizeof(echo_buffer)}));
    REQUIRE(read_result);
    CHECK(std::string_view{reinterpret_cast<const char*>(echo_buffer),
                           std::get<0>(*read_result)} == message);

    client_end.reset();
    context.run_for(100ms);

    CHECK(session_finished);
}

TEST_CASE("io::write_all: drains a payload larger than the socket buffer") {
    io_context context;
    auto pair = net::unix_dom::pair::create();
    REQUIRE(pair);

    constexpr std::size_t kSize = 1u << 20;
    auto source = std::make_unique_for_overwrite<std::byte[]>(kSize);
    for (std::size_t index = 0; index < kSize; ++index) {
        source[index] = static_cast<std::byte>(index * 31u + 7u);
    }
    auto destination = std::make_unique_for_overwrite<std::byte[]>(kSize);

    ex::detach(io::write_all(context, pair->b, rbytes{source.get(), kSize})
                   | ex::then([&] { pair->b.reset(); }));

    std::size_t received = 0;
    auto reader = io::loop(context, [&]() {
        return io::read(context, pair->a, wbytes{destination.get() + received, kSize - received})
             | ex::then([&](std::size_t byte_count) {
                   received += byte_count;
                   return byte_count == 0;
               });
    });
    REQUIRE(ex::sync_wait(context, reader));

    CHECK(received == kSize);
    CHECK(std::memcmp(source.get(), destination.get(), kSize) == 0);
}
