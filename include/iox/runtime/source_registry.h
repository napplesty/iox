// iox — unified async IO for Linux
// runtime/source_registry.h — the driver bridge: which external completion
// sources (Driver SPI, design §六) are attached to a loop, and how they are
// pumped. fd-mounted sources get a readiness poll; busy sources tick the
// loop (~1 ms) while attached. Method bodies live in io_context.h (defined
// after class io_context — they call its public acquire_sqe/submit_cancel;
// a two-phase header-only definition keeps this header cycle-free).
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

    /// Register `s` with the loop (no-op if already attached).
    void attach(io_context& ctx, completion_source& s) noexcept;

    /// Unregister `s` (fd sources: cancel the readiness poll). Legal from
    /// inside on_ready. Retirement completes on the NEXT pump — run_for()
    /// once after detaching before destroying the source.
    void detach(io_context& ctx, completion_source& s) noexcept;

    bool attached(const completion_source& s) const noexcept;

    void sweep_retired() noexcept;

private:
    // Readiness poll for one fd-mounted source. One op in flight at a time
    // (arm -> CQE -> on_ready -> re-arm); retiring goes through
    // submit_cancel and parks on the CQE — the registration keeps the
    // storage alive until then (ADR-008 teardown lesson).
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

    // ~1 ms heartbeat while busy sources exist; wakes run() (a normal CQE)
    // so the busy slots get polled without touching the loop's logic.
    struct busy_tick final : op_base {
        __kernel_timespec ts{};
        source_registry* registry = nullptr; // back-pointer: the tick is a member
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

} // namespace iox
