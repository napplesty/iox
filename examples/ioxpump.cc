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

std::expected<channel, error> open_channel(io_context& context, std::string_view uri,
                                           bool for_sink) {
    const auto parts = split_uri(uri);
    if (parts.scheme == "file" || parts.scheme.empty()) {
        const auto mode = for_sink ? (fs::mode::rw | fs::mode::create | fs::mode::truncate)
                                   : fs::mode::read;
        auto file = fs::file::open(parts.rest.c_str(), mode);
        if (!file) {
            return std::unexpected(file.error());
        }
        return channel{std::move(*file)};
    }
    if (parts.scheme != "tcp") {
        return std::unexpected(error::from_errno(EINVAL));
    }
    auto endpoint = net::endpoint::parse(parts.rest);
    if (!endpoint) {
        return std::unexpected(endpoint.error());
    }
    if (parts.rest.starts_with("0.0.0.0:") || parts.rest.starts_with("[::]:")) {
        auto acceptor = net::tcp::acceptor::listen(*endpoint);
        if (!acceptor) {
            return std::unexpected(acceptor.error());
        }
        auto accepted = ex::sync_wait(context, io::accept(context, *acceptor));
        if (!accepted) {
            return std::unexpected(accepted.error ? *accepted.error : error::from_errno(EIO));
        }
        return channel{std::move(std::get<0>(*accepted))};
    }
    auto socket = net::tcp::socket::unconnected(endpoint->family());
    if (!socket) {
        return std::unexpected(socket.error());
    }
    auto connect_result = ex::sync_wait(context, io::connect(context, *socket, *endpoint));
    if (!connect_result) {
        return std::unexpected(connect_result.error ? *connect_result.error : error::from_errno(EIO));
    }
    return channel{std::move(*socket)};
}

auto userspace_pump(io_context& context, channel& source, channel& dest, std::byte* buffer,
                    std::size_t chunk, std::uint64_t& moved, bool& eof) {
    return io::loop(context, [&context, &source, &dest, buffer, chunk, &moved, &eof]() {
        return std::visit([&](auto& handle) { return io::read(context, handle, wbytes{buffer, chunk}); }, source)
             | ex::let_value([&buffer, &moved, &eof, &context, &dest](std::size_t count) {
                   eof = (count == 0);
                   return std::visit(
                              [&](auto& handle) { return io::write_all(context, handle, rbytes{buffer, count}); },
                              dest)
                        | ex::then([&moved, count]() { moved += count; });
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

    io_context context;

    signal::set signals{SIGINT, SIGTERM};
    auto watcher = signal::watcher::create(signals);
    ex::inplace_stop_source stop_source;
    bool pump_done = false;
    if (watcher) {
        ex::detach(io::loop(context, [&]() {
            return io::signal(context, *watcher)
                 | ex::then([&](::signalfd_siginfo signal_info) {
                        if (!pump_done) {
                            std::fprintf(stderr, "ioxpump: signal %u, draining...\n",
                                         signal_info.ssi_signo);
                        }
                        stop_source.request_stop();
                        return true;
                    });
        }));
    }

    auto source = open_channel(context, argv[1], false);
    if (!source) {
        std::fprintf(stderr, "open %s: %s\n", argv[1], source.error().message().c_str());
        return 1;
    }
    auto dest = open_channel(context, argv[2], true);
    if (!dest) {
        std::fprintf(stderr, "open %s: %s\n", argv[2], dest.error().message().c_str());
        return 1;
    }

    const iox::fd input_fd = std::visit([](auto& handle) { return io::detail::reader_fd(handle); }, *source);
    const iox::fd output_fd = std::visit([](auto& handle) { return io::detail::writer_fd(handle); }, *dest);

    std::uint64_t moved = 0;
    const auto start_time = std::chrono::steady_clock::now();

    auto result = ex::sync_wait(context, stop_source, io::pump(context, input_fd, output_fd, 256 * 1024, &moved));
    bool interrupted = result.stopped;

    if (!result && result.error && moved == 0 && io::detail::splice_unsupported(result.error->code())) {
        const auto chunk = 256 * 1024;
        const auto buffer = std::make_unique_for_overwrite<std::byte[]>(chunk);
        bool eof = false;
        auto fallback_result = ex::sync_wait(context, stop_source,
                                             userspace_pump(context, *source, *dest, buffer.get(), chunk, moved, eof));
        result = fallback_result;
        interrupted = fallback_result.stopped;
    }

    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    pump_done = true;

    if (watcher && !interrupted) {
        ::raise(SIGINT);
        context.run_for(std::chrono::milliseconds(50));
    }

    const double mib = static_cast<double>(moved) / (1024.0 * 1024.0);

    if (!result && result.error) {
        std::fprintf(stderr, "pump: %s\n", result.error->message().c_str());
        return 1;
    }
    if (interrupted) {
        std::printf("interrupted: moved %.1f MiB in %.2fs (%.0f MiB/s)\n", mib, seconds,
                    seconds > 0 ? mib / seconds : 0.0);
        return 0;
    }
    std::printf("moved %.1f MiB in %.2fs (%.0f MiB/s)\n", mib, seconds,
                seconds > 0 ? mib / seconds : 0.0);
    return 0;
}
