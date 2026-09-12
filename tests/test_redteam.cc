// iox — unified async IO for Linux
// tests/test_redteam.cc — completion-signature
#include <doctest/doctest.h>

#include <fcntl.h>
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
#include <iox/fs/file.h>
#include <iox/fs/inotify_event.h>
#include <iox/ops.h>
#include <iox/pipe/channel.h>
#include <iox/runtime/blocking_pool.h>

using namespace std::chrono_literals;
using namespace iox;
namespace ex = iox::exec;

namespace {

struct temp_path {
    std::string value;

    temp_path() : value("/tmp/iox_redteam_" + std::to_string(::getpid()) + "_" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this))) {}
    ~temp_path() { ::unlink(value.c_str()); }
    temp_path(const temp_path&) = delete;
    temp_path& operator=(const temp_path&) = delete;
};

struct stop_counting_receiver {
    using receiver_concept = stdexec::receiver_tag;
    ex::inplace_stop_token tok;
    int* stopped = nullptr;
    int* values = nullptr;

    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}};
    }
    template <class... As>
    void set_value(As&&...) && noexcept {
        if (values != nullptr) {
            ++*values;
        }
    }
    void set_error(iox::error) && noexcept {
        if (values != nullptr) {
            ++*values;
        }
    }
    void set_error(std::exception_ptr) && noexcept {
        if (values != nullptr) {
            ++*values;
        }
    }
    void set_stopped() && noexcept {
        if (stopped != nullptr) {
            ++*stopped;
        }
    }
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
    io_context ctx;
    ex::inplace_stop_source src;

    auto flow = ex::when_all(
        io::sleep_for(ctx, 1h),
        io::sleep_for(ctx, 30ms) | ex::then([&] { src.request_stop(); }));

    const auto t0 = std::chrono::steady_clock::now();
    auto r = ex::sync_wait(ctx, src, flow);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(elapsed < 1s);
    CHECK_FALSE(r);
    CHECK_FALSE(r.error.has_value());
    CHECK(r.stopped);
}

TEST_CASE("redteam P1: mass cancel far beyond the 64-receipt pool") {
    io_context ctx;
    ex::inplace_stop_source src;
    int stopped = 0;

    using sleep_op_t = decltype(stdexec::connect(
        io::sleep_for(std::declval<io_context&>(), 1h),
        std::declval<stop_counting_receiver>()));
    std::vector<std::optional<sleep_op_t>> ops;
    ops.reserve(100);
    for (int i = 0; i < 100; ++i) {
        ops.emplace_back(stdexec::connect(io::sleep_for(ctx, 1h),
                                          stop_counting_receiver{src.get_token(), &stopped, nullptr}));
        stdexec::start(*ops[i]);
    }
    src.request_stop(); // 100 concurrent cancels: the pool must grow, not drop
    ctx.run_for(300ms);

    CHECK(stopped == 100); // used to be 64 with 36 stranded forever
}

TEST_CASE("redteam P1: an interrupted run_for does not kill a later run()") {
    io_context ctx;

    int interrupted = 0;
    auto stopper = ex::connect(io::schedule(ctx) | ex::then([&] {
                                   ++interrupted;
                                   ctx.stop();
                               }),
                               plain_counting_receiver{&interrupted});
    ex::start(stopper);
    ctx.run_for(500ms);
    CHECK(interrupted >= 1);

    int later = 0;
    auto timer = ex::connect(io::sleep_for(ctx, 750ms) | ex::then([&] {
                                 ++later;
                                 ctx.stop();
                             }),
                             plain_counting_receiver{&later});
    ex::start(timer);
    ctx.run();
    CHECK(later >= 1);
}

TEST_CASE("redteam P1: nested sync_wait inside a completion does not re-enter dispatch") {
    io_context ctx;
    int nested = -1;

    ex::detach(io::sleep_for(ctx, 5ms) | ex::then([&] {
                   auto r = ex::sync_wait(ctx, io::sleep_for(ctx, 1ms));
                   nested = r ? 1 : 0;
               }));
    ctx.run_for(1s);
    CHECK(nested == 1);
}

TEST_CASE("redteam P1: pump into a full sink stays cancellable") {
    io_context ctx;
    temp_path path;
    {
        auto f = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(f);
        std::array<std::byte, 16384> chunk{};
        for (int i = 0; i < 64; ++i) {
            auto wr = ex::sync_wait(ctx,
                                    io::write_all(ctx, *f, rbytes{chunk.data(), chunk.size()}));
            REQUIRE(wr);
        }
    }
    auto src = fs::file::open(path.value.c_str(), fs::mode::read);
    auto sink = pipe::pair::create();
    REQUIRE(src);
    REQUIRE(sink);

    ex::inplace_stop_source src_stop;
    int stopped = 0;
    auto op = stdexec::connect(
        io::pump(ctx, src->read_handle(), sink->w.write_handle(), 16 * 1024),
        stop_counting_receiver{src_stop.get_token(), &stopped, nullptr});
    stdexec::start(op);

    auto canceller = io::sleep_for(ctx, 100ms) | ex::then([&] { src_stop.request_stop(); });
    REQUIRE(ex::sync_wait(ctx, src_stop, canceller));
    ctx.run_for(300ms);
    CHECK(stopped == 1); // the wedge used to park the io thread forever
}

TEST_CASE("redteam P2: uoffset_t{UINT64_MAX} is a typed error, not a positional alias") {
    io_context ctx;
    temp_path path;
    {
        auto f = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(f);
        REQUIRE(::pwrite(f->read_handle().v, "abc", 3, 0) == 3);
    }
    auto f = fs::file::open(path.value.c_str(), fs::mode::read);
    REQUIRE(f);
    ::lseek(f->read_handle().v, 1, SEEK_SET);

    std::array<std::byte, 8> buf{};
    auto r = ex::sync_wait(ctx, io::read_at(ctx, *f, wbytes{buf.data(), buf.size()},
                                            uoffset_t{UINT64_MAX}));
    REQUIRE_FALSE(r);
    REQUIRE(r.error);
    CHECK(r.error->code() == EOVERFLOW);

    auto w = ex::sync_wait(ctx, io::write_at(ctx, *f, rbytes{buf.data(), 0},
                                             uoffset_t{UINT64_MAX}));
    (void)w;
}

TEST_CASE("redteam P2: event_range cannot walk past the buffer on a lying length") {
    std::byte buf[sizeof(::inotify_event)];
    auto* ev = reinterpret_cast<::inotify_event*>(buf);
    ev->wd = 1;
    ev->mask = 0;
    ev->cookie = 0;
    ev->len = 1000;

    fs::event_range range{wbytes{buf, sizeof(buf)}, sizeof(::inotify_event)};
    int visited = 0;
    for (const auto& e : range) {
        ++visited;
    }
    CHECK(visited == 1);
}

TEST_CASE("redteam P1: blocking_pool teardown leaves the context usable") {
    io_context ctx;
    {
        blocking_pool pool(ctx);
        auto r = ex::sync_wait(ctx, pool.run([] { return 42; }));
        REQUIRE(r);
        CHECK(std::get<0>(*r) == 42);
    } // pool dies here; the resident wakeup op must retire with it

    auto after = ex::sync_wait(ctx, io::schedule(ctx) | ex::then([] { return 7; }));
    REQUIRE(after);
    CHECK(std::get<0>(*after) == 7);
}

TEST_CASE("redteam P2: owning close (rvalue) tolerates handle death and fd reuse") {
    io_context ctx;
    temp_path path;
    {
        auto f = fs::file::open(path.value.c_str(),
                                fs::mode::rw | fs::mode::create | fs::mode::truncate);
        REQUIRE(f);
        auto r = ex::sync_wait(ctx, io::close(ctx, std::move(*f)));
        REQUIRE(r);
    } // f is gone; the owning close stole the number, nothing dangles

    auto victim = fs::file::open(path.value.c_str(),
                                 fs::mode::rw | fs::mode::create | fs::mode::truncate);
    REQUIRE(victim);
    ctx.run_for(50ms);
    CHECK(victim->valid());
    const std::string_view probe = "still open";
    auto wr = ex::sync_wait(ctx, io::write_all(ctx, *victim, as_rbytes(std::span{probe})));
    REQUIRE(wr);
}

TEST_CASE("redteam P0: an unrelated ctx.stop() does not strand sync_wait's op") {
    io_context ctx;
    ex::detach(io::sleep_for(ctx, 30ms) | ex::then([&] { ctx.stop(); }));

    const auto t0 = std::chrono::steady_clock::now();
    auto r = ex::sync_wait(ctx, io::sleep_for(ctx, 150ms));
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE(r);
    CHECK(elapsed >= 140ms);
}

TEST_CASE("redteam P0: raw-fd write to a dead peer is EPIPE, not SIGPIPE") {
    io_context ctx;
    int sv[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0);
    ::close(sv[1]);

    auto first = ex::sync_wait(ctx, io::write(ctx, iox::fd{sv[0]}, rbytes{reinterpret_cast<const std::byte*>("x"), 1}));
    (void)first;
    ctx.run_for(20ms);
    auto second = ex::sync_wait(ctx, io::write(ctx, iox::fd{sv[0]}, rbytes{reinterpret_cast<const std::byte*>("y"), 1}));
    if (!second) { // when the kernel reports it, it must be EPIPE, not death
        REQUIRE(second.error);
        CHECK(second.error->code() == EPIPE);
    }
    ::close(sv[0]);
}

TEST_CASE("redteam followup: sync_wait inside a live batch_scope errors, never spins") {
    io_context ctx;
    batch_scope scope{ctx};
    auto r = ex::sync_wait(ctx, io::sleep_for(ctx, 1ms));
    REQUIRE_FALSE(r);
    REQUIRE(r.error);
    CHECK(r.error->code() == EDEADLK);
}

TEST_CASE("redteam followup: run_for re-entered from a completion terminates") {
    io_context ctx;
    int inner = 0;
    ex::detach(io::sleep_for(ctx, 5ms) | ex::then([&] {
        ++inner;
        ctx.run_for(5ms);
    }));
    const auto t0 = std::chrono::steady_clock::now();
    ctx.run_for(2s);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(inner == 1);
    CHECK(elapsed < 1s); // ended early via the nested stop — never past 2s
    auto s = ex::sync_wait(ctx, io::sleep_for(ctx, 1ms));
    REQUIRE(s);
}
