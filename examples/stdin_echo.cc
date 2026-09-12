// stdin_echo — M1 acceptance example: fd-generic async read/write with EOF
// handling. Every read and write goes through io_uring; nothing blocks.
//
//     echo hello | ./build/stdin_echo
//     ./build/stdin_echo   # interactive (Ctrl-D ends)
#include <array>
#include <cstddef>
#include <cstdio>

#include <iox/core/exec.h>
#include <iox/core/fd.h>
#include <iox/ops.h>

int main() {
    iox::io_context ctx;
    std::array<std::byte, 4096> buf;

    for (;;) {
        auto r = iox::exec::sync_wait(
            ctx, iox::io::read(ctx, iox::fd{0}, iox::wbytes{buf.data(), buf.size()}));
        if (!r) {
            std::fprintf(stderr, "read failed: %s\n",
                         r.error ? r.error->message().c_str() : "stopped");
            return 1;
        }
        const auto n = std::get<0>(*r);
        if (n == 0) {
            break; // EOF
        }

        auto w = iox::exec::sync_wait(
            ctx, iox::io::write(ctx, iox::fd{1}, iox::rbytes{buf.data(), n}.first(n)));
        if (!w || std::get<0>(*w) != n) {
            return 1;
        }
    }
    return 0;
}
