# ADR-009: Driver SPI — vocabulary CPOs, completion sources, capabilities

Date: 2026-09-04
Status: accepted

## Context

M5's goal (design §六): make every hardware backend (NVMe uring_cmd, RDMA
ibverbs, XDP UMEM — M6+) pluggable with **zero core changes**. Three
questions had to be answered:

1. How does a driver's *operation* ride the same vocabulary users already
   compose (`io::read(ctx, handle, view) | pipelines`)?
2. How does a driver's *completion* — which does not come from an SQE the
   ring submitted — enter the event loop?
3. How do callers ask about *optional* runtime capabilities the type system
   cannot know (mmap-able BARs, DMA engines)?

## Decision

### 1. Vocabulary = tag_invoke CPOs (our own protocol)

All 15 vocabulary operations (read, write, read_at, write_at, poll, fsync,
close, accept, connect, send_to, recv_from, signal, wait_pid, splice, tee)
are customization-point objects. Calling `io::read(ctx, h, dest)` resolves
through unqualified `tag_invoke(io::read_t, ctx, h, dest)` — either a
driver overload found by ADL next to the handle type, or the constrained
fd-driver default template at the bottom of each ops header. A concrete
driver overload always beats the default template in overload resolution;
for non-fd handle types the default is not even viable. Callers see no
difference: same syntax, same composition, same completion-signature rules.

`iox/core/cpo.h` defines `iox::io::tag_invocable` and the protocol
deliberately does **not** use stdexec's customization machinery —
`stdexec::tag_invocable` is already marked deprecated in our snapshot
("use member functions instead"). The SPI is the library's public surface;
it must not move when stdexec internals churn. Cost check: the CPO layer
adds one constexpr-indirect call that inlines away; bench_echo stayed at
the 100% ratio vs raw liburing after the refactor (§"Verification").

Customization rules (documented per CPO in the headers): the CPO forwards
*every* argument, so a driver's `tag_invoke` for splice must spell the full
7-parameter form (`ctx, in, out, len, off_in, off_out, flags`), defaulted
or not. Defaults keep their exact accepted handle domains — the refactor
was semantics-preserving (93/93 pre-existing tests unchanged).

### 2. Completions = the op_base thunk protocol, bridged by completion_source

A driver operation is an `op_base` (address + one function pointer) whose
payload lives in the op state — the same protocol every ring op already
uses. The driver delivers completion with
`ctx.dispatch(op_address, res, flags)` (public, the same entry the CQE
loop uses). Once armed, a device operation is indistinguishable from a
ring operation: no vtable on the data path (§三.①).

`iox/driver/completion_source.h` bridges drivers that are NOT already
running on the io thread. Three attachment modes:

* **fd-mounted** — `completion_fd()` returns a readiness fd (an IRQ fd).
  attach arms one poll; the CQE calls `on_ready()`, which drains and
  dispatches. Zero busy CPU; the standard hardware shape.
* **busy slot** — no fd: the context ticks `has_work()` every ~1 ms while
  any busy source is attached (a persistent timeout op — `run()` logic is
  untouched). Costs CPU by definition; for devices with neither fd nor
  thread.
* **active** — no attachment: driver code already on the io thread (inside
  another completion, a timer continuation) calls `ctx.dispatch` directly.

Teardown carries ADR-008's lessons: detach cancels the readiness poll and
the registration keeps the watch storage until the cancel parks; retiring
watches never dereference their source; `detach_source` is legal from
inside `on_ready` (the busy tick walks by index and tolerates mid-loop
erase). The pattern for a clean shutdown: `detach → run_for(one tick)` —
pinned by tests.

### 3. Capabilities = io::supports with tag_invoke overrides

Compile-time concepts say what the *vocabulary* accepts; `io::supports(tag,
handle)` is the runtime second level (design §四.①) for capabilities that
depend on the specific device instance. fd defaults: `zero_copy` true
(registered buffers exist on every ring), others false; a driver overrides
per tag via `tag_invoke(supports_t, tag, const H&)`. Tags today:
`zero_copy`, `mmap`, `dma` (M6+ drivers).

### 4. Registration = compile-time trait

`iox::driver::registered_driver<D>` specializes to true next to the driver.
Runtime discovery (id → factory, dlopen) is deferred until a second real
driver exists; nothing in the SPI blocks it.

## Verification

* `tests/test_driver.cc` — a software counter device exercises all three
  attachment modes end-to-end through the real `io::read` CPO (including a
  producer thread for the fd-mounted doorbell and a timer continuation for
  active injection), two sources running side by side, detach-from-inside-
  on_ready, teardown cleanliness, default-fd-path no-regression, and the
  capability overrides. 93/93 suite green (release + ASan).
* `examples/eventfd_device.cc` — the M5 acceptance: an eventfd wrapped as
  a device, attached with one line, read through the ordinary vocabulary,
  capabilities queried, registered as a driver. Zero core changes.

## Consequences

* Adding an NVMe driver in M6 means: a handle type, tag_invoke overloads
  for supported ops, a completion_source (uring_cmd completions arrive as
  ring CQEs, so likely the *active* mode from the CQE thunk), capability
  answers, one trait specialization. No core edits.
* Driver-op cancellation is not yet spelled by the SPI (a driver op parked
  in a device has no stop-token wiring). M6 drivers will need it; the seam
  is the op's `start()` seeing `get_stop_token(get_env(r))` exactly like
  fd ops do.
* The deferred red-team design proposals (typed fd hatch, loop_ctl<T>,
  sync_wait expected shape, error domains, io::shutdown — ADR-008 list)
  remain deferred; none block M6.
