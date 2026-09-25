// iox — unified async IO for Linux
// examples/file_copy.cc — unified vocabulary, using registered buffers (zero-copy _FIXED paths) and
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include <iox/core/exec.h>
#include <iox/fs/file.h>
#include <iox/core/mr.h>
#include <iox/ops.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s SRC DST\n", argv[0]);
        return 2;
    }

    iox::io_context context;

    auto source = iox::fs::file::open(argv[1], iox::fs::mode::read);
    if (!source) {
        std::fprintf(stderr, "open %s: %s\n", argv[1], source.error().message().c_str());
        return 1;
    }
    auto dest = iox::fs::file::open(argv[2], iox::fs::mode::rw | iox::fs::mode::create |
                                                iox::fs::mode::truncate);
    if (!dest) {
        std::fprintf(stderr, "open %s: %s\n", argv[2], dest.error().message().c_str());
        return 1;
    }

    constexpr std::size_t kChunk = 256 * 1024;
    auto pool = iox::buffer_pool::create(context, kChunk, 2);
    if (!pool) {
        std::fprintf(stderr, "buffer_pool: %s\n", pool.error().message().c_str());
        return 1;
    }

    iox::uoffset_t offset{0};
    std::uint64_t total = 0;
    for (;;) {
        auto slot = pool->take();
        if (!slot) {
            std::fprintf(stderr, "pool: %s\n", slot.error().message().c_str());
            return 1;
        }

        auto read_result = iox::exec::sync_wait(context, iox::io::read_at(context, *source, slot->writable(), offset));
        if (!read_result) {
            std::fprintf(stderr, "read: %s\n", read_result.error->message().c_str());
            return 1;
        }
        const auto count = std::get<0>(*read_result);
        if (count == 0) {
            pool->give_back(*slot);
            break;
        }

        auto write_result = iox::exec::sync_wait(
            context, iox::io::write_at(context, *dest, slot->readable_first(count), offset));
        if (!write_result || std::get<0>(*write_result) != count) {
            std::fprintf(stderr, "write failed\n");
            return 1;
        }

        total += count;
        offset = offset + count;
        pool->give_back(*slot);
    }

    auto fsync_result = iox::exec::sync_wait(context, iox::io::fsync(context, *dest));
    if (!fsync_result) {
        std::fprintf(stderr, "fsync failed\n");
        return 1;
    }

    std::printf("copied %llu bytes in %zu KiB chunks (registered buffers)\n",
                static_cast<unsigned long long>(total), kChunk / 1024);
    return 0;
}
