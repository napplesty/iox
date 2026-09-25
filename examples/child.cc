// iox — unified async IO for Linux
// examples/child.cc — it through the ordinary vocabulary (write_all in, read out), and reap it
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
    io_context context;

    auto to_child = pipe::pair::create();
    auto from_child = pipe::pair::create();
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

    const std::string_view message = "hello from iox\n";
    auto write_result = ex::sync_wait(context, io::write_all(context, to_child->w, as_rbytes(std::span{message})));
    if (!write_result) {
        std::fprintf(stderr, "write: %s\n", write_result.error ? write_result.error->message().c_str() : "?");
        return 1;
    }
    to_child->w.reset();

    std::byte buffer[256];
    std::string received;
    bool eof = false;
    auto reader = io::loop(context, [&]() {
        return io::read(context, from_child->r, wbytes{buffer, sizeof(buffer)})
             | ex::let_value([&](std::size_t count) {
                   eof = (count == 0);
                   received.append(reinterpret_cast<const char*>(buffer), count);
                   return ex::just(count == 0);
               });
    });
    if (!ex::sync_wait(context, reader)) {
        std::fprintf(stderr, "read failed\n");
        return 1;
    }
    std::printf("child said: %s", received.c_str());

    auto wait_result = ex::sync_wait(context, io::wait_pid(context, *child));
    if (!wait_result) {
        std::fprintf(stderr, "wait: %s\n", wait_result.error ? wait_result.error->message().c_str() : "?");
        return 1;
    }
    const auto& status = std::get<0>(*wait_result);
    if (status.success()) {
        std::printf("child exited cleanly (code 0)\n");
    } else if (status.exited) {
        std::printf("child exited with code %d\n", status.code);
    } else {
        std::printf("child killed by signal %d\n", status.code);
    }
    return status.success() ? 0 : 3;
}
