// Conformance suite seed (design §七.③): ONE set of assertions, run against
// every readable/writable handle pair. This file is the executable proof of
// the "unified frontend" claim — pipe ends, raw fds and files all satisfy
// the same vocabulary contract. TCP/Unix sockets join in M3; NVMe/RDMA/XDP
// handles join in M6+.
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

/// The shared contract: whatever the handle types are, write then read must
/// round-trip the exact bytes through the same vocabulary calls.
/// (Handles with independent per-end positions, like pipe ends or separate
/// opens, use the stream form; a single seekable file shares one position,
/// so its conformance case is the positional form below.)
template <class W, class R>
void rw_roundtrip(io_context& ctx, W&& w, R&& r) {
    const std::string_view msg = "unified vocabulary";
    std::array<std::byte, 64> buf{};

    auto wr = ex::sync_wait(ctx, io::write(ctx, w, as_rbytes(std::span{msg})));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == msg.size());

    auto rd = ex::sync_wait(ctx, io::read(ctx, r, wbytes{buf.data(), buf.size()}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == msg.size());
    CHECK(std::string_view{reinterpret_cast<const char*>(buf.data()), std::get<0>(*rd)} == msg);
}

/// Positional variant for single-handle seekable objects.
void rw_roundtrip_positional(io_context& ctx, fs::file& f) {
    const std::string_view msg = "unified vocabulary";
    std::array<std::byte, 64> buf{};

    auto wr = ex::sync_wait(ctx, io::write_at(ctx, f, as_rbytes(std::span{msg}), uoffset_t{0}));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == msg.size());

    auto rd = ex::sync_wait(ctx, io::read_at(ctx, f, wbytes{buf.data(), buf.size()}, uoffset_t{0}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == msg.size());
    CHECK(std::string_view{reinterpret_cast<const char*>(buf.data()), std::get<0>(*rd)} == msg);
}

/// Cancellation conformance: an in-flight read on any readable handle must
/// complete set_stopped (not error) when its stop token fires.
/// (Covered extensively in test_cancel.cc for timers; per-handle cancel
/// checks join here as handles gain M4 cancellation of blocking reads.)

} // namespace

TEST_CASE("conformance: typed pipe ends") {
    io_context ctx;
    auto p = pipe::pair::create();
    REQUIRE(p);
    rw_roundtrip(ctx, p->w, p->r);
}

TEST_CASE("conformance: raw fds use the identical contract") {
    io_context ctx;
    auto p = pipe::pair::create();
    REQUIRE(p);
    rw_roundtrip(ctx, p->w.write_handle(), p->r.read_handle());
}

TEST_CASE("conformance: fs::file read and write") {
    io_context ctx;
    const std::string path = "/tmp/iox_test_conf_" + std::to_string(::getpid());
    struct unlink_on_exit {
        std::string p;
        ~unlink_on_exit() { ::unlink(p.c_str()); }
    } guard{path};

    auto f = fs::file::open(path.c_str(), fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(f);
    rw_roundtrip_positional(ctx, *f); // one handle, one shared position
}

TEST_CASE("conformance: unix socketpair ends") {
    io_context ctx;
    auto pr = net::unix_dom::pair::create();
    REQUIRE(pr);
    rw_roundtrip(ctx, pr->a, pr->b);
}

TEST_CASE("conformance: tcp socket pair over loopback") {
    io_context ctx;
    // ephemeral listener
    auto acc = net::tcp::acceptor::listen(*net::endpoint::ipv4_any(0));
    REQUIRE(acc);
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    REQUIRE(::getsockname(acc->accept_handle().v, reinterpret_cast<sockaddr*>(&ss), &len) == 0);
    const auto port = ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);

    auto client = net::tcp::socket::unconnected(net::endpoint::family::ipv4);
    REQUIRE(client);
    auto setup = ex::when_all(io::accept(ctx, *acc),
                              io::connect(ctx, *client,
                                          *net::endpoint::ipv4("127.0.0.1", port)));
    auto done = ex::sync_wait(ctx, setup);
    REQUIRE(done);
    auto server_side = std::get<0>(std::move(*done));

    rw_roundtrip(ctx, *client, server_side);
}

TEST_CASE("conformance: registered buffers over pipe ends") {
    io_context ctx;
    auto p = pipe::pair::create();
    REQUIRE(p);
    auto pool = buffer_pool::create(ctx, 4096, 2);
    REQUIRE(pool);
    auto wbuf = pool->take();
    auto rbuf = pool->take();
    REQUIRE(wbuf.has_value());
    REQUIRE(rbuf.has_value());

    const std::string_view msg = "registered conformance";
    ::memset(wbuf->data, 0, wbuf->size);
    ::memcpy(wbuf->data, msg.data(), msg.size());

    // A registered buffer transfers as a whole slot — that is the fixed-op
    // contract; the first bytes must carry the message.
    auto wr = ex::sync_wait(ctx, io::write(ctx, p->w, *wbuf));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == wbuf->size);

    auto rd = ex::sync_wait(ctx, io::read(ctx, p->r, *rbuf));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == rbuf->size);
    CHECK(std::string_view{reinterpret_cast<const char*>(rbuf->data), msg.size()} == msg);
}

// ---------------------------------------------------------------------------
// Full-quantification additions (M6 pre-work): EOF, dead peer, cancellation
// — the same three behavioral contracts asserted across every backend.
// ---------------------------------------------------------------------------

namespace {

const std::byte conf_payload[4]{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

/// EOF conformance: after the writer side is cut, a read completes 0 — EOF
/// is a VALUE on every backend, never an error, never a hang.
template <class R>
void eof_conformance(io_context& ctx, R& r, auto&& cut_writer) {
    std::array<std::byte, 8> buf{};
    cut_writer();
    auto rd = ex::sync_wait(ctx, io::read(ctx, r, wbytes{buf.data(), buf.size()}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == 0);
}

/// Dead-reader conformance: writes after the reader is gone surface a TYPED
/// error (EPIPE, or ECONNRESET for sockets) — never SIGPIPE, never a hang.
/// A stream socket may absorb the first write into its buffers; if so, the
/// follow-up after the FIN/RST must fail.
template <class W>
void dead_reader_conformance(io_context& ctx, W& w, auto&& cut_reader) {
    cut_reader();
    auto wr = ex::sync_wait(ctx, io::write(ctx, w, rbytes{conf_payload, sizeof(conf_payload)}));
    if (!wr) {
        REQUIRE(wr.error);
        CHECK(wr.error->code() == EPIPE);
        return;
    }
    ctx.run_for(50ms);
    auto wr2 = ex::sync_wait(ctx, io::write(ctx, w, rbytes{conf_payload, sizeof(conf_payload)}));
    REQUIRE_FALSE(wr2);
    REQUIRE(wr2.error);
}

/// Cancellation conformance: a read blocked on silence completes
/// set_stopped — the RESULT carries no value and no error, and it happens
/// promptly. Asserted at the child level on purpose: composed when_all
/// semantics under an external stop request are stdexec's domain (a
/// stop-requested when_all with stop-capable children completes set_stopped
/// even when every child converted its cancellation into a value).
template <class R>
void cancel_conformance(io_context& ctx, R& r) {
    ex::inplace_stop_source src;
    std::array<std::byte, 8> buf{};
    ex::detach(io::sleep_for(ctx, 30ms) | ex::then([&] { src.request_stop(); }));
    const auto t0 = std::chrono::steady_clock::now();
    auto res = ex::sync_wait(ctx, src, io::read(ctx, r, wbytes{buf.data(), buf.size()}));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE_FALSE(res);
    REQUIRE_FALSE(res.error.has_value()); // stopped is NOT an error
    CHECK(res.stopped);
    CHECK(elapsed < 1s);
}

/// The loopback pair reused by the tcp conformance cases.
struct conf_tcp_pair {
    std::optional<net::tcp::acceptor> acc;
    std::optional<net::tcp::socket> a;
    std::optional<net::tcp::socket> b;

    static conf_tcp_pair create(io_context& ctx) {
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
        return conf_tcp_pair{std::optional<net::tcp::acceptor>{std::move(*acc)},
                             std::optional<net::tcp::socket>{std::move(*client)},
                             std::optional<net::tcp::socket>{std::get<0>(std::move(*done))}};
    }
};

} // namespace

TEST_CASE("conformance: EOF on pipe, unix pair and tcp is value 0") {
    io_context ctx;

    auto p = pipe::pair::create();
    REQUIRE(p);
    eof_conformance(ctx, p->r, [&w = p->w] { w.reset(); });

    auto up = net::unix_dom::pair::create();
    REQUIRE(up);
    eof_conformance(ctx, up->b, [&a = up->a] { a.reset(); });

    auto tp = conf_tcp_pair::create(ctx);
    eof_conformance(ctx, *tp.a, [&b = tp.b] { b.reset(); });
}

TEST_CASE("conformance: dead reader turns writes into typed errors everywhere") {
    io_context ctx;

    auto p = pipe::pair::create();
    REQUIRE(p);
    dead_reader_conformance(ctx, p->w, [&r = p->r] { r.reset(); });

    auto tp = conf_tcp_pair::create(ctx);
    dead_reader_conformance(ctx, *tp.a, [&b = tp.b] { b.reset(); });
}

TEST_CASE("conformance: cancellation of a blocked read is set_stopped everywhere") {
    io_context ctx;

    auto p = pipe::pair::create();
    REQUIRE(p);
    cancel_conformance(ctx, p->r);

    auto up = net::unix_dom::pair::create();
    REQUIRE(up);
    cancel_conformance(ctx, up->b);

    auto tp = conf_tcp_pair::create(ctx);
    cancel_conformance(ctx, *tp.a);
}

TEST_CASE("conformance: file EOF is read_at past end completing 0") {
    io_context ctx;
    const std::string path = "/tmp/iox_test_conf_eof_" + std::to_string(::getpid());
    struct unlink_on_exit {
        std::string p;
        ~unlink_on_exit() { ::unlink(p.c_str()); }
    } guard{path};

    auto wr = ex::sync_wait(ctx, io::open(ctx, path.c_str(),
                                          fs::mode::rw | fs::mode::create | fs::mode::truncate));
    REQUIRE(wr);
    auto& f = std::get<0>(*wr);
    auto w1 = ex::sync_wait(ctx,
                            io::write_at(ctx, f, rbytes{conf_payload, sizeof(conf_payload)},
                                         uoffset_t{0}));
    REQUIRE(w1);

    std::array<std::byte, 8> buf{};
    auto rd = ex::sync_wait(ctx,
                            io::read_at(ctx, f, wbytes{buf.data(), buf.size()}, uoffset_t{4}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == 0); // past the last byte: EOF value, not error
}
