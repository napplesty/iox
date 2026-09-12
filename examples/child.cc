// child — the M4 process showcase: spawn a child with wired pipes, talk to
// it through the ordinary vocabulary (write_all in, read out), and reap it
// asynchronously through the pidfd — no waitpid blocking, no SIGCHLD
// handling, no pid races.
//
//     $ ./build/child
//     child said: HELLO FROM IOX
//     child exited cleanly (code 0)
#include <cstdio>

#include <iox/compose/write_all.h>
#include <iox/core/buffer.h>
#include <iox/core/exec.h>
#include <iox/ops.h>
#include <iox/pipe/channel.h>
#include <iox/process/process.h>

using namespace iox;
namespace ex = iox::exec;

int main() {
    io_context ctx;

    auto to_child = pipe::pair::create(); // parent writes → child stdin
    auto from_child = pipe::pair::create(); // child stdout → parent reads
    if (!to_child || !from_child) {
        std::perror("pipe2");
        return 1;
    }

    auto child = process::process::spawn(
        {"/usr/bin/tr", "a-z", "A-Z"},
        {.in = std::move(to_child->r), .out = std::move(from_child->w)});
    if (!child) {
        std::fprintf(stderr, "spawn: %s\n", child.error().message().c_str());
        return 1;
    }

    const std::string_view msg = "hello from iox\n";
    auto wr = ex::sync_wait(ctx, io::write_all(ctx, to_child->w, as_rbytes(std::span{msg})));
    if (!wr) {
        std::fprintf(stderr, "write: %s\n", wr.error ? wr.error->message().c_str() : "?");
        return 1;
    }
    to_child->w.reset(); // EOF: tr flushes and exits

    // Read the answer until EOF — one loop, the usual read vocabulary.
    std::byte buf[256];
    std::string got;
    bool eof = false;
    auto reader = io::loop(ctx, [&]() {
        return io::read(ctx, from_child->r, wbytes{buf, sizeof(buf)})
             | ex::let_value([&](std::size_t n) {
                   eof = (n == 0);
                   got.append(reinterpret_cast<const char*>(buf), n);
                   return ex::just(n == 0);
               });
    });
    if (!ex::sync_wait(ctx, reader)) {
        std::fprintf(stderr, "read failed\n");
        return 1;
    }
    std::printf("child said: %s", got.c_str());

    auto st = ex::sync_wait(ctx, io::wait_pid(ctx, *child));
    if (!st) {
        std::fprintf(stderr, "wait: %s\n", st.error ? st.error->message().c_str() : "?");
        return 1;
    }
    const auto& status = std::get<0>(*st);
    if (status.success()) {
        std::printf("child exited cleanly (code 0)\n");
    } else if (status.exited) {
        std::printf("child exited with code %d\n", status.code);
    } else {
        std::printf("child killed by signal %d\n", status.code);
    }
    return status.success() ? 0 : 3;
}
