// iox — unified async IO for Linux
// driver/completion_source.h — the external completion bridge (design §六).
//
// A driver's completions enter the event loop through the SAME protocol the
// ring uses: an op_base address + a thunk, delivered by
// io_context::dispatch(user_data, res, flags). Payloads ride in the op
// state itself (res is just a signal). Three attachment modes:
//
//   * fd-mounted   — completion_fd() returns a readiness fd (eventfd &c);
//                    the context polls it and calls on_ready() when it is
//                    readable. Zero busy CPU; the standard hardware shape
//                    (an IRQ fd).
//   * busy slot    — completion_fd() is invalid; the context ticks the
//                    source every ~1 ms while attached and calls on_ready()
//                    when has_work() reports true. Costs CPU by definition
//                    — for devices with no fd and no thread of their own.
//   * active       — no attachment at all: driver code already running on
//                    the io thread (inside another completion, a timer,
//                    on_ready) calls ctx.dispatch directly.
//
// All callbacks run ON THE IO THREAD. attach/detach on the owning thread
// only. Control-path polymorphism only: once armed, a device operation is
// indistinguishable from a ring operation (§三.① — no vtable on the data
// path).
#pragma once

#include "iox/core/fd.h"

namespace iox {

class io_context; // the bridge is hosted by io_context (runtime/io_context.h)

/// Driver-side bridge into the event loop. Implementations keep their
/// pending-operation queues; completions are delivered with
/// ctx.dispatch(op_address, res, flags) using the op_base thunk protocol.
class completion_source {
public:
    virtual ~completion_source() = default;

    /// fd-mounted mode: the readiness fd. Return an invalid fd for the
    /// busy-slot mode. Called once at attach.
    virtual iox::fd completion_fd() const noexcept { return {}; }

    /// The fd is readable (fd mode) or the busy slot fired with work
    /// (busy mode): drain the device and dispatch completions.
    virtual void on_ready(io_context&) noexcept {}

    /// Busy mode only: return true when on_ready() should run.
    virtual bool has_work() const noexcept { return false; }
};

} // namespace iox
