// ioxpump — the flagship demo (M4 form): move data between ANY two
// endpoints with ONE loop body, now with kernel zero-copy and a SIGINT
// graceful exit.
//
//     ./build/ioxpump file:///tmp/source.bin tcp://127.0.0.1:9000   # push out
//     ./build/ioxpump tcp://0.0.0.0:9000 file:///tmp/dest.bin     # receive
//     ^C                                                          # graceful
//
// Zero-copy: both endpoints are fds, so the pump tries io::pump (splice
// through a bounce pipe — file→socket IS sendfile, file→file IS
// copy_file_range, all with zero userspace copies). If the kernel refuses
// splice for this pair, it falls back to the userspace read/write loop
// below — same vocabulary, same semantics.
//
// Graceful exit: SIGINT/SIGTERM are blocked and watched on a signalfd;
// the watcher flips a stop source, sync_wait exposes it to the pump, the
// in-flight splice is cancelled through the loop (the loop forwards stop
// tokens to the operations it runs), and the pump unwinds with set_stopped
// instead of dying mid-write. At most the in-flight chunk (≤256 KiB,
// sitting in the bounce pipe) is lost; everything that reached the sink
// stays there.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <iox/compose/detach.h>
#include <iox/compose/loop.h>
#include <iox/compose/pump.h>
#include <iox/compose/write_all.h>
#include <iox/core/buffer.h>
#include <iox/core/exec.h>
#include <iox/fs/file.h>
#include <iox/net/tcp.h>
#include <iox/ops.h>
#include <iox/signal/set.h>
#include <iox/signal/watcher.h>

using namespace iox;
namespace ex = iox::exec;

namespace {

using channel = std::variant<fs::file, net::tcp::socket>;

struct parsed {
    std::string scheme;
    std::string rest;
};

parsed split_uri(std::string_view uri) {
    const auto colon = uri.find("://");
    if (colon == std::string_view::npos) {
        return {.scheme = "file", .rest = std::string{uri}};
    }
    return {.scheme = std::string{uri.substr(0, colon)},
            .rest = std::string{uri.substr(colon + 3)}};
}

std::expected<channel, error> open_channel(io_context& ctx, std::string_view uri,
                                           bool for_sink) {
    const auto p = split_uri(uri);
    if (p.scheme == "file" || p.scheme.empty()) {
        const auto m = for_sink ? (fs::mode::rw | fs::mode::create | fs::mode::truncate)
                                : fs::mode::read;
        auto f = fs::file::open(p.rest.c_str(), m);
        if (!f) {
            return std::unexpected(f.error());
        }
        return channel{std::move(*f)};
    }
    if (p.scheme != "tcp") {
        return std::unexpected(error::from_errno(EINVAL));
    }
    auto ep = net::endpoint::parse(p.rest);
    if (!ep) {
        return std::unexpected(ep.error());
    }
    // "0.0.0.0:port" (or any wildcard) means: listen and accept one peer;
    // anything else means: connect out.
    if (p.rest.starts_with("0.0.0.0:") || p.rest.starts_with("[::]:")) {
        auto acceptor = net::tcp::acceptor::listen(*ep);
        if (!acceptor) {
            return std::unexpected(acceptor.error());
        }
        auto got = ex::sync_wait(ctx, io::accept(ctx, *acceptor));
        if (!got) {
            return std::unexpected(got.error ? *got.error : error::from_errno(EIO));
        }
        return channel{std::move(std::get<0>(*got))};
    }
    auto sock = net::tcp::socket::unconnected(ep->family());
    if (!sock) {
        return std::unexpected(sock.error());
    }
    auto conn = ex::sync_wait(ctx, io::connect(ctx, *sock, *ep));
    if (!conn) {
        return std::unexpected(conn.error ? *conn.error : error::from_errno(EIO));
    }
    return channel{std::move(*sock)};
}

/// The userspace fallback: read a chunk from ANY readable handle, write_all
/// it to ANY writable handle, stop at EOF (the M3 core). `eof` lives at the
/// CALLER: loop-lambda state must outlive the lambda (ADR-005's trap — a
/// local here dangles the moment this function returns).
auto userspace_pump(io_context& ctx, channel& source, channel& dest, std::byte* buffer,
                    std::size_t chunk, std::uint64_t& moved, bool& eof) {
    // Parameters die when this function returns; the loop lambda outlives
    // it. Values in, references to CALLER-owned state out — anything else
    // dangles (the ASan-confirmed trap this comment replaces).
    return io::loop(ctx, [&ctx, &source, &dest, buffer, chunk, &moved, &eof]() {
        return std::visit([&](auto& h) { return io::read(ctx, h, wbytes{buffer, chunk}); }, source)
             | ex::let_value([&buffer, &moved, &eof, &ctx, &dest](std::size_t n) {
                   eof = (n == 0);
                   // The then stays OUTSIDE the visit: a lambda inside a
                   // generic lambda has a distinct closure type per handle
                   // alternative, which would give the visitor two sender
                   // types. write_all itself is one non-template function
                   // (write_all.h) — exactly so visits stay mono-type.
                   return std::visit(
                              [&](auto& h) { return io::write_all(ctx, h, rbytes{buffer, n}); },
                              dest)
                        // count only what reached the sink: counting
                        // at read time over-reports when a cancel
                        // lands mid-write_all (red-team finding)
                        | ex::then([&moved, n]() { moved += n; });
               })
             | ex::then([&eof]() { return eof; });
    });
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s SRC DST\n"
                             "  file:///path | /path          file endpoint\n"
                             "  tcp://host:port               connect out\n"
                             "  tcp://0.0.0.0:port            listen, accept one peer\n"
                             "  SIGINT/SIGTERM                graceful stop\n",
                     argv[0]);
        return 2;
    }

    io_context ctx;

    // Signals become ordinary completions: block them, watch the signalfd,
    // and flip the stop source from a detached watcher loop.
    signal::set signals{SIGINT, SIGTERM};
    auto watcher = signal::watcher::create(signals);
    ex::inplace_stop_source stop_src;
    bool pump_done = false; // set once the pump finishes; quiets the exit raise
    if (watcher) {
        ex::detach(io::loop(ctx, [&]() {
            return io::signal(ctx, *watcher)
                 | ex::then([&](::signalfd_siginfo si) {
                        if (!pump_done) {
                            std::fprintf(stderr, "ioxpump: signal %u, draining...\n",
                                         si.ssi_signo);
                        }
                        stop_src.request_stop();
                        return true;
                    });
        }));
    }

    auto source = open_channel(ctx, argv[1], false);
    if (!source) {
        std::fprintf(stderr, "open %s: %s\n", argv[1], source.error().message().c_str());
        return 1;
    }
    auto dest = open_channel(ctx, argv[2], true);
    if (!dest) {
        std::fprintf(stderr, "open %s: %s\n", argv[2], dest.error().message().c_str());
        return 1;
    }

    const iox::fd in = std::visit([](auto& h) { return io::detail::reader_fd(h); }, *source);
    const iox::fd out = std::visit([](auto& h) { return io::detail::writer_fd(h); }, *dest);

    std::uint64_t moved = 0;
    const auto t0 = std::chrono::steady_clock::now();

    // Kernel zero-copy first; on a splice refusal with nothing moved, the
    // same stream goes through the userspace fallback below.
    auto r = ex::sync_wait(ctx, stop_src, io::pump(ctx, in, out, 256 * 1024, &moved));
    bool interrupted = r.stopped; // set_stopped under the stop token

    if (!r && r.error && moved == 0 && io::detail::splice_unsupported(r.error->code())) {
        const auto chunk = 256 * 1024;
        const auto buffer = std::make_unique_for_overwrite<std::byte[]>(chunk);
        bool eof = false;
        auto r2 = ex::sync_wait(ctx, stop_src,
                                userspace_pump(ctx, *source, *dest, buffer.get(), chunk, moved, eof));
        r = r2;
        interrupted = r2.stopped;
    }

    // Stats cover the pump only — not the watcher reaping below.
    const auto secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    pump_done = true;

    // Reap the detached signal watcher — but only if it is still running.
    // Closing its fd does NOT wake the in-flight read (io_uring pins the
    // struct file), so deliver a real, blocked signal instead: the watcher
    // completes, returns true and its detached op frees itself. After an
    // interrupt the watcher has ALREADY exited; raising here would leave an
    // unconsumed SIGINT that the mask teardown delivers fatally.
    if (watcher && !interrupted) {
        ::raise(SIGINT);
        ctx.run_for(std::chrono::milliseconds(50));
    }

    const double mib = static_cast<double>(moved) / (1024.0 * 1024.0);

    if (!r && r.error) {
        std::fprintf(stderr, "pump: %s\n", r.error->message().c_str());
        return 1;
    }
    if (interrupted) {
        std::printf("interrupted: moved %.1f MiB in %.2fs (%.0f MiB/s)\n", mib, secs,
                    secs > 0 ? mib / secs : 0.0);
        return 0;
    }
    std::printf("moved %.1f MiB in %.2fs (%.0f MiB/s)\n", mib, secs,
                secs > 0 ? mib / secs : 0.0);
    return 0;
}
