// iox — unified async IO for Linux
// examples/echo_server.cc — Each accepted connection gets a detached io::loop session that echoes
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

    iox::io_context context;
    auto listener = iox::net::tcp::acceptor::listen(*iox::net::endpoint::ipv4_any(port));
    if (!listener) {
        std::fprintf(stderr, "listen: %s\n", listener.error().message().c_str());
        return 1;
    }
    std::printf("echoing on 127.0.0.1:%u (Ctrl-C to stop)\n", port);

    struct session {
        iox::net::tcp::socket socket;
        std::unique_ptr<std::byte[]> buffer = std::make_unique<std::byte[]>(4096);

        explicit session(iox::net::tcp::socket sock) : socket(std::move(sock)) {}
    };

    auto serve_one = [&](iox::net::tcp::socket sock) {
        auto* session_ptr = new session{std::move(sock)};
        auto body = iox::io::loop(context, [session_ptr, &context, eof = false]() mutable {
            return iox::io::read(context, session_ptr->socket, iox::wbytes{session_ptr->buffer.get(), 4096})
                 | iox::exec::let_value([session_ptr, &context, &eof](std::size_t count) {
                       eof = (count == 0);
                       return iox::io::write_all(context, session_ptr->socket,
                                                 iox::rbytes{session_ptr->buffer.get(), count});
                   })
                 | iox::exec::then([&eof]() { return eof; });
        });
        iox::exec::detach(std::move(body) | iox::exec::upon_error([](auto&&) {})
                                 | iox::exec::then([session_ptr] { delete session_ptr; }));
    };

    auto accept_loop = iox::io::loop(context, [&]() {
        return iox::io::accept(context, *listener) | iox::exec::then(serve_one);
    });

  // The accept loop never yields true; run it detached and serve forever.
    iox::exec::detach(std::move(accept_loop));
    context.run();
    return 0;
}
