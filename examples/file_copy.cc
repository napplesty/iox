// file_copy — M2 acceptance example: chunked async file copy through the
// unified vocabulary, using registered buffers (zero-copy _FIXED paths) and
// positional IO. The loop is driven one chunk at a time via sync_wait;
// pipeline depth arrives with the loop combinators in M3+.
//
//     ./build/file_copy SRC DST
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

    iox::io_context ctx;

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
    auto pool = iox::buffer_pool::create(ctx, kChunk, 2);
    if (!pool) {
        std::fprintf(stderr, "buffer_pool: %s\n", pool.error().message().c_str());
        return 1;
    }

    iox::uoffset_t off{0};
    std::uint64_t total = 0;
    for (;;) {
        auto slot = pool->take();
        if (!slot) {
            std::fprintf(stderr, "pool: %s\n", slot.error().message().c_str());
            return 1;
        }

        auto rd = iox::exec::sync_wait(ctx, iox::io::read_at(ctx, *source, slot->writable(), off));
        if (!rd) {
            std::fprintf(stderr, "read: %s\n", rd.error->message().c_str());
            return 1;
        }
        const auto n = std::get<0>(*rd);
        if (n == 0) {
            pool->give_back(*slot);
            break;
        }

        auto wr = iox::exec::sync_wait(
            ctx, iox::io::write_at(ctx, *dest, slot->readable_first(n), off));
        if (!wr || std::get<0>(*wr) != n) {
            std::fprintf(stderr, "write failed\n");
            return 1;
        }

        total += n;
        off = off + n;
        pool->give_back(*slot);
    }

    auto sync = iox::exec::sync_wait(ctx, iox::io::fsync(ctx, *dest));
    if (!sync) {
        std::fprintf(stderr, "fsync failed\n");
        return 1;
    }

    std::printf("copied %llu bytes in %zu KiB chunks (registered buffers)\n",
                static_cast<unsigned long long>(total), kChunk / 1024);
    return 0;
}
