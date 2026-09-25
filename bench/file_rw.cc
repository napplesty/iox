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
    int raw_fd = -1;
    explicit temp_file() : path("/tmp/iox_bench_" + std::to_string(::getpid())) {
        raw_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    }
    ~temp_file() {
        if (raw_fd >= 0) {
            ::close(raw_fd);
        }
        ::unlink(path.c_str());
    }
    temp_file(const temp_file&) = delete;
    temp_file& operator=(const temp_file&) = delete;
};

double mib_per_s(std::uint64_t bytes, clock_t_::duration duration) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0) /
           std::chrono::duration<double>(duration).count();
}

template <bool IsWrite>
struct file_window;

template <bool IsWrite>
struct refill_receiver {
    using receiver_concept = stdexec::receiver_tag;
    file_window<IsWrite>* window = nullptr;
    std::size_t slot = 0;

    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token,
                                          stdexec::inplace_stop_token{}}};
    }
    template <class... As>
    void set_value(As&&...) && noexcept { window->on_complete(slot); }
    void set_error(iox::error) && noexcept { window->on_complete(slot); }
    void set_error(std::exception_ptr) && noexcept { window->on_complete(slot); }
    void set_stopped() && noexcept { window->on_complete(slot); }
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

    io_context& context;
    buffer_pool& pool;
    iox::fd file;
    std::size_t chunk = 0;
    std::size_t n_chunks = 0;
    std::size_t next = 0;
    std::uint64_t count = 0;
    std::vector<std::optional<op_t>> slots;
    std::vector<std::optional<registered_buffer>> buffers;

    void arm(std::size_t chunk_index, std::size_t slot) {
        auto buffer = pool.take();
        if (!buffer) {
            return;
        }
        buffers[slot] = *buffer;
        const uoffset_t offset{static_cast<std::uint64_t>(chunk_index) * chunk};
        if constexpr (IsWrite) {
            slots[slot].emplace(stdexec::connect(
                io::write_at(context, file, buffer->readable(), offset),
                refill_receiver<true>{this, slot}));
        } else {
            slots[slot].emplace(stdexec::connect(
                io::read_at(context, file, buffer->writable(), offset),
                refill_receiver<false>{this, slot}));
        }
        stdexec::start(*slots[slot]);
    }

    void on_complete(std::size_t slot) {
        ++count;
        if (buffers[slot]) {
            pool.give_back(*buffers[slot]);
            buffers[slot].reset();
        }
        if (next < n_chunks) {
            arm(next++, slot);
        } else if (count == n_chunks) {
            context.stop();
        }
    }
};

template <bool IsWrite>
double run_iox(io_context& context, int raw_fd, buffer_pool& pool, std::size_t chunk,
               std::size_t n_chunks) {
    file_window<IsWrite> window{context, pool, iox::fd{raw_fd}, chunk, n_chunks, 0, 0, {}, {}};
    window.slots.resize(kDepth);
    window.buffers.resize(kDepth);

    const auto start_time = clock_t_::now();
    {
        batch_scope scope{context};
        for (std::size_t index = 0; index < kDepth && index < n_chunks; ++index) {
            window.arm(window.next++, index);
        }
    }
    context.run_for(60s);
    const auto end_time = clock_t_::now();

    if (window.count != n_chunks) {
        return -1.0;
    }
    return mib_per_s(static_cast<std::uint64_t>(chunk) * n_chunks, end_time - start_time);
}

template <bool IsWrite>
double run_raw(uring::ring& ring, int raw_fd, std::byte* buffer, std::size_t chunk,
               std::size_t n_chunks) {
    struct raw_op final : op_base {
        static void thunk(op_base*, io_context&, std::int32_t, std::uint32_t) noexcept {}
        raw_op() noexcept : op_base(&raw_op::thunk) {}
    };
    std::vector<raw_op> operations(kDepth);
    std::vector<std::byte*> buffers(kDepth);
    for (auto& slot_buffer : buffers) {
        slot_buffer = buffer + (&slot_buffer - buffers.data()) * chunk;
    }

    std::size_t next = 0, reaped = 0, inflight = 0;
    const auto start_time = clock_t_::now();
    while (reaped < n_chunks) {
        while (inflight < kDepth && next < n_chunks) {
            io_uring_sqe* sqe = ring.next_sqe();
            if (sqe == nullptr) {
                ring.flush();
                sqe = ring.next_sqe();
                if (sqe == nullptr) break;
            }
            const auto offset = static_cast<std::int64_t>(next) *
                                static_cast<std::int64_t>(chunk);
            if constexpr (IsWrite) {
                ::io_uring_prep_write(sqe, raw_fd, buffers[next % kDepth], chunk, offset);
            } else {
                ::io_uring_prep_read(sqe, raw_fd, buffers[next % kDepth], chunk, offset);
            }
            ::io_uring_sqe_set_data(sqe, &operations[inflight]);
            ++inflight;
            ++next;
        }
        ring.flush_and_wait(1);
        const unsigned count = ring.for_each_cqe([](io_uring_cqe*) {});
        reaped += count;
        inflight -= count;
    }
    const auto end_time = clock_t_::now();
    return mib_per_s(static_cast<std::uint64_t>(chunk) * n_chunks, end_time - start_time);
}

}

int main(int argc, char** argv) {
    const std::uint64_t mib = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 128;
    const std::size_t chunk_kib = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 64;

    const std::size_t chunk = chunk_kib * 1024;
    const std::size_t n_chunks = static_cast<std::size_t>(mib) * 1024 * 1024 / chunk;

    io_context context{uring::ring_params{.entries = 256, .cq_entries = 4096}};
    if (!context.ok()) {
        std::fprintf(stderr, "ring init failed\n");
        return 1;
    }
    auto pool = buffer_pool::create(context, chunk, kDepth);
    if (!pool) {
        std::fprintf(stderr, "pool: %s\n", pool.error().message().c_str());
        return 1;
    }

    uring::ring raw_ring{uring::ring_params{.entries = 256, .cq_entries = 4096}};
    if (!raw_ring.ok()) {
        return 1;
    }
    auto* raw_buffer = static_cast<std::byte*>(
        ::operator new(chunk * kDepth, std::align_val_t{4096}));
    ::memset(raw_buffer, 0xAB, chunk * kDepth);

    temp_file file;
    if (file.raw_fd < 0) {
        std::fprintf(stderr, "temp file failed\n");
        return 1;
    }

    std::printf("sequential file IO, %llu MiB, %zu KiB chunks, queue depth %zu\n",
                static_cast<unsigned long long>(mib), chunk_kib, kDepth);

    (void)run_raw<true>(raw_ring, file.raw_fd, raw_buffer, chunk, n_chunks);
    (void)run_raw<false>(raw_ring, file.raw_fd, raw_buffer, chunk, n_chunks);

    const double iox_write_rate = run_iox<true>(context, file.raw_fd, *pool, chunk, n_chunks);
    const double raw_write_rate = run_raw<true>(raw_ring, file.raw_fd, raw_buffer, chunk, n_chunks);
    const double iox_read_rate = run_iox<false>(context, file.raw_fd, *pool, chunk, n_chunks);
    const double raw_read_rate = run_raw<false>(raw_ring, file.raw_fd, raw_buffer, chunk, n_chunks);

    ::operator delete(raw_buffer, std::align_val_t{4096});

    if (iox_write_rate < 0 || iox_read_rate < 0) {
        std::fprintf(stderr, "iox pass failed\n");
        return 1;
    }

    std::printf("  write : iox %8.1f MiB/s   raw %8.1f MiB/s   (%.0f%%)\n",
                iox_write_rate, raw_write_rate, 100.0 * iox_write_rate / raw_write_rate);
    std::printf("  read  : iox %8.1f MiB/s   raw %8.1f MiB/s   (%.0f%%)\n",
                iox_read_rate, raw_read_rate, 100.0 * iox_read_rate / raw_read_rate);
    return 0;
}
