// iox — unified async IO for Linux
// tests/test_splice.cc — moves), io::tee (in-pipe duplication), io::pump (bounce-pipe mover with
#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

#include <iox/compose/pump.h>
#include <iox/compose/write_all.h>
#include <iox/core/buffer.h>
#include <iox/core/exec.h>
#include <iox/core/units.h>
#include <iox/fs/file.h>
#include <iox/ops.h>
#include <iox/pipe/channel.h>

using namespace iox;
namespace ex = iox::exec;

namespace {

struct temp_path {
    std::string value;

    temp_path() : value("/tmp/iox_splice_" + std::to_string(::getpid()) + "_" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this))) {}
    ~temp_path() { ::unlink(value.c_str()); }
    temp_path(const temp_path&) = delete;
    temp_path& operator=(const temp_path&) = delete;
};

std::string slurp_pipe(io_context& ctx, pipe::read_end& r) {
    std::string got;
    std::array<std::byte, 256> buffer{};
    for (;;) {
        auto rd = ex::sync_wait(ctx, io::read(ctx, r, wbytes{buffer.data(), buffer.size()}));
        REQUIRE(rd);
        const std::size_t n = std::get<0>(*rd);
        if (n == 0) {
            return got;
        }
        got.append(reinterpret_cast<const char*>(buffer.data()), n);
    }
}

}

TEST_CASE("splice: moves bytes between pipes without userspace") {
    io_context ctx;
    auto p1 = pipe::pair::create();
    auto p2 = pipe::pair::create();
    REQUIRE(p1);
    REQUIRE(p2);
    REQUIRE(::write(p1->w.write_handle().v, "abc", 3) == 3);

    auto r = ex::sync_wait(ctx,
                           io::splice(ctx, p1->r.read_handle(), p2->w.write_handle(), 3));
    REQUIRE(r);
    CHECK(std::get<0>(*r) == 3);
    p2->w.reset();
    CHECK(slurp_pipe(ctx, p2->r) == "abc");
}

TEST_CASE("splice: pinned offset reads a file window") {
    io_context ctx;
    temp_path path;
    {
        auto f = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(f);
        REQUIRE(::pwrite(f->read_handle().v, "0123456789", 10, 0) == 10);
    }

    auto f = fs::file::open(path.value.c_str(), fs::mode::read);
    REQUIRE(f);
    auto sink = pipe::pair::create();
    REQUIRE(sink);

    auto r = ex::sync_wait(ctx, io::splice(ctx, f->read_handle(), sink->w.write_handle(),
                                           4, uoffset_t{2}));
    REQUIRE(r);
    CHECK(std::get<0>(*r) == 4);
    sink->w.reset();
    CHECK(slurp_pipe(ctx, sink->r) == "2345");
}

TEST_CASE("tee: duplicates pipe data without consuming it") {
    io_context ctx;
    auto source = pipe::pair::create();
    auto copy1 = pipe::pair::create();
    auto copy2 = pipe::pair::create();
    REQUIRE(source);
    REQUIRE(copy1);
    REQUIRE(copy2);
    REQUIRE(::write(source->w.write_handle().v, "xy", 2) == 2);

    auto t = ex::sync_wait(ctx, io::tee(ctx, source->r.read_handle(),
                                        copy1->w.write_handle(), 2));
    REQUIRE(t);
    CHECK(std::get<0>(*t) == 2);

    auto s = ex::sync_wait(ctx, io::splice(ctx, source->r.read_handle(),
                                           copy2->w.write_handle(), 2));
    REQUIRE(s);
    CHECK(std::get<0>(*s) == 2);

    copy1->w.reset();
    copy2->w.reset();
    CHECK(slurp_pipe(ctx, copy1->r) == "xy");
    CHECK(slurp_pipe(ctx, copy2->r) == "xy");
}

TEST_CASE("pump: file → file full copy (the copy_file_range shape)") {
    io_context ctx;
    temp_path src_path;
    temp_path dst_path;

    constexpr std::size_t kSize = 1 << 20;
    {
        auto source = fs::file::open(src_path.value.c_str(),
                                  fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(source);
        auto bytes = std::make_unique_for_overwrite<std::byte[]>(kSize);
        for (std::size_t i = 0; i < kSize; ++i) {
            bytes[i] = static_cast<std::byte>(i & 0xff);
        }
        auto wr = ex::sync_wait(ctx,
                                io::write_all(ctx, *source, rbytes{bytes.get(), kSize}));
        REQUIRE(wr);
    }

    auto source = fs::file::open(src_path.value.c_str(), fs::mode::read);
    auto dest = fs::file::open(dst_path.value.c_str(),
                              fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(source);
    REQUIRE(dest);

    std::uint64_t moved = 0;
    auto r = ex::sync_wait(ctx, io::pump(ctx, source->read_handle(), dest->write_handle(),
                                         64 * 1024, &moved));
    REQUIRE(r);
    CHECK(moved == kSize);

    auto verify = fs::file::open(dst_path.value.c_str(), fs::mode::read);
    REQUIRE(verify);
    auto size = verify->size();
    REQUIRE(size);
    CHECK(*size == kSize);
    auto bytes = std::make_unique_for_overwrite<std::byte[]>(kSize);
    auto rd = ex::sync_wait(ctx, io::read(ctx, *verify, wbytes{bytes.get(), kSize}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        if (bytes[i] != static_cast<std::byte>(i & 0xff)) {
            CHECK_MESSAGE(false, "payload mismatch");
            break;
        }
    }
}

TEST_CASE("pump: file → socket (the sendfile shape)") {
    io_context ctx;
    temp_path path;
    constexpr std::string_view payload = "sendfile through iox::pump";

    int sv[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0);
    {
        auto f = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(f);
        auto wr = ex::sync_wait(ctx, io::write_all(ctx, *f, as_rbytes(std::span{payload})));
        REQUIRE(wr);
        auto source = fs::file::open(path.value.c_str(), fs::mode::read);
        REQUIRE(source);

        std::uint64_t moved = 0;
        auto r = ex::sync_wait(ctx,
                               io::pump(ctx, source->read_handle(), iox::fd{sv[0]}, 4096, &moved));
        REQUIRE(r);
        CHECK(moved == payload.size());
    }
    ::shutdown(sv[0], SHUT_WR);

    std::string got;
    std::array<std::byte, 128> buffer{};
    for (;;) {
        auto rd = ex::sync_wait(ctx, io::read(ctx, iox::fd{sv[1]}, wbytes{buffer.data(), buffer.size()}));
        REQUIRE(rd);
        const std::size_t n = std::get<0>(*rd);
        if (n == 0) {
            break;
        }
        got.append(reinterpret_cast<const char*>(buffer.data()), n);
    }
    CHECK(got == payload);
    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_CASE("pump: a source that cannot splice fails cleanly, nothing moved") {
    io_context ctx;
    auto sink = pipe::pair::create();
    REQUIRE(sink);
    const int efd = ::eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd >= 0);

    std::uint64_t moved = 0;
    auto r = ex::sync_wait(ctx, io::pump(ctx, iox::fd{efd}, sink->w.write_handle(), 4096, &moved));
    REQUIRE_FALSE(r);
    REQUIRE(r.error);
    CHECK(r.error->code() == EINVAL);
    CHECK(moved == 0);

    std::byte raw[8]{};
    auto rd = ex::sync_wait(ctx, io::read(ctx, iox::fd{efd}, wbytes{raw, sizeof(raw)}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == 8);
    CHECK(std::to_integer<int>(raw[0]) == 1);
    ::close(efd);
}

TEST_CASE("pump: cancellation stops the pump mid-stream") {
    io_context ctx;
    auto source = pipe::pair::create();
    auto sink = pipe::pair::create();
    REQUIRE(source);
    REQUIRE(sink);

    ex::inplace_stop_source stop_src;
    int stopped = 0;
    struct stop_counting {
        using receiver_concept = stdexec::receiver_tag;
        ex::inplace_stop_token tok;
        int* stopped;
        auto get_env() const noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}};
        }
        void set_value() && noexcept {}
        void set_error(iox::error) && noexcept {}
        void set_error(std::exception_ptr) && noexcept {}
        void set_stopped() && noexcept { ++*stopped; }
    };
    auto op = stdexec::connect(io::pump(ctx, source->r.read_handle(), sink->w.write_handle()),
                               stop_counting{stop_src.get_token(), &stopped});
    stdexec::start(op);

    auto canceller = io::sleep_for(ctx, std::chrono::milliseconds(50)) |
                     ex::then([&] { stop_src.request_stop(); });
    REQUIRE(ex::sync_wait(ctx, stop_src, canceller));
    ctx.run_for(std::chrono::milliseconds(100));
    CHECK(stopped == 1);
}
