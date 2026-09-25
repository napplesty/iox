// iox — unified async IO for Linux
// tests/test_inotify.cc — events through the ordinary io::read vocabulary.
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

struct recorded {
    std::uint32_t mask = 0;
    std::string name;
};

std::vector<recorded> read_events(io_context& context, fs::watcher& watcher) {
    std::array<std::byte, 4096> buffer{};
    auto result = ex::sync_wait(context, io::read(context, watcher, wbytes{buffer.data(), buffer.size()}));
    REQUIRE(result);
    const std::size_t byte_count = std::get<0>(*result);
    std::vector<recorded> events;
    for (const auto& event : fs::event_range{wbytes{buffer.data(), buffer.size()}, byte_count}) {
        events.push_back({event.mask, std::string{event.name}});
    }
    return events;
}

}

TEST_CASE("fs::watcher: create, modify and delete carry the name") {
    io_context context;
    temp_dir dir;
    auto watcher = fs::watcher::create();
    REQUIRE(watcher);
    auto wd = watcher->add(dir.value, IN_CREATE | IN_MODIFY | IN_DELETE);
    REQUIRE(wd);

    {
        FILE* file = std::fopen((dir.value + "/test.txt").c_str(), "w");
        REQUIRE(file != nullptr);
        std::fputs("hello", file);
        std::fclose(file);
    }
    auto events = read_events(context, *watcher);
    REQUIRE_FALSE(events.empty());
    bool saw_create = false;
    bool saw_modify = false;
    for (const auto& event : events) {
        if (event.name == "test.txt") {
            saw_create = saw_create || (event.mask & IN_CREATE);
            saw_modify = saw_modify || (event.mask & IN_MODIFY);
        }
    }
    CHECK(saw_create);
    CHECK(saw_modify);

    CHECK(::unlink((dir.value + "/test.txt").c_str()) == 0);
    events = read_events(context, *watcher);
    REQUIRE(events.size() >= 1);
    CHECK((events.back().mask & IN_DELETE) != 0);
    CHECK(events.back().name == "test.txt");
}

TEST_CASE("fs::watcher: IN_IGNORED arrives when the watched dir is removed") {
    io_context context;
    temp_dir dir;
    auto watcher = fs::watcher::create();
    REQUIRE(watcher);
    REQUIRE(watcher->add(dir.value, IN_CREATE));

    ::rmdir(dir.value.c_str());
    dir.value.clear();

    auto events = read_events(context, *watcher);
    REQUIRE_FALSE(events.empty());
    CHECK((events.back().mask & IN_IGNORED) != 0);
}

TEST_CASE("fs::event_range: empty range for a zero-length read") {
    std::byte buffer[16];
    fs::event_range none{wbytes{buffer, sizeof(buffer)}, 0};
    CHECK(none.empty());
    CHECK(none.size() == 0);
}
