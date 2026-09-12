// timer_hello — M1 acceptance example: timers, schedule, and sender
// composition. Runs entirely on the io thread of one io_context.
//
//     ./build/timer_hello
#include <chrono>
#include <cstdio>

#include <iox/core/exec.h>
#include <iox/ops.h>

using namespace std::chrono_literals;

int main() {
    iox::io_context ctx;

    auto flow = iox::io::sleep_for(ctx, 100ms)
              | iox::exec::let_value([&] {
                    std::puts("tick 1 (t=100ms)");
                    return iox::io::sleep_for(ctx, 100ms);
                })
              | iox::exec::let_value([&] {
                    std::puts("tick 2 (t=200ms)");
                    return iox::io::schedule(ctx); // hop back through the ring
                })
              | iox::exec::then([&] {
                    std::puts("done");
                    ctx.stop();
                });

    auto r = iox::exec::sync_wait(ctx, flow);
    return r ? 0 : 1;
}
