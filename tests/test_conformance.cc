// iox — unified async IO for Linux
// tests/test_conformance.cc — every readable/writable handle pair. This file is the executable proof of
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <optional>
#include <cstddef>
#include <cstring>
#include <string_view>

#include <unistd.h>

#include <iox/compose/detach.h>
#include <iox/core/exec.h>
#include <iox/fs/file.h>
#include <iox/core/mr.h>
#include <iox/net/tcp.h>
#include <iox/net/unix.h>
#include <iox/ops.h>
#include <iox/pipe/channel.h>

using namespace iox;
namespace ex = iox::exec;
using namespace std::chrono_literals;

namespace {

template <class Writer, class Reader>
void rw_roundtrip(io_context& context, Writer&& writer, Reader&& reader) {
    const std::string_view message = "unified vocabulary";
    std::array<std::byte, 64> buffer{};

    auto write_result = ex::sync_wait(context, io::write(context, writer, as_rbytes(std::span{message})));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == message.size());

    auto read_result = ex::sync_wait(context, io::read(context, reader, wbytes{buffer.data(), buffer.size()}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == message.size());
    CHECK(std::string_view{reinterpret_cast<const char*>(buffer.data()), std::get<0>(*read_result)} == message);
}

void rw_roundtrip_positional(io_context& context, fs::file& file) {
    const std::string_view message = "unified vocabulary";
    std::array<std::byte, 64> buffer{};

    auto write_result = ex::sync_wait(context, io::write_at(context, file, as_rbytes(std::span{message}), uoffset_t{0}));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == message.size());

    auto read_result = ex::sync_wait(context, io::read_at(context, file, wbytes{buffer.data(), buffer.size()}, uoffset_t{0}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == message.size());
    CHECK(std::string_view{reinterpret_cast<const char*>(buffer.data()), std::get<0>(*read_result)} == message);
}

}

TEST_CASE("conformance: typed pipe ends") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);
    rw_roundtrip(context, pair->w, pair->r);
}

TEST_CASE("conformance: raw fds use the identical contract") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);
    rw_roundtrip(context, pair->w.write_handle(), pair->r.read_handle());
}

TEST_CASE("conformance: fs::file read and write") {
    io_context context;
    const std::string path = "/tmp/iox_test_conf_" + std::to_string(::getpid());
    struct unlink_on_exit {
        std::string path;
        ~unlink_on_exit() { ::unlink(path.c_str()); }
    } guard{path};

    auto file = fs::file::open(path.c_str(), fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(file);
    rw_roundtrip_positional(context, *file);
}

TEST_CASE("conformance: unix socketpair ends") {
    io_context context;
    auto unix_pair = net::unix_dom::pair::create();
    REQUIRE(unix_pair);
    rw_roundtrip(context, unix_pair->a, unix_pair->b);
}

TEST_CASE("conformance: tcp socket pair over loopback") {
    io_context context;
    auto acceptor = net::tcp::acceptor::listen(*net::endpoint::ipv4_any(0));
    REQUIRE(acceptor);
    sockaddr_storage storage{};
    socklen_t address_length = sizeof(storage);
    REQUIRE(::getsockname(acceptor->accept_handle().v, reinterpret_cast<sockaddr*>(&storage), &address_length) == 0);
    const auto port = ntohs(reinterpret_cast<sockaddr_in*>(&storage)->sin_port);

    auto client = net::tcp::socket::unconnected(net::endpoint::family::ipv4);
    REQUIRE(client);
    auto setup = ex::when_all(io::accept(context, *acceptor),
                              io::connect(context, *client,
                                          *net::endpoint::ipv4("127.0.0.1", port)));
    auto setup_result = ex::sync_wait(context, setup);
    REQUIRE(setup_result);
    auto server_side = std::get<0>(std::move(*setup_result));

    rw_roundtrip(context, *client, server_side);
}

TEST_CASE("conformance: registered buffers over pipe ends") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);
    auto pool = buffer_pool::create(context, 4096, 2);
    REQUIRE(pool);
    auto write_buffer = pool->take();
    auto read_buffer = pool->take();
    REQUIRE(write_buffer.has_value());
    REQUIRE(read_buffer.has_value());

    const std::string_view message = "registered conformance";
    ::memset(write_buffer->data, 0, write_buffer->size);
    ::memcpy(write_buffer->data, message.data(), message.size());

    auto write_result = ex::sync_wait(context, io::write(context, pair->w, *write_buffer));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == write_buffer->size);

    auto read_result = ex::sync_wait(context, io::read(context, pair->r, *read_buffer));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == read_buffer->size);
    CHECK(std::string_view{reinterpret_cast<const char*>(read_buffer->data), message.size()} == message);
}

namespace {

const std::byte conf_payload[4]{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

template <class Reader>
void eof_conformance(io_context& context, Reader& reader, auto&& cut_writer) {
    std::array<std::byte, 8> buffer{};
    cut_writer();
    auto read_result = ex::sync_wait(context, io::read(context, reader, wbytes{buffer.data(), buffer.size()}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == 0);
}

template <class Writer>
void dead_reader_conformance(io_context& context, Writer& writer, auto&& cut_reader) {
    cut_reader();
    auto write_result = ex::sync_wait(context, io::write(context, writer, rbytes{conf_payload, sizeof(conf_payload)}));
    if (!write_result) {
        REQUIRE(write_result.error);
        CHECK(write_result.error->code() == EPIPE);
        return;
    }
    context.run_for(50ms);
    auto retry_result = ex::sync_wait(context, io::write(context, writer, rbytes{conf_payload, sizeof(conf_payload)}));
    REQUIRE_FALSE(retry_result);
    REQUIRE(retry_result.error);
}

template <class Reader>
void cancel_conformance(io_context& context, Reader& reader) {
    ex::inplace_stop_source stop_source;
    std::array<std::byte, 8> buffer{};
    ex::detach(io::sleep_for(context, 30ms) | ex::then([&] { stop_source.request_stop(); }));
    const auto start_time = std::chrono::steady_clock::now();
    auto result = ex::sync_wait(context, stop_source, io::read(context, reader, wbytes{buffer.data(), buffer.size()}));
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    REQUIRE_FALSE(result);
    REQUIRE_FALSE(result.error.has_value());
    CHECK(result.stopped);
    CHECK(elapsed < 1s);
}

struct conf_tcp_pair {
    std::optional<net::tcp::acceptor> acceptor;
    std::optional<net::tcp::socket> client;
    std::optional<net::tcp::socket> server;

    static conf_tcp_pair create(io_context& context) {
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
        return conf_tcp_pair{std::optional<net::tcp::acceptor>{std::move(*acceptor)},
                             std::optional<net::tcp::socket>{std::move(*client)},
                             std::optional<net::tcp::socket>{std::get<0>(std::move(*connected))}};
    }
};

}

TEST_CASE("conformance: EOF on pipe, unix pair and tcp is value 0") {
    io_context context;

    auto pair = pipe::pair::create();
    REQUIRE(pair);
    eof_conformance(context, pair->r, [&writer = pair->w] { writer.reset(); });

    auto unix_pair = net::unix_dom::pair::create();
    REQUIRE(unix_pair);
    eof_conformance(context, unix_pair->b, [&peer = unix_pair->a] { peer.reset(); });

    auto tcp_pair = conf_tcp_pair::create(context);
    eof_conformance(context, *tcp_pair.client, [&server = tcp_pair.server] { server.reset(); });
}

TEST_CASE("conformance: dead reader turns writes into typed errors everywhere") {
    io_context context;

    auto pair = pipe::pair::create();
    REQUIRE(pair);
    dead_reader_conformance(context, pair->w, [&reader = pair->r] { reader.reset(); });

    auto tcp_pair = conf_tcp_pair::create(context);
    dead_reader_conformance(context, *tcp_pair.client, [&server = tcp_pair.server] { server.reset(); });
}

TEST_CASE("conformance: cancellation of a blocked read is set_stopped everywhere") {
    io_context context;

    auto pair = pipe::pair::create();
    REQUIRE(pair);
    cancel_conformance(context, pair->r);

    auto unix_pair = net::unix_dom::pair::create();
    REQUIRE(unix_pair);
    cancel_conformance(context, unix_pair->b);

    auto tcp_pair = conf_tcp_pair::create(context);
    cancel_conformance(context, *tcp_pair.client);
}

TEST_CASE("conformance: file EOF is read_at past end completing 0") {
    io_context context;
    const std::string path = "/tmp/iox_test_conf_eof_" + std::to_string(::getpid());
    struct unlink_on_exit {
        std::string path;
        ~unlink_on_exit() { ::unlink(path.c_str()); }
    } guard{path};

    auto opened = ex::sync_wait(context, io::open(context, path.c_str(),
                                          fs::mode::rw | fs::mode::create | fs::mode::truncate));
    REQUIRE(opened);
    auto& file = std::get<0>(*opened);
    auto write_result = ex::sync_wait(context,
                            io::write_at(context, file, rbytes{conf_payload, sizeof(conf_payload)},
                                         uoffset_t{0}));
    REQUIRE(write_result);

    std::array<std::byte, 8> buffer{};
    auto read_result = ex::sync_wait(context,
                            io::read_at(context, file, wbytes{buffer.data(), buffer.size()}, uoffset_t{4}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == 0);
}
