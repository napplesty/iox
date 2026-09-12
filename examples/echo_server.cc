// echo_server — M3 acceptance example: concurrent TCP echo server.
//
// Each accepted connection gets a detached io::loop session that echoes
// until EOF. Note what is NOT here: no threads, no callback state machines,
// no manual CQE routing — and the session body is the same vocabulary that
// echoes files, pipes or NVMe queues.
//
//     ./build/echo_server [port=7777]        # then: nc localhost 7777
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

#include <iox/core/exec.h>
#include <iox/compose/loop.h>
#include <iox/compose/write_all.h>
#include <iox/compose/detach.h>
#include <iox/net/tcp.h>
#include <iox/ops.h>

int main(int argc, char** argv) {
    const std::uint16_t port = argc > 1 ? static_cast<std::uint16_t>(
                                              std::strtoul(argv[1], nullptr, 10))
                                        : 7777;

    iox::io_context ctx;
    auto acc = iox::net::tcp::acceptor::listen(*iox::net::endpoint::ipv4_any(port));
    if (!acc) {
        std::fprintf(stderr, "listen: %s\n", acc.error().message().c_str());
        return 1;
    }
    std::printf("echoing on 127.0.0.1:%u (Ctrl-C to stop)\n", port);

    // The session owns its socket and buffer; it lives until its loop
    // reaches EOF or error, then deletes itself.
    struct session {
        iox::net::tcp::socket sock;
        std::unique_ptr<std::byte[]> buf = std::make_unique<std::byte[]>(4096);

        explicit session(iox::net::tcp::socket s) : sock(std::move(s)) {}
    };

    auto serve_one = [&](iox::net::tcp::socket s) {
        auto* sn = new session{std::move(s)};
        // eof rides in the loop lambda's capture (op state): the chain
        // completes asynchronously after the body returned — a body-local
        // would dangle.
        auto body = iox::io::loop(ctx, [sn, &ctx, eof = false]() mutable {
            return iox::io::read(ctx, sn->sock, iox::wbytes{sn->buf.get(), 4096})
                 | iox::exec::let_value([sn, &ctx, &eof](std::size_t n) {
                       eof = (n == 0);
                       return iox::io::write_all(ctx, sn->sock,
                                                 iox::rbytes{sn->buf.get(), n});
                   })
                 | iox::exec::then([&eof]() { return eof; });
        });
        // both completion and error free the session
        iox::exec::detach(std::move(body) | iox::exec::upon_error([](auto&&) {})
                                 | iox::exec::then([sn] { delete sn; }));
    };

    auto accept_loop = iox::io::loop(ctx, [&]() {
        return iox::io::accept(ctx, *acc) | iox::exec::then(serve_one);
    });

    // The accept loop never yields true; run it detached and serve forever.
    iox::exec::detach(std::move(accept_loop));
    ctx.run();
    return 0;
}
