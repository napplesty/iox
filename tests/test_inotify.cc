// Integration tests for fs::watcher (inotify) + fs::event_range: directory
// events through the ordinary io::read vocabulary.
#include <doctest/doctest.h>

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include <iox/core/exec.h>
#include <iox/fs/inotify_event.h>
#include <iox/fs/watcher.h>
#include <iox/ops.h>

using namespace iox;
namespace ex = iox::exec;

namespace {

struct temp_dir {
    std::string value;

    temp_dir() : value("/tmp/iox_inotify_" + std::to_string(::getpid()) + "_" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this))) {
        ::mkdir(value.c_str(), 0700);
    }
    ~temp_dir() {
        ::rmdir(value.c_str());
    }
    temp_dir(const temp_dir&) = delete;
    temp_dir& operator=(const temp_dir&) = delete;
};

/// One read of everything currently queued. Names are COPIED out: fs::event
/// holds string_views into the read buffer, which dies with this frame.
struct recorded {
    std::uint32_t mask = 0;
    std::string name;
};

std::vector<recorded> read_events(io_context& ctx, fs::watcher& w) {
    std::array<std::byte, 4096> buf{};
    auto r = ex::sync_wait(ctx, io::read(ctx, w, wbytes{buf.data(), buf.size()}));
    REQUIRE(r);
    const std::size_t n = std::get<0>(*r);
    std::vector<recorded> out;
    for (const auto& e : fs::event_range{wbytes{buf.data(), buf.size()}, n}) {
        out.push_back({e.mask, std::string{e.name}});
    }
    return out;
}

} // namespace

TEST_CASE("fs::watcher: create, modify and delete carry the name") {
    io_context ctx;
    temp_dir dir;
    auto w = fs::watcher::create();
    REQUIRE(w);
    auto wd = w->add(dir.value, IN_CREATE | IN_MODIFY | IN_DELETE);
    REQUIRE(wd);

    { // create + modify: one buffered write queues both
        FILE* f = std::fopen((dir.value + "/test.txt").c_str(), "w");
        REQUIRE(f != nullptr);
        std::fputs("hello", f);
        std::fclose(f);
    }
    auto events = read_events(ctx, *w);
    REQUIRE_FALSE(events.empty());
    bool saw_create = false;
    bool saw_modify = false;
    for (const auto& e : events) {
        if (e.name == "test.txt") {
            saw_create = saw_create || (e.mask & IN_CREATE);
            saw_modify = saw_modify || (e.mask & IN_MODIFY);
        }
    }
    CHECK(saw_create);
    CHECK(saw_modify);

    CHECK(::unlink((dir.value + "/test.txt").c_str()) == 0);
    events = read_events(ctx, *w);
    REQUIRE(events.size() >= 1);
    CHECK((events.back().mask & IN_DELETE) != 0);
    CHECK(events.back().name == "test.txt");
}

TEST_CASE("fs::watcher: IN_IGNORED arrives when the watched dir is removed") {
    io_context ctx;
    temp_dir dir; // removes itself at scope exit, after our read below
    auto w = fs::watcher::create();
    REQUIRE(w);
    REQUIRE(w->add(dir.value, IN_CREATE));

    ::rmdir(dir.value.c_str());
    dir.value.clear(); // don't double-rmdir

    auto events = read_events(ctx, *w);
    REQUIRE_FALSE(events.empty());
    CHECK((events.back().mask & IN_IGNORED) != 0);
}

TEST_CASE("fs::event_range: empty range for a zero-length read") {
    std::byte buf[16];
    fs::event_range none{wbytes{buf, sizeof(buf)}, 0};
    CHECK(none.empty());
    CHECK(none.size() == 0);
}
