// iox — unified async IO for Linux
// tests/test_fs.cc — unified vocabulary, typed open errors, fsync, deferred close.
#include <doctest/doctest.h>

#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include <iox/core/exec.h>
#include <iox/fs/file.h>
#include <iox/ops.h>

using namespace iox;
namespace ex = iox::exec;

namespace {

struct temp_path {
    std::string value;

    temp_path() : value("/tmp/iox_test_" + std::to_string(::getpid()) + "_" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this))) {}
    ~temp_path() { ::unlink(value.c_str()); }
    temp_path(const temp_path&) = delete;
    temp_path& operator=(const temp_path&) = delete;
};

std::string_view view_of(std::span<const std::byte> b, std::size_t n) {
    return std::string_view{reinterpret_cast<const char*>(b.data()), n};
}

}

TEST_CASE("file: write_at/read_at round trip") {
    io_context ctx;
    temp_path path;

    auto maybe = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(maybe.has_value());
    fs::file& f = *maybe;

    const std::string_view msg = "hello iox fs";
    auto wr = ex::sync_wait(ctx,
                            io::write_at(ctx, f, as_rbytes(std::span{msg}), uoffset_t{0}));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == msg.size());

    std::array<std::byte, 64> buf{};
    auto rd = ex::sync_wait(ctx, io::read_at(ctx, f, wbytes{buf.data(), buf.size()},
                                             uoffset_t{0}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == msg.size());
    CHECK(view_of(buf, std::get<0>(*rd)) == msg);

  // positional reads must not interfere: read at an offset past the data
    std::array<std::byte, 8> tail{};
    auto rd2 = ex::sync_wait(ctx, io::read_at(ctx, f, wbytes{tail.data(), tail.size()},
                                              uoffset_t{6}));
    REQUIRE(rd2);
    CHECK(view_of(tail, std::get<0>(*rd2)) == msg.substr(6));

    auto size = f.size();
    REQUIRE(size);
    CHECK(*size == msg.size());
}

TEST_CASE("file: read_at past EOF returns 0") {
    io_context ctx;
    temp_path path;
    auto maybe = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(maybe.has_value());

    std::array<std::byte, 8> buf{};
    auto rd = ex::sync_wait(ctx, io::read_at(ctx, *maybe, wbytes{buf.data(), buf.size()},
                                             uoffset_t{4096}));
    REQUIRE(rd);
    CHECK(std::get<0>(*rd) == 0);
}

TEST_CASE("file: stream read/write and fsync") {
    io_context ctx;
    temp_path path;
    auto maybe = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(maybe.has_value());
    fs::file& f = *maybe;

    const std::string_view msg = "stream";
    auto wr = ex::sync_wait(ctx, io::write(ctx, f, as_rbytes(std::span{msg})));
    REQUIRE(wr);
    CHECK(std::get<0>(*wr) == msg.size());

    auto sync = ex::sync_wait(ctx, io::fsync(ctx, f));
    CHECK(sync);

    auto rd_file = fs::file::open(path.value.c_str(), fs::mode::read);
    REQUIRE(rd_file);
    std::array<std::byte, 32> buf{};
    auto rd = ex::sync_wait(ctx, io::read(ctx, *rd_file, wbytes{buf.data(), buf.size()}));
    REQUIRE(rd);
    CHECK(view_of(buf, std::get<0>(*rd)) == msg);

    auto rd2 = ex::sync_wait(ctx, io::read(ctx, *rd_file, wbytes{buf.data(), buf.size()}));
    REQUIRE(rd2);
    CHECK(std::get<0>(*rd2) == 0);
}

TEST_CASE("file: open failure is a typed error") {
    auto maybe = fs::file::open("/definitely/not/here/iox", fs::mode::read);
    REQUIRE_FALSE(maybe.has_value());
    CHECK(maybe.error().code() == ENOENT);
    CHECK(maybe.error().name() == std::string_view{"ENOENT"});
    CHECK_FALSE(maybe.error().message().empty());
}

TEST_CASE("file: deferred close through the ring") {
    io_context ctx;
    temp_path path;
    auto maybe = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(maybe.has_value());
    fs::file& f = *maybe;
    CHECK(f.read_handle().valid());

    auto c = ex::sync_wait(ctx, io::close(ctx, f));
    REQUIRE(c);
    CHECK_FALSE(f.valid());
    CHECK(f.read_handle().v == -1);

}
