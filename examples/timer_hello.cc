// iox — unified async IO for Linux
// examples/timer_hello.cc — composition. Runs entirely on the io thread of one io_context.
#include <chrono>
#include <cstdio>

#include <iox/core/exec.h>
#include <iox/ops.h>

using namespace std::chrono_literals;

int main() {
    iox::io_context context;

    auto flow = iox::io::sleep_for(context, 100ms)
              | iox::exec::let_value([&] {
                    std::puts("tick 1 (t=100ms)");
                    return iox::io::sleep_for(context, 100ms);
                })
              | iox::exec::let_value([&] {
                    std::puts("tick 2 (t=200ms)");
                    return iox::io::schedule(context);
                })
              | iox::exec::then([&] {
                    std::puts("done");
                    context.stop();
                });

    auto result = iox::exec::sync_wait(context, flow);
    return result ? 0 : 1;
}
