// Registered-memory tests: buffer_pool + the type-driven zero-copy
// (IORING_OP_*_FIXED) read/write paths over pipe ends and files.
#include <doctest/doctest.h>

#include <unistd.h>

#include <array>
#include <cstring>
#include <string>

#include <iox/core/exec.h>
#include <iox/fs/file.h>
#include <iox/core/mr.h>
#include <iox/ops.h>
#include <iox/pipe/channel.h>

using namespace iox;
namespace ex = iox::exec;

namespace {
std::string_view view_of(std::span<const std::byte> b, std::size_t n) {
    return std::string_view{reinterpret_cast<const char*>(b.data()), n};
}
} // namespace

TEST_CASE("buffer_pool: fixed write/read round trip on pipe") {
    io_context ctx;
    auto p = pipe::pair::create();
    REQUIRE(p);

    auto pool = buffer_pool::create(ctx, 4096, 4);
    REQUIRE(pool);
    CHECK(pool->available() == 4);

    auto wbuf = pool->take();
    REQUIRE(wbuf);
    auto rbuf = pool->take();
    REQUIRE(rbuf);

    const std::string_view msg = "registered zero-copy";
    ::memset(wbuf->data, 0, wbuf->size);
    ::memcpy(wbuf->data, msg.data(), msg.size());

    // The registered_buffer overload selects IORING_OP_WRITE_FIXED and
    // transfers the whole slot — the fixed-op contract.
    auto wr = ex::sync_wait(ctx, io::write(ctx, p->w, *wbuf));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == wbuf->size);

    auto rd = ex::sync_wait(ctx, io::read(ctx, p->r, *rbuf));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == rbuf->size);
    CHECK(view_of(rbuf->readable().as_span(), msg.size()) == msg);

    pool->give_back(*wbuf);
    pool->give_back(*rbuf);
    CHECK(pool->available() == 4);

    // double give_back is ignored deterministically
    pool->give_back(*wbuf);
    CHECK(pool->available() == 4);
}

TEST_CASE("buffer_pool: exhaustion is a typed error") {
    io_context ctx;
    auto pool = buffer_pool::create(ctx, 4096, 2);
    REQUIRE(pool);

    auto a = pool->take();
    auto b = pool->take();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    auto c = pool->take();
    REQUIRE_FALSE(c);
    CHECK(c.error().code() == EBUSY);

    pool->give_back(*a);
    auto d = pool->take();
    REQUIRE(d);
}

TEST_CASE("buffer_pool: one table per ring") {
    io_context ctx;
    auto first = buffer_pool::create(ctx, 4096, 2);
    REQUIRE(first);

    auto second = buffer_pool::create(ctx, 4096, 2);
    REQUIRE_FALSE(second);
    CHECK(second.error().code() == EBUSY);

    first->reset(); // unregisters
    auto third = buffer_pool::create(ctx, 4096, 2);
    REQUIRE(third);
}

TEST_CASE("buffer_pool: fixed read/write on file") {
    io_context ctx;
    const std::string path = "/tmp/iox_test_mr_" + std::to_string(::getpid());
    auto f = fs::file::open(path.c_str(), fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(f);
    struct unlink_on_exit {
        std::string p;
        ~unlink_on_exit() { ::unlink(p.c_str()); }
    } guard{path};

    auto pool = buffer_pool::create(ctx, 8192, 2);
    REQUIRE(pool);

    auto wbuf = pool->take();
    auto rbuf = pool->take();
    REQUIRE(wbuf.has_value());
    REQUIRE(rbuf.has_value());

    const std::string_view msg = "fixed path on files";
    ::memset(wbuf->data, 0, wbuf->size);
    ::memcpy(wbuf->data, msg.data(), msg.size());

    auto wr = ex::sync_wait(ctx, io::write_at(ctx, *f, wbuf->readable(), uoffset_t{0}));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == wbuf->size);

    auto rd = ex::sync_wait(ctx, io::read_at(ctx, *f, rbuf->writable(), uoffset_t{0}));
    REQUIRE(rd);
    CHECK(view_of(rbuf->readable().as_span(), msg.size()) == msg);
}
