// iox — unified async IO for Linux
// examples/stdin_echo.cc — handling. Every read and write goes through io_uring; nothing blocks.
#include <array>
#include <cstddef>
#include <cstdio>

#include <iox/core/exec.h>
#include <iox/core/fd.h>
#include <iox/ops.h>

int main() {
    iox::io_context context;
    std::array<std::byte, 4096> buffer;

    for (;;) {
        auto read_result = iox::exec::sync_wait(
            context, iox::io::read(context, iox::fd{0}, iox::wbytes{buffer.data(), buffer.size()}));
        if (!read_result) {
            std::fprintf(stderr, "read failed: %s\n",
                         read_result.error ? read_result.error->message().c_str() : "stopped");
            return 1;
        }
        const auto count = std::get<0>(*read_result);
        if (count == 0) {
            break;
        }

        auto write_result = iox::exec::sync_wait(
            context, iox::io::write(context, iox::fd{1}, iox::rbytes{buffer.data(), count}.first(count)));
        if (!write_result || std::get<0>(*write_result) != count) {
            return 1;
        }
    }
    return 0;
}
