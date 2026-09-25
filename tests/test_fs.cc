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

std::string_view view_of(std::span<const std::byte> bytes, std::size_t length) {
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), length};
}

}

TEST_CASE("file: write_at/read_at round trip") {
    io_context context;
    temp_path path;

    auto opened = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(opened.has_value());
    fs::file& file = *opened;

    const std::string_view message = "hello iox fs";
    auto write_result = ex::sync_wait(context,
                            io::write_at(context, file, as_rbytes(std::span{message}), uoffset_t{0}));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == message.size());

    std::array<std::byte, 64> buffer{};
    auto read_result = ex::sync_wait(context, io::read_at(context, file, wbytes{buffer.data(), buffer.size()},
                                             uoffset_t{0}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == message.size());
    CHECK(view_of(buffer, std::get<0>(*read_result)) == message);

  // positional reads must not interfere: read at an offset past the data
    std::array<std::byte, 8> tail{};
    auto tail_result = ex::sync_wait(context, io::read_at(context, file, wbytes{tail.data(), tail.size()},
                                              uoffset_t{6}));
    REQUIRE(tail_result);
    CHECK(view_of(tail, std::get<0>(*tail_result)) == message.substr(6));

    auto size = file.size();
    REQUIRE(size);
    CHECK(*size == message.size());
}

TEST_CASE("file: read_at past EOF returns 0") {
    io_context context;
    temp_path path;
    auto opened = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(opened.has_value());

    std::array<std::byte, 8> buffer{};
    auto read_result = ex::sync_wait(context, io::read_at(context, *opened, wbytes{buffer.data(), buffer.size()},
                                             uoffset_t{4096}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == 0);
}

TEST_CASE("file: stream read/write and fsync") {
    io_context context;
    temp_path path;
    auto opened = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(opened.has_value());
    fs::file& file = *opened;

    const std::string_view message = "stream";
    auto write_result = ex::sync_wait(context, io::write(context, file, as_rbytes(std::span{message})));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == message.size());

    auto fsync_result = ex::sync_wait(context, io::fsync(context, file));
    CHECK(fsync_result);

    auto reopened_file = fs::file::open(path.value.c_str(), fs::mode::read);
    REQUIRE(reopened_file);
    std::array<std::byte, 32> buffer{};
    auto read_result = ex::sync_wait(context, io::read(context, *reopened_file, wbytes{buffer.data(), buffer.size()}));
    REQUIRE(read_result);
    CHECK(view_of(buffer, std::get<0>(*read_result)) == message);

    auto eof_result = ex::sync_wait(context, io::read(context, *reopened_file, wbytes{buffer.data(), buffer.size()}));
    REQUIRE(eof_result);
    CHECK(std::get<0>(*eof_result) == 0);
}

TEST_CASE("file: open failure is a typed error") {
    auto opened = fs::file::open("/definitely/not/here/iox", fs::mode::read);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code() == ENOENT);
    CHECK(opened.error().name() == std::string_view{"ENOENT"});
    CHECK_FALSE(opened.error().message().empty());
}

TEST_CASE("file: deferred close through the ring") {
    io_context context;
    temp_path path;
    auto opened = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(opened.has_value());
    fs::file& file = *opened;
    CHECK(file.read_handle().valid());

    auto close_result = ex::sync_wait(context, io::close(context, file));
    REQUIRE(close_result);
    CHECK_FALSE(file.valid());
    CHECK(file.read_handle().v == -1);

}
