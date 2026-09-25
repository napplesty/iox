// iox — unified async IO for Linux
// bench/splice.cc — tmpfs→tmpfs twice, once with the userspace read/write loop and once with
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <sys/resource.h>
#include <unistd.h>

#include <iox/compose/pump.h>
#include <iox/compose/write_all.h>
#include <iox/core/buffer.h>
#include <iox/core/exec.h>
#include <iox/fs/file.h>
#include <iox/ops.h>

using namespace iox;
namespace ex = iox::exec;

namespace {

double cpu_seconds() {
    ::rusage usage{};
    (void)!::getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

std::string temp_path(const char* tag) {
    return std::string{"/dev/shm/iox_bench_"} + tag + "_" + std::to_string(::getpid());
}

bool prepare(const std::string& path, std::size_t bytes) {
    auto file = fs::file::open(path.c_str(), fs::mode::rw | fs::mode::create | fs::mode::truncate);
    if (!file) {
        return false;
    }
    const auto block = std::make_unique_for_overwrite<std::byte[]>(4096);
    for (std::size_t index = 0; index < 4096; ++index) {
        block[index] = static_cast<std::byte>(index & 0xff);
    }
    for (std::size_t offset = 0; offset < bytes; offset += 4096) {
        const std::size_t count = std::min<std::size_t>(4096, bytes - offset);
        if (::pwrite(file->read_handle().v, block.get(), count, static_cast<off_t>(offset)) < 0) {
            return false;
        }
    }
    return true;
}

bool verify(const std::string& path, std::size_t bytes) {
    auto file = fs::file::open(path.c_str(), fs::mode::read);
    if (!file) {
        return false;
    }
    const auto block = std::make_unique_for_overwrite<std::byte[]>(4096);
    for (std::size_t offset = 0; offset < bytes; offset += 4096) {
        const std::size_t count = std::min<std::size_t>(4096, bytes - offset);
        if (::pread(file->read_handle().v, block.get(), count, static_cast<off_t>(offset)) <
            static_cast<::ssize_t>(count)) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (block[index] != static_cast<std::byte>((offset + index) & 0xff)) {
                return false;
            }
        }
    }
    return true;
}

}

int main(int argc, char** argv) {
    const std::size_t mib = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 256;
    const std::size_t bytes = mib * 1024 * 1024;
    constexpr std::size_t kChunk = 256 * 1024;

    const std::string source_path = temp_path("source");
    const std::string dest_path = temp_path("dest");
    if (!prepare(source_path, bytes)) {
        std::fprintf(stderr, "prepare failed (is /dev/shm large enough?)\n");
        return 1;
    }

    io_context context;
    const auto buffer = std::make_unique_for_overwrite<std::byte[]>(kChunk);

    {
        auto source = fs::file::open(source_path.c_str(), fs::mode::read);
        auto dest = fs::file::open(dest_path.c_str(),
                                  fs::mode::rw | fs::mode::create | fs::mode::truncate);
        if (!source || !dest) {
            std::perror("open");
            return 1;
        }
        std::uint64_t moved = 0;
        bool eof = false;
        auto pump = io::loop(context, [&]() {
            return io::read(context, *source, wbytes{buffer.get(), kChunk})
                 | ex::let_value([&](std::size_t count) {
                       eof = (count == 0);
                       moved += count;
                       return io::write_all(context, *dest, rbytes{buffer.get(), count});
                   })
                 | ex::then([&]() { return eof; });
        });
        const auto wall_start = std::chrono::steady_clock::now();
        const double cpu_start = cpu_seconds();
        if (!ex::sync_wait(context, pump)) {
            std::fprintf(stderr, "userspace pump failed\n");
            return 1;
        }
        const auto wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        const double cpu_used = cpu_seconds() - cpu_start;
        std::printf("userspace read/write: %5.0f MiB/s   cpu %6.2fs (%5.0f MiB/cpu-s)\n",
                    moved / 1024.0 / 1024.0 / wall_seconds, cpu_used,
                    cpu_used > 0 ? moved / 1024.0 / 1024.0 / cpu_used : 0.0);
    }

    {
        auto source = fs::file::open(source_path.c_str(), fs::mode::read);
        auto dest = fs::file::open(dest_path.c_str(),
                                  fs::mode::rw | fs::mode::create | fs::mode::truncate);
        if (!source || !dest) {
            std::perror("open");
            return 1;
        }
        std::uint64_t moved = 0;
        const auto wall_start = std::chrono::steady_clock::now();
        const double cpu_start = cpu_seconds();
        auto result = ex::sync_wait(context,
                                    io::pump(context, source->read_handle(), dest->write_handle(),
                                             kChunk, &moved));
        if (!result) {
            std::fprintf(stderr, "splice pump failed\n");
            return 1;
        }
        const auto wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        const double cpu_used = cpu_seconds() - cpu_start;
        std::printf("kernel splice (pump): %5.0f MiB/s   cpu %6.2fs (%5.0f MiB/cpu-s)\n",
                    moved / 1024.0 / 1024.0 / wall_seconds, cpu_used,
                    cpu_used > 0 ? moved / 1024.0 / 1024.0 / cpu_used : 0.0);
    }

    const bool ok = verify(dest_path, bytes);
    std::printf("payload: %s\n", ok ? "identical" : "MISMATCH");
    ::unlink(source_path.c_str());
    ::unlink(dest_path.c_str());
    return ok ? 0 : 1;
}
