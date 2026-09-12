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

double cpu_secs() {
    ::rusage ru{};
    (void)!::getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
           ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

std::string tmp_name(const char* tag) {
    return std::string{"/dev/shm/iox_bench_"} + tag + "_" + std::to_string(::getpid());
}

bool prepare(const std::string& path, std::size_t bytes) {
    auto f = fs::file::open(path.c_str(), fs::mode::rw | fs::mode::create | fs::mode::truncate);
    if (!f) {
        return false;
    }
    const auto block = std::make_unique_for_overwrite<std::byte[]>(4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        block[i] = static_cast<std::byte>(i & 0xff);
    }
    for (std::size_t off = 0; off < bytes; off += 4096) {
        const std::size_t n = std::min<std::size_t>(4096, bytes - off);
        if (::pwrite(f->read_handle().v, block.get(), n, static_cast<off_t>(off)) < 0) {
            return false;
        }
    }
    return true;
}

bool verify(const std::string& path, std::size_t bytes) {
    auto f = fs::file::open(path.c_str(), fs::mode::read);
    if (!f) {
        return false;
    }
    const auto block = std::make_unique_for_overwrite<std::byte[]>(4096);
    for (std::size_t off = 0; off < bytes; off += 4096) {
        const std::size_t n = std::min<std::size_t>(4096, bytes - off);
        if (::pread(f->read_handle().v, block.get(), n, static_cast<off_t>(off)) <
            static_cast<::ssize_t>(n)) {
            return false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (block[i] != static_cast<std::byte>((off + i) & 0xff)) {
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

    const std::string src_path = tmp_name("source");
    const std::string dst_path = tmp_name("dest");
    if (!prepare(src_path, bytes)) {
        std::fprintf(stderr, "prepare failed (is /dev/shm large enough?)\n");
        return 1;
    }

    io_context ctx;
    const auto buffer = std::make_unique_for_overwrite<std::byte[]>(kChunk);

    {
        auto source = fs::file::open(src_path.c_str(), fs::mode::read);
        auto dest = fs::file::open(dst_path.c_str(),
                                  fs::mode::rw | fs::mode::create | fs::mode::truncate);
        if (!source || !dest) {
            std::perror("open");
            return 1;
        }
        std::uint64_t moved = 0;
        bool eof = false;
        auto pump = io::loop(ctx, [&]() {
            return io::read(ctx, *source, wbytes{buffer.get(), kChunk})
                 | ex::let_value([&](std::size_t n) {
                       eof = (n == 0);
                       moved += n;
                       return io::write_all(ctx, *dest, rbytes{buffer.get(), n});
                   })
                 | ex::then([&]() { return eof; });
        });
        const auto wall0 = std::chrono::steady_clock::now();
        const double cpu0 = cpu_secs();
        if (!ex::sync_wait(ctx, pump)) {
            std::fprintf(stderr, "userspace pump failed\n");
            return 1;
        }
        const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        const double cpu = cpu_secs() - cpu0;
        std::printf("userspace read/write: %5.0f MiB/s   cpu %6.2fs (%5.0f MiB/cpu-s)\n",
                    moved / 1024.0 / 1024.0 / wall, cpu,
                    cpu > 0 ? moved / 1024.0 / 1024.0 / cpu : 0.0);
    }

    {
        auto source = fs::file::open(src_path.c_str(), fs::mode::read);
        auto dest = fs::file::open(dst_path.c_str(),
                                  fs::mode::rw | fs::mode::create | fs::mode::truncate);
        if (!source || !dest) {
            std::perror("open");
            return 1;
        }
        std::uint64_t moved = 0;
        const auto wall0 = std::chrono::steady_clock::now();
        const double cpu0 = cpu_secs();
        auto r = ex::sync_wait(ctx,
                               io::pump(ctx, source->read_handle(), dest->write_handle(),
                                        kChunk, &moved));
        if (!r) {
            std::fprintf(stderr, "splice pump failed\n");
            return 1;
        }
        const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        const double cpu = cpu_secs() - cpu0;
        std::printf("kernel splice (pump): %5.0f MiB/s   cpu %6.2fs (%5.0f MiB/cpu-s)\n",
                    moved / 1024.0 / 1024.0 / wall, cpu,
                    cpu > 0 ? moved / 1024.0 / 1024.0 / cpu : 0.0);
    }

    const bool ok = verify(dst_path, bytes);
    std::printf("payload: %s\n", ok ? "identical" : "MISMATCH");
    ::unlink(src_path.c_str());
    ::unlink(dst_path.c_str());
    return ok ? 0 : 1;
}
