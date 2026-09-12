// iox — unified async IO for Linux
// include/iox/runtime/source_registry.h — the driver bridge: which external completion
#pragma once

#include <poll.h>

#include <linux/time_types.h>

#include <memory>
#include <vector>

#include "iox/driver/completion_source.h"
#include "iox/runtime/op.h"

namespace iox {

class io_context;

class source_registry {
public:
    source_registry() noexcept;

    void attach(io_context& ctx, completion_source& s) noexcept;

    void detach(io_context& ctx, completion_source& s) noexcept;

    bool attached(const completion_source& s) const noexcept;

    void sweep_retired() noexcept;

private:
    struct source_watch final : op_base {
        completion_source* source = nullptr;
        bool retiring = false;
        bool parked = false;

        source_watch() noexcept : op_base(&source_watch::on_fired) {}
        void arm(io_context& ctx) noexcept;
        static void on_fired(op_base* self, io_context& ctx, std::int32_t res,
                             std::uint32_t) noexcept;
    };

    struct source_reg {
        completion_source* source;
        std::unique_ptr<source_watch> watch;
    };

    struct busy_tick final : op_base {
        __kernel_timespec ts{};
        source_registry* registry = nullptr;
        bool active = false;
        bool in_flight = false;

        busy_tick() noexcept : op_base(&busy_tick::on_fired) {}
        static void on_fired(op_base* self, io_context& ctx, std::int32_t res,
                             std::uint32_t) noexcept;
        static void arm(io_context& ctx, busy_tick& t) noexcept;
    };

    void arm_busy_tick(io_context& ctx) noexcept;
    void stop_busy_tick(io_context& ctx) noexcept;

    std::vector<source_reg> sources_{};
    std::vector<completion_source*> busy_sources_{};
    busy_tick busy_tick_{};
};

}
