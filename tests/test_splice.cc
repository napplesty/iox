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

std::string slurp_pipe(io_context& context, pipe::read_end& reader) {
    std::string received;
    std::array<std::byte, 256> buffer{};
    for (;;) {
        auto read_result = ex::sync_wait(context, io::read(context, reader, wbytes{buffer.data(), buffer.size()}));
        REQUIRE(read_result);
        const std::size_t byte_count = std::get<0>(*read_result);
        if (byte_count == 0) {
            return received;
        }
        received.append(reinterpret_cast<const char*>(buffer.data()), byte_count);
    }
}

}

TEST_CASE("splice: moves bytes between pipes without userspace") {
    io_context context;
    auto source_pair = pipe::pair::create();
    auto sink_pair = pipe::pair::create();
    REQUIRE(source_pair);
    REQUIRE(sink_pair);
    REQUIRE(::write(source_pair->w.write_handle().v, "abc", 3) == 3);

    auto splice_result = ex::sync_wait(context,
                           io::splice(context, source_pair->r.read_handle(), sink_pair->w.write_handle(), 3));
    REQUIRE(splice_result);
    CHECK(std::get<0>(*splice_result) == 3);
    sink_pair->w.reset();
    CHECK(slurp_pipe(context, sink_pair->r) == "abc");
}

TEST_CASE("splice: pinned offset reads a file window") {
    io_context context;
    temp_path path;
    {
        auto file = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(file);
        REQUIRE(::pwrite(file->read_handle().v, "0123456789", 10, 0) == 10);
    }

    auto file = fs::file::open(path.value.c_str(), fs::mode::read);
    REQUIRE(file);
    auto sink = pipe::pair::create();
    REQUIRE(sink);

    auto splice_result = ex::sync_wait(context, io::splice(context, file->read_handle(), sink->w.write_handle(),
                                           4, uoffset_t{2}));
    REQUIRE(splice_result);
    CHECK(std::get<0>(*splice_result) == 4);
    sink->w.reset();
    CHECK(slurp_pipe(context, sink->r) == "2345");
}

TEST_CASE("tee: duplicates pipe data without consuming it") {
    io_context context;
    auto source = pipe::pair::create();
    auto copy1 = pipe::pair::create();
    auto copy2 = pipe::pair::create();
    REQUIRE(source);
    REQUIRE(copy1);
    REQUIRE(copy2);
    REQUIRE(::write(source->w.write_handle().v, "xy", 2) == 2);

    auto tee_result = ex::sync_wait(context, io::tee(context, source->r.read_handle(),
                                        copy1->w.write_handle(), 2));
    REQUIRE(tee_result);
    CHECK(std::get<0>(*tee_result) == 2);

    auto splice_result = ex::sync_wait(context, io::splice(context, source->r.read_handle(),
                                           copy2->w.write_handle(), 2));
    REQUIRE(splice_result);
    CHECK(std::get<0>(*splice_result) == 2);

    copy1->w.reset();
    copy2->w.reset();
    CHECK(slurp_pipe(context, copy1->r) == "xy");
    CHECK(slurp_pipe(context, copy2->r) == "xy");
}

TEST_CASE("pump: file → file full copy (the copy_file_range shape)") {
    io_context context;
    temp_path source_path;
    temp_path destination_path;

    constexpr std::size_t kSize = 1 << 20;
    {
        auto source = fs::file::open(source_path.value.c_str(),
                                  fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(source);
        auto bytes = std::make_unique_for_overwrite<std::byte[]>(kSize);
        for (std::size_t index = 0; index < kSize; ++index) {
            bytes[index] = static_cast<std::byte>(index & 0xff);
        }
        auto write_result = ex::sync_wait(context,
                                io::write_all(context, *source, rbytes{bytes.get(), kSize}));
        REQUIRE(write_result);
    }

    auto source = fs::file::open(source_path.value.c_str(), fs::mode::read);
    auto destination = fs::file::open(destination_path.value.c_str(),
                              fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(source);
    REQUIRE(destination);

    std::uint64_t moved = 0;
    auto result = ex::sync_wait(context, io::pump(context, source->read_handle(), destination->write_handle(),
                                         64 * 1024, &moved));
    REQUIRE(result);
    CHECK(moved == kSize);

    auto verify = fs::file::open(destination_path.value.c_str(), fs::mode::read);
    REQUIRE(verify);
    auto size = verify->size();
    REQUIRE(size);
    CHECK(*size == kSize);
    auto bytes = std::make_unique_for_overwrite<std::byte[]>(kSize);
    auto read_result = ex::sync_wait(context, io::read(context, *verify, wbytes{bytes.get(), kSize}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == kSize);
    for (std::size_t index = 0; index < kSize; ++index) {
        if (bytes[index] != static_cast<std::byte>(index & 0xff)) {
            CHECK_MESSAGE(false, "payload mismatch");
            break;
        }
    }
}

TEST_CASE("pump: file → socket (the sendfile shape)") {
    io_context context;
    temp_path path;
    constexpr std::string_view payload = "sendfile through iox::pump";

    int socket_pair[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, socket_pair) == 0);
    {
        auto file = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(file);
        auto write_result = ex::sync_wait(context, io::write_all(context, *file, as_rbytes(std::span{payload})));
        REQUIRE(write_result);
        auto source = fs::file::open(path.value.c_str(), fs::mode::read);
        REQUIRE(source);

        std::uint64_t moved = 0;
        auto result = ex::sync_wait(context,
                               io::pump(context, source->read_handle(), iox::fd{socket_pair[0]}, 4096, &moved));
        REQUIRE(result);
        CHECK(moved == payload.size());
    }
    ::shutdown(socket_pair[0], SHUT_WR);

    std::string received;
    std::array<std::byte, 128> buffer{};
    for (;;) {
        auto read_result = ex::sync_wait(context, io::read(context, iox::fd{socket_pair[1]}, wbytes{buffer.data(), buffer.size()}));
        REQUIRE(read_result);
        const std::size_t byte_count = std::get<0>(*read_result);
        if (byte_count == 0) {
            break;
        }
        received.append(reinterpret_cast<const char*>(buffer.data()), byte_count);
    }
    CHECK(received == payload);
    ::close(socket_pair[0]);
    ::close(socket_pair[1]);
}

TEST_CASE("pump: a source that cannot splice fails cleanly, nothing moved") {
    io_context context;
    auto sink = pipe::pair::create();
    REQUIRE(sink);
    const int efd = ::eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd >= 0);

    std::uint64_t moved = 0;
    auto result = ex::sync_wait(context, io::pump(context, iox::fd{efd}, sink->w.write_handle(), 4096, &moved));
    REQUIRE_FALSE(result);
    REQUIRE(result.error);
    CHECK(result.error->code() == EINVAL);
    CHECK(moved == 0);

    std::byte raw[8]{};
    auto read_result = ex::sync_wait(context, io::read(context, iox::fd{efd}, wbytes{raw, sizeof(raw)}));
    REQUIRE(read_result);
    CHECK(std::get<0>(*read_result) == 8);
    CHECK(std::to_integer<int>(raw[0]) == 1);
    ::close(efd);
}

TEST_CASE("pump: cancellation stops the pump mid-stream") {
    io_context context;
    auto source = pipe::pair::create();
    auto sink = pipe::pair::create();
    REQUIRE(source);
    REQUIRE(sink);

    ex::inplace_stop_source stop_source;
    int stopped = 0;
    struct stop_counting {
        using receiver_concept = stdexec::receiver_tag;
        ex::inplace_stop_token token;
        int* stopped;
        auto get_env() const noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, token}};
        }
        void set_value() && noexcept {}
        void set_error(iox::error) && noexcept {}
        void set_error(std::exception_ptr) && noexcept {}
        void set_stopped() && noexcept { ++*stopped; }
    };
    auto operation = stdexec::connect(io::pump(context, source->r.read_handle(), sink->w.write_handle()),
                               stop_counting{stop_source.get_token(), &stopped});
    stdexec::start(operation);

    auto canceller = io::sleep_for(context, std::chrono::milliseconds(50)) |
                     ex::then([&] { stop_source.request_stop(); });
    REQUIRE(ex::sync_wait(context, stop_source, canceller));
    context.run_for(std::chrono::milliseconds(100));
    CHECK(stopped == 1);
}
