// iox — unified async IO for Linux
// examples/ioxpump.cc — endpoints with ONE loop body, now with kernel zero-copy and a SIGINT
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

auto userspace_pump(io_context& ctx, channel& source, channel& dest, std::byte* buffer,
                    std::size_t chunk, std::uint64_t& moved, bool& eof) {
    return io::loop(ctx, [&ctx, &source, &dest, buffer, chunk, &moved, &eof]() {
        return std::visit([&](auto& h) { return io::read(ctx, h, wbytes{buffer, chunk}); }, source)
             | ex::let_value([&buffer, &moved, &eof, &ctx, &dest](std::size_t n) {
                   eof = (n == 0);
                   return std::visit(
                              [&](auto& h) { return io::write_all(ctx, h, rbytes{buffer, n}); },
                              dest)
                        | ex::then([&moved, n]() { moved += n; });
               })
             | ex::then([&eof]() { return eof; });
    });
}

}

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

    signal::set signals{SIGINT, SIGTERM};
    auto watcher = signal::watcher::create(signals);
    ex::inplace_stop_source stop_src;
    bool pump_done = false;
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

    auto r = ex::sync_wait(ctx, stop_src, io::pump(ctx, in, out, 256 * 1024, &moved));
    bool interrupted = r.stopped;

    if (!r && r.error && moved == 0 && io::detail::splice_unsupported(r.error->code())) {
        const auto chunk = 256 * 1024;
        const auto buffer = std::make_unique_for_overwrite<std::byte[]>(chunk);
        bool eof = false;
        auto r2 = ex::sync_wait(ctx, stop_src,
                                userspace_pump(ctx, *source, *dest, buffer.get(), chunk, moved, eof));
        r = r2;
        interrupted = r2.stopped;
    }

    const auto secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    pump_done = true;

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
