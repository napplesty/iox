// iox — tests/test_redteam.cc: regression probes for completion, cancel, and teardown edge cases.
#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <iox/compose/detach.h>
#include <iox/compose/pump.h>
#include <iox/compose/write_all.h>
#include <iox/core/exec.h>
#include <iox/driver/completion_source.h>
#include <iox/fs/file.h>
#include <iox/fs/inotify_event.h>
#include <iox/ops.h>
#include <iox/pipe/channel.h>
#include <iox/runtime/blocking_pool.h>

#include "support/receivers.h"

using namespace std::chrono_literals;
using namespace iox;
namespace ex = iox::exec;

using test_rx::stop_counting_receiver;

namespace {

struct temp_path {
    std::string value;

    temp_path() : value("/tmp/iox_redteam_" + std::to_string(::getpid()) + "_" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this))) {}
    ~temp_path() { ::unlink(value.c_str()); }
    temp_path(const temp_path&) = delete;
    temp_path& operator=(const temp_path&) = delete;
};

struct plain_counting_receiver {
    using receiver_concept = stdexec::receiver_tag;
    int* count = nullptr;

    stdexec::env<> get_env() const noexcept { return {}; }
    template <class... As>
    void set_value(As&&...) && noexcept {
        if (count != nullptr) {
            ++*count;
        }
    }
    void set_error(iox::error) && noexcept {}
    void set_error(std::exception_ptr) && noexcept {}
    void set_stopped() && noexcept {
        if (count != nullptr) {
            ++*count;
        }
    }
};

}

TEST_CASE("redteam P0: when_all + cancel completes stopped, never a fabricated success") {
    io_context context;
    ex::inplace_stop_source stop_source;

    auto flow = ex::when_all(
        io::sleep_for(context, 1h),
        io::sleep_for(context, 30ms) | ex::then([&] { stop_source.request_stop(); }));

    const auto start_time = std::chrono::steady_clock::now();
    auto result = ex::sync_wait(context, stop_source, flow);
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    CHECK(elapsed < 1s);
    CHECK_FALSE(result);
    CHECK_FALSE(result.error.has_value());
    CHECK(result.stopped);
}

TEST_CASE("redteam P1: mass cancel far beyond the 64-receipt pool") {
    io_context context;
    ex::inplace_stop_source stop_source;
    int stopped = 0;

    using sleep_op_t = decltype(stdexec::connect(
        io::sleep_for(std::declval<io_context&>(), 1h),
        std::declval<stop_counting_receiver>()));
    std::vector<std::optional<sleep_op_t>> operations;
    operations.reserve(100);
    for (int index = 0; index < 100; ++index) {
        operations.emplace_back(stdexec::connect(io::sleep_for(context, 1h),
                                          stop_counting_receiver{stop_source.get_token(), &stopped, nullptr}));
        stdexec::start(*operations[index]);
    }
    stop_source.request_stop(); // 100 concurrent cancels: the pool must grow, not drop
    context.run_for(300ms);

    CHECK(stopped == 100); // used to be 64 with 36 stranded forever
}

TEST_CASE("redteam P1: an interrupted run_for does not kill a later run()") {
    io_context context;

    int interrupted = 0;
    auto stopper = ex::connect(io::schedule(context) | ex::then([&] {
                                   ++interrupted;
                                   context.stop();
                               }),
                               plain_counting_receiver{&interrupted});
    ex::start(stopper);
    context.run_for(500ms);
    CHECK(interrupted >= 1);

    int later = 0;
    auto timer = ex::connect(io::sleep_for(context, 750ms) | ex::then([&] {
                                 ++later;
                                 context.stop();
                             }),
                             plain_counting_receiver{&later});
    ex::start(timer);
    context.run();
    CHECK(later >= 1);
}

TEST_CASE("redteam P1: nested sync_wait inside a completion does not re-enter dispatch") {
    io_context context;
    int nested = -1;

    ex::detach(io::sleep_for(context, 5ms) | ex::then([&] {
                   auto result = ex::sync_wait(context, io::sleep_for(context, 1ms));
                   nested = result ? 1 : 0;
               }));
    context.run_for(1s);
    CHECK(nested == 1);
}

TEST_CASE("redteam P1: pump into a full sink stays cancellable") {
    io_context context;
    temp_path path;
    {
        auto file = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(file);
        std::array<std::byte, 16384> chunk{};
        for (int index = 0; index < 64; ++index) {
            auto write_result = ex::sync_wait(context,
                                    io::write_all(context, *file, rbytes{chunk.data(), chunk.size()}));
            REQUIRE(write_result);
        }
    }
    auto source = fs::file::open(path.value.c_str(), fs::mode::read);
    auto sink = pipe::pair::create();
    REQUIRE(source);
    REQUIRE(sink);

    ex::inplace_stop_source stop_source;
    int stopped = 0;
    auto operation = stdexec::connect(
        io::pump(context, source->read_handle(), sink->w.write_handle(), 16 * 1024),
        stop_counting_receiver{stop_source.get_token(), &stopped, nullptr});
    stdexec::start(operation);

    auto canceller = io::sleep_for(context, 100ms) | ex::then([&] { stop_source.request_stop(); });
    REQUIRE(ex::sync_wait(context, stop_source, canceller));
    context.run_for(300ms);
    CHECK(stopped == 1); // the wedge used to park the io thread forever
}

TEST_CASE("redteam P2: uoffset_t{UINT64_MAX} is a typed error, not a positional alias") {
    io_context context;
    temp_path path;
    {
        auto file = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(file);
        REQUIRE(::pwrite(file->read_handle().v, "abc", 3, 0) == 3);
    }
    auto file = fs::file::open(path.value.c_str(), fs::mode::read);
    REQUIRE(file);
    ::lseek(file->read_handle().v, 1, SEEK_SET);

    std::array<std::byte, 8> buffer{};
    auto read_result = ex::sync_wait(context, io::read_at(context, *file, wbytes{buffer.data(), buffer.size()},
                                            uoffset_t{UINT64_MAX}));
    REQUIRE_FALSE(read_result);
    REQUIRE(read_result.error);
    CHECK(read_result.error->code() == EOVERFLOW);

    auto write_result = ex::sync_wait(context, io::write_at(context, *file, rbytes{buffer.data(), 0},
                                             uoffset_t{UINT64_MAX}));
    (void)write_result;
}

TEST_CASE("redteam P2: event_range cannot walk past the buffer on a lying length") {
    std::byte buffer[sizeof(::inotify_event)];
    auto* event = reinterpret_cast<::inotify_event*>(buffer);
    event->wd = 1;
    event->mask = 0;
    event->cookie = 0;
    event->len = 1000;

    fs::event_range range{wbytes{buffer, sizeof(buffer)}, sizeof(::inotify_event)};
    int visited = 0;
    for (const auto& entry : range) {
        ++visited;
    }
    CHECK(visited == 1);
}

TEST_CASE("redteam P1: blocking_pool teardown leaves the context usable") {
    io_context context;
    {
        blocking_pool pool(context);
        auto result = ex::sync_wait(context, pool.run([] { return 42; }));
        REQUIRE(result);
        CHECK(std::get<0>(*result) == 42);
    } // pool dies here; the resident wakeup op must retire with it

    auto after = ex::sync_wait(context, io::schedule(context) | ex::then([] { return 7; }));
    REQUIRE(after);
    CHECK(std::get<0>(*after) == 7);
}

TEST_CASE("redteam P2: owning close (rvalue) tolerates handle death and fd reuse") {
    io_context context;
    temp_path path;
    {
        auto file = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(file);
        auto close_result = ex::sync_wait(context, io::close(context, std::move(*file)));
        REQUIRE(close_result);
    } // file is gone; the owning close stole the number, nothing dangles

    auto victim = fs::file::open(path.value.c_str(),
                                 fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(victim);
    context.run_for(50ms);
    CHECK(victim->valid());
    const std::string_view probe = "still open";
    auto write_result = ex::sync_wait(context, io::write_all(context, *victim, as_rbytes(std::span{probe})));
    REQUIRE(write_result);
}

TEST_CASE("redteam P0: an unrelated ctx.stop() does not strand sync_wait's op") {
    io_context context;
    ex::detach(io::sleep_for(context, 30ms) | ex::then([&] { context.stop(); }));

    const auto start_time = std::chrono::steady_clock::now();
    auto result = ex::sync_wait(context, io::sleep_for(context, 150ms));
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    REQUIRE(result);
    CHECK(elapsed >= 140ms);
}

TEST_CASE("redteam P0: raw-fd write to a dead peer is EPIPE, not SIGPIPE") {
    io_context context;
    int socket_pair[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, socket_pair) == 0);
    ::close(socket_pair[1]);

    auto first = ex::sync_wait(context, io::write(context, iox::fd{socket_pair[0]}, rbytes{reinterpret_cast<const std::byte*>("x"), 1}));
    (void)first;
    context.run_for(20ms);
    auto second = ex::sync_wait(context, io::write(context, iox::fd{socket_pair[0]}, rbytes{reinterpret_cast<const std::byte*>("y"), 1}));
    if (!second) { // when the kernel reports it, it must be EPIPE, not death
        REQUIRE(second.error);
        CHECK(second.error->code() == EPIPE);
    }
    ::close(socket_pair[0]);
}

TEST_CASE("redteam followup: sync_wait inside a live batch_scope errors, never spins") {
    io_context context;
    batch_scope scope{context};
    auto result = ex::sync_wait(context, io::sleep_for(context, 1ms));
    REQUIRE_FALSE(result);
    REQUIRE(result.error);
    CHECK(result.error->code() == EDEADLK);
}

TEST_CASE("redteam followup: run_for re-entered from a completion terminates") {
    io_context context;
    int inner = 0;
    ex::detach(io::sleep_for(context, 5ms) | ex::then([&] {
        ++inner;
        context.run_for(5ms);
    }));
    const auto start_time = std::chrono::steady_clock::now();
    context.run_for(2s);
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    CHECK(inner == 1);
    CHECK(elapsed < 1s); // ended early via the nested stop — never past 2s
    auto result = ex::sync_wait(context, io::sleep_for(context, 1ms));
    REQUIRE(result);
}

TEST_CASE("redteam followup: splice/tee beyond 4GiB is EOVERFLOW, never a silent short move") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);

    auto splice_result = ex::sync_wait(context, io::splice(context, pair->r.read_handle(), pair->w.write_handle(),
                                           std::size_t{1} << 32));
    REQUIRE_FALSE(splice_result);
    REQUIRE(splice_result.error);
    CHECK(splice_result.error->code() == EOVERFLOW);

    auto tee_result = ex::sync_wait(context, io::tee(context, pair->r.read_handle(), pair->w.write_handle(),
                                        std::size_t{1} << 32));
    REQUIRE_FALSE(tee_result);
    REQUIRE(tee_result.error);
    CHECK(tee_result.error->code() == EOVERFLOW);
}

TEST_CASE("redteam followup: pump clamps an out-of-range chunk, still moves every byte") {
    io_context context;
    temp_path source_path;
    temp_path destination_path;
    constexpr std::string_view payload = "chunk clamp payload";
    {
        auto file = fs::file::open(source_path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(file);
        REQUIRE(ex::sync_wait(context, io::write_all(context, *file, as_rbytes(std::span{payload}))));
    }
    auto source = fs::file::open(source_path.value.c_str(), fs::mode::read);
    auto destination = fs::file::open(destination_path.value.c_str(),
                               fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(source);
    REQUIRE(destination);

    std::uint64_t moved = 0;
    auto result = ex::sync_wait(context, io::pump(context, source->read_handle(), destination->write_handle(),
                                         std::size_t{1} << 32, &moved)); // used to move 1 byte
    REQUIRE(result);
    CHECK(moved == payload.size());
}

TEST_CASE("redteam followup: a completion source whose fd is dead retires, never spins") {
    io_context context;
    struct dead_fd_source final : completion_source {
        int raw;
        explicit dead_fd_source(int raw) : raw(raw) {}
        iox::fd completion_fd() const noexcept override { return iox::fd{raw}; }
    };
    const int raw = ::eventfd(0, EFD_CLOEXEC);
    REQUIRE(raw >= 0);
    ::close(raw); // arm-after-close: every poll_add submission fails with EBADF at once

    dead_fd_source source{raw};
    context.attach_source(source);
    context.run_for(50ms);
    CHECK_FALSE(context.source_attached(source)); // used to re-arm forever at 100% CPU
}

TEST_CASE("redteam followup: a busy source re-attached across a pending cancel keeps ticking") {
    io_context context;
    struct busy_source final : completion_source {
        int ready_calls = 0;
        bool has_work() const noexcept override { return true; }
        void on_ready(io_context&) noexcept override { ++ready_calls; }
    } source;

    context.attach_source(source); // tick armed and in flight
    context.detach_source(source); // cancel submitted while the tick is in flight
    context.attach_source(source); // re-arm skipped (still in flight), only the CQE can revive it
    context.run_for(50ms);
    CHECK(source.ready_calls > 0); // used to starve: the canceled tick was never re-armed
}

TEST_CASE("redteam followup: run() on a failed ring returns instead of dereferencing it") {
    io_context context{uring::ring_params{.entries = 0}};
    REQUIRE_FALSE(context.ok());
    context.run(); // used to walk a zero-initialized ring and crash
    CHECK(context.run_for(1ms) == iox::error{});
}

TEST_CASE("redteam followup: run() restarts itself like run_for and sync_wait do") {
    io_context context;
    int fired = 0;
    auto operation = stdexec::connect(io::sleep_for(context, 1ms) | ex::then([&] {
                                   ++fired;
                                   context.stop();
                               }),
                               plain_counting_receiver{&fired});
    stdexec::start(operation);
    context.stop(); // a stop requested before run() must not veto it
    context.run();
    CHECK(fired >= 1);
}

TEST_CASE("redteam followup: run_for inside a live batch_scope reports EDEADLK") {
    io_context context;
    batch_scope scope{context};
    CHECK(context.run_for(1ms) == iox::error::from_errno(EDEADLK));
}

TEST_CASE("redteam followup: a negative sleep fires immediately, never EINVAL") {
    io_context context;
    auto result = ex::sync_wait(context, io::sleep_for(context, -5ms));
    REQUIRE(result);
    auto until_result = ex::sync_wait(context, io::sleep_until(context, std::chrono::steady_clock::now() - 1s));
    REQUIRE(until_result);
}

TEST_CASE("redteam followup: write_policy honors an injected stall error") {
    io_context context;
    auto pair = pipe::pair::create();
    REQUIRE(pair);
    auto result = ex::sync_wait(
        context, io::detail::fd_sender<io::detail::write_policy>{
                 &context, pair->w.write_handle(),
                 {rbytes{reinterpret_cast<const std::byte*>("x"), 1}, false, EIO}});
    REQUIRE_FALSE(result);
    REQUIRE(result.error);
    CHECK(result.error->code() == EIO);
}

TEST_CASE("redteam followup: a blocking_pool without workers fails fast, never hangs") {
    io_context context;
    blocking_pool pool(context, 0);
    auto result = ex::sync_wait(context, pool.run([] { return 1; }));
    REQUIRE_FALSE(result);
    REQUIRE(result.error);
}
