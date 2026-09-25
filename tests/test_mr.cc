// iox — unified async IO for Linux
// tests/test_mr.cc — (IORING_OP_*_FIXED) read/write paths over pipe ends and files.
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
std::string_view view_of(std::span<const std::byte> bytes, std::size_t length) {
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), length};
}
}

TEST_CASE("buffer_pool: fixed write/read round trip on pipe") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);

    auto pool = buffer_pool::create(context, 4096, 4);
    REQUIRE(pool);
    CHECK(pool->available() == 4);

    auto write_buffer = pool->take();
    REQUIRE(write_buffer);
    auto read_buffer = pool->take();
    REQUIRE(read_buffer);

    const std::string_view message = "registered zero-copy";
    ::memset(write_buffer->data, 0, write_buffer->size);
    ::memcpy(write_buffer->data, message.data(), message.size());

    auto write_result = ex::sync_wait(context, io::write(context, pair->w, *write_buffer));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == write_buffer->size);

    auto read_result = ex::sync_wait(context, io::read(context, pair->r, *read_buffer));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == read_buffer->size);
    CHECK(view_of(read_buffer->readable().as_span(), message.size()) == message);

    pool->give_back(*write_buffer);
    pool->give_back(*read_buffer);
    CHECK(pool->available() == 4);

    pool->give_back(*write_buffer);
    CHECK(pool->available() == 4);
}

TEST_CASE("buffer_pool: exhaustion is a typed error") {
    io_context context;
    auto pool = buffer_pool::create(context, 4096, 2);
    REQUIRE(pool);

    auto first = pool->take();
    auto second = pool->take();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    auto exhausted = pool->take();
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error().code() == EBUSY);

    pool->give_back(*first);
    auto reused = pool->take();
    REQUIRE(reused);
}

TEST_CASE("buffer_pool: one table per ring") {
    io_context context;
    auto first = buffer_pool::create(context, 4096, 2);
    REQUIRE(first);

    auto second = buffer_pool::create(context, 4096, 2);
    REQUIRE_FALSE(second);
    CHECK(second.error().code() == EBUSY);

    first->reset();
    auto third = buffer_pool::create(context, 4096, 2);
    REQUIRE(third);
}

TEST_CASE("buffer_pool: fixed read/write on file") {
    io_context context;
    const std::string path = "/tmp/iox_test_mr_" + std::to_string(::getpid());
    auto file = fs::file::open(path.c_str(), fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(file);
    struct unlink_on_exit {
        std::string path;
        ~unlink_on_exit() { ::unlink(path.c_str()); }
    } guard{path};

    auto pool = buffer_pool::create(context, 8192, 2);
    REQUIRE(pool);

    auto write_buffer = pool->take();
    auto read_buffer = pool->take();
    REQUIRE(write_buffer.has_value());
    REQUIRE(read_buffer.has_value());

    const std::string_view message = "fixed path on files";
    ::memset(write_buffer->data, 0, write_buffer->size);
    ::memcpy(write_buffer->data, message.data(), message.size());

    auto write_result = ex::sync_wait(context, io::write_at(context, *file, write_buffer->readable(), uoffset_t{0}));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == write_buffer->size);

    auto read_result = ex::sync_wait(context, io::read_at(context, *file, read_buffer->writable(), uoffset_t{0}));
    REQUIRE(read_result);
    CHECK(view_of(read_buffer->readable().as_span(), message.size()) == message);
}
