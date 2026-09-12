// iox — unified async IO for Linux
// bench/file_rw.cc — iox vocabulary (registered buffers, positional IO, batched submission)
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include <iox/core/exec.h>
#include <iox/runtime/io_context.h>
#include <iox/core/mr.h>
#include <iox/ops.h>

using namespace iox;
using namespace std::chrono_literals;

namespace {

using clock_t_ = std::chrono::steady_clock;
constexpr std::size_t kDepth = 64;

struct temp_file {
    std::string path;
    int raw = -1;
    explicit temp_file() : path("/tmp/iox_bench_" + std::to_string(::getpid())) {
        raw = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    }
    ~temp_file() {
        if (raw >= 0) {
            ::close(raw);
        }
        ::unlink(path.c_str());
    }
    temp_file(const temp_file&) = delete;
    temp_file& operator=(const temp_file&) = delete;
};

double mib_per_s(std::uint64_t bytes, clock_t_::duration d) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0) /
           std::chrono::duration<double>(d).count();
}

template <bool IsWrite>
struct file_window;

template <bool IsWrite>
struct refill_receiver {
    using receiver_concept = stdexec::receiver_tag;
    file_window<IsWrite>* w = nullptr;
    std::size_t slot = 0;

    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token,
                                          stdexec::inplace_stop_token{}}};
    }
    template <class... As>
    void set_value(As&&...) && noexcept { w->on_complete(slot); }
    void set_error(iox::error) && noexcept { w->on_complete(slot); }
    void set_error(std::exception_ptr) && noexcept { w->on_complete(slot); }
    void set_stopped() && noexcept { w->on_complete(slot); }
};

template <bool IsWrite>
struct file_window {
    using op_t = std::conditional_t<
        IsWrite,
        decltype(stdexec::connect(io::write_at(std::declval<io_context&>(), iox::fd{},
                                               std::declval<iox::rbytes>(), uoffset_t{0}),
                                  refill_receiver<true>{})),
        decltype(stdexec::connect(io::read_at(std::declval<io_context&>(), iox::fd{},
                                              std::declval<iox::wbytes>(), uoffset_t{0}),
                                  refill_receiver<false>{}))>;

    io_context& ctx;
    buffer_pool& pool;
    iox::fd f;
    std::size_t chunk = 0;
    std::size_t n_chunks = 0;
    std::size_t next = 0;
    std::uint64_t count = 0;
    std::vector<std::optional<op_t>> slots;
    std::vector<std::optional<registered_buffer>> bufs;

    void arm(std::size_t chunk_idx, std::size_t slot) {
        auto b = pool.take();
        if (!b) {
            return;
        }
        bufs[slot] = *b;
        const uoffset_t off{static_cast<std::uint64_t>(chunk_idx) * chunk};
        if constexpr (IsWrite) {
            slots[slot].emplace(stdexec::connect(
                io::write_at(ctx, f, b->readable(), off),
                refill_receiver<true>{this, slot}));
        } else {
            slots[slot].emplace(stdexec::connect(
                io::read_at(ctx, f, b->writable(), off),
                refill_receiver<false>{this, slot}));
        }
        stdexec::start(*slots[slot]);
    }

    void on_complete(std::size_t slot) {
        ++count;
        if (bufs[slot]) {
            pool.give_back(*bufs[slot]);
            bufs[slot].reset();
        }
        if (next < n_chunks) {
            arm(next++, slot);
        } else if (count == n_chunks) {
            ctx.stop();
        }
    }
};

template <bool IsWrite>
double run_iox(io_context& ctx, int raw_fd, buffer_pool& pool, std::size_t chunk,
               std::size_t n_chunks) {
    file_window<IsWrite> w{ctx, pool, iox::fd{raw_fd}, chunk, n_chunks, 0, 0, {}, {}};
    w.slots.resize(kDepth);
    w.bufs.resize(kDepth);

    const auto t0 = clock_t_::now();
    {
        batch_scope scope{ctx};
        for (std::size_t i = 0; i < kDepth && i < n_chunks; ++i) {
            w.arm(w.next++, i);
        }
    }
    ctx.run_for(60s);
    const auto t1 = clock_t_::now();

    if (w.count != n_chunks) {
        return -1.0;
    }
    return mib_per_s(static_cast<std::uint64_t>(chunk) * n_chunks, t1 - t0);
}

template <bool IsWrite>
double run_raw(uring::ring& ring, int raw_fd, std::byte* buf, std::size_t chunk,
               std::size_t n_chunks) {
    struct raw_op final : op_base {
        static void thunk(op_base*, io_context&, std::int32_t, std::uint32_t) noexcept {}
        raw_op() noexcept : op_base(&raw_op::thunk) {}
    };
    std::vector<raw_op> ops(kDepth);
    std::vector<std::byte*> bufs(kDepth);
    for (auto& b : bufs) {
        b = buf + (&b - bufs.data()) * chunk;
    }

    std::size_t next = 0, reaped = 0, inflight = 0;
    const auto t0 = clock_t_::now();
    while (reaped < n_chunks) {
        while (inflight < kDepth && next < n_chunks) {
            io_uring_sqe* sqe = ring.next_sqe();
            if (sqe == nullptr) {
                ring.flush();
                sqe = ring.next_sqe();
                if (sqe == nullptr) break;
            }
            const auto off = static_cast<std::int64_t>(next) *
                             static_cast<std::int64_t>(chunk);
            if constexpr (IsWrite) {
                ::io_uring_prep_write(sqe, raw_fd, bufs[next % kDepth], chunk, off);
            } else {
                ::io_uring_prep_read(sqe, raw_fd, bufs[next % kDepth], chunk, off);
            }
            ::io_uring_sqe_set_data(sqe, &ops[inflight]);
            ++inflight;
            ++next;
        }
        ring.flush_and_wait(1);
        const unsigned n = ring.for_each_cqe([](io_uring_cqe*) {});
        reaped += n;
        inflight -= n;
    }
    const auto t1 = clock_t_::now();
    return mib_per_s(static_cast<std::uint64_t>(chunk) * n_chunks, t1 - t0);
}

}

int main(int argc, char** argv) {
    const std::uint64_t mib = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 128;
    const std::size_t chunk_kib = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 64;

    const std::size_t chunk = chunk_kib * 1024;
    const std::size_t n_chunks = static_cast<std::size_t>(mib) * 1024 * 1024 / chunk;

    io_context ctx{uring::ring_params{.entries = 256, .cq_entries = 4096}};
    if (!ctx.ok()) {
        std::fprintf(stderr, "ring init failed\n");
        return 1;
    }
    auto pool = buffer_pool::create(ctx, chunk, kDepth);
    if (!pool) {
        std::fprintf(stderr, "pool: %s\n", pool.error().message().c_str());
        return 1;
    }

    uring::ring raw_ring{uring::ring_params{.entries = 256, .cq_entries = 4096}};
    if (!raw_ring.ok()) {
        return 1;
    }
    auto* raw_buf = static_cast<std::byte*>(
        ::operator new(chunk * kDepth, std::align_val_t{4096}));
    ::memset(raw_buf, 0xAB, chunk * kDepth);

    temp_file f;
    if (f.raw < 0) {
        std::fprintf(stderr, "temp file failed\n");
        return 1;
    }

    std::printf("sequential file IO, %llu MiB, %zu KiB chunks, queue depth %zu\n",
                static_cast<unsigned long long>(mib), chunk_kib, kDepth);

    (void)run_raw<true>(raw_ring, f.raw, raw_buf, chunk, n_chunks);
    (void)run_raw<false>(raw_ring, f.raw, raw_buf, chunk, n_chunks);

    const double iox_wr = run_iox<true>(ctx, f.raw, *pool, chunk, n_chunks);
    const double raw_wr = run_raw<true>(raw_ring, f.raw, raw_buf, chunk, n_chunks);
    const double iox_rd = run_iox<false>(ctx, f.raw, *pool, chunk, n_chunks);
    const double raw_rd = run_raw<false>(raw_ring, f.raw, raw_buf, chunk, n_chunks);

    ::operator delete(raw_buf, std::align_val_t{4096});

    if (iox_wr < 0 || iox_rd < 0) {
        std::fprintf(stderr, "iox pass failed\n");
        return 1;
    }

    std::printf("  write : iox %8.1f MiB/s   raw %8.1f MiB/s   (%.0f%%)\n",
                iox_wr, raw_wr, 100.0 * iox_wr / raw_wr);
    std::printf("  read  : iox %8.1f MiB/s   raw %8.1f MiB/s   (%.0f%%)\n",
                iox_rd, raw_rd, 100.0 * iox_rd / raw_rd);
    return 0;
}
