# ADR-008: Red-team hardening round

Date: 2026-09-04
Status: accepted

## Context

After M4, four independent red-team agents attacked the library from
non-overlapping angles (lifetime/concurrency, cancellation semantics, API
design, stress/edge cases), each required to produce *runnable* evidence.
They filed 20+ confirmed findings; every one was re-verified by re-running
the repro before fixing, and every repro was re-run against the fixed
library to confirm it turned clean. This ADR records the fixes that changed
behavior; the proposals that need design decisions are listed at the end.

Artifacts: `.attack/{lifetime,cancel,design,stress}/` (repro programs +
reports, kept out of the build).

## Fixed: completion-signature honesty (P0, all policies)

Every vocabulary sender could complete `set_stopped` (the cancellation path)
but **no policy declared `set_stopped_t()` in its completion signatures**.
Alone, that was invisible; composed with `when_all` it was fatal: stdexec
computed "_SendsStopped = false" and routed the runtime `set_stopped` into
an empty optional — SIGABRT in debug, and in -DNDEBUG **cancellation silently
became a value success**. All 19 policies now declare `set_stopped_t()`
(mechanical: every cancellable fd op), and a regression test pins
`when_all + cancel → stopped == true, no value, no error`.

An existing test had encoded the bug as its expectation ("when_all with
external stop" REQUIRED a value); it now asserts the honest channel.

## Fixed: sync_wait drives to REAL completion (P0)

`sync_wait` treated "run() returned" as "the sender completed". Anything
calling `ctx.stop()` from an unrelated completion (watchdog — a documented
pattern) ended run() early; sync_wait then reported a fake `stopped` and
destroyed the still-in-flight operation with its stack frame. The next pump
dispatched the late CQE into dead memory (ASan stack-use-after-return).
`sync_wait_impl` now loops `while (!done) { restart(); run(); }` — a stop()
ends one pass, never the wait. Cancelling a sync_wait goes through the stop
token (which completes operations), not through ctx.stop().

## Fixed: stale run_for deadline (P1)

An interrupted `run_for` left its timeout SQE in the kernel holding the
*current* epoch; later plain `run()`/`sync_wait` (which never bumped the
epoch) honored that deadline and were killed by a `stop()` belonging to
nobody — a 900 ms timer's completion was silently swallowed by a 500 ms
corpse. `run_for` now retires the epoch on exit.

## Fixed: CQE dispatch is re-entrancy-safe (P1)

`drain()` used the batch `io_uring_for_each_cqe` + one advance-at-end. A
nested drain inside a completion (the natural `sync_wait` inside a `then`)
re-reaped the CQE *currently being dispatched* — the same op's thunk ran
again, re-entered run(), unbounded recursion, stack overflow (with a
use-after-free tail if the stack held). The ring now peeks, **copies, marks
seen, and only then dispatches** each CQE; nested drains can only see newer
entries. Cost: one copy of a 16-byte struct per completion.

## Fixed: mass cancel beyond the receipt pool (P1)

64 cancel receipts; a mass cancel of N > 64 in-flight ops silently dropped
N−64 cancel requests — and since a stop callback fires once, those ops were
**permanently uncancellable** (their sync_wait hung forever; measured: 100
ops, 36 stranded). The receipt pool is now a growing `deque` (stable
addresses); exhaustion cannot happen.

## Fixed: blocking_pool teardown (P1)

The pool keeps a resident `poll(eventfd)` wakeup op whose state lives inside
the pool object. Destroying the pool left that op in flight on a
still-alive io_context — the next pump dispatched into freed memory
(heap-use-after-free, both the SQE-pending and CQE-pending forms). The
destructor now pokes the eventfd, pumps the context so the poll completes
and — gated on `stopping_` — does not re-arm; undelivered cells are freed.
(Verification of why "close the fd" is not enough: io_uring pins the struct
file.)

## Fixed: pump cannot wedge the loop on a full sink (P1)

The bounce pipe's read end was O_NONBLOCK (for the recovery drain), which
poisoned stage-2 splice: a full sink returned `-EAGAIN` and io_uring —
unlike read/write — does **not** retry EAGAIN for splice. The error fell
into `recover_stuck`, whose synchronous `write` to the full sink parked the
io thread; even the cancel timer could never dispatch. Now both bounce ends
stay blocking on the hot path, and `recover_stuck` temporarily flips both
fds to nonblocking with a bounded total budget, restoring flags after.

Also documented (behavior kept): **one pump per sink fd**. Stage 2 splices
with `off_out = -1`, and the kernel does not serialize position writes
across separate splices the way it does for WRITE — two pumps into one fd
overwrite each other's chunks while both report full success.

## Fixed: smaller holes

- **SIGPIPE**: raw-fd `io::write` used IORING_OP_WRITE; one dead peer killed
  the process. `io_context` construction now ignores SIGPIPE process-wide
  (the same call libuv makes); typed sockets were already on
  SEND|MSG_NOSIGNAL.
- **Owning close**: `io::close(ctx, std::move(handle))` steals the fd number
  now (slot cleared at construction, no dangling pointer, no double close
  when the handle dies before the CQE — the detached-close shape). The
  borrowing lvalue form keeps slot-clear-on-completion and its "handle
  outlives the sender" contract.
- **`uoffset_t{UINT64_MAX}`** wrapped to int64 −1 — the kernel's "current
  position" sentinel — silently turning positional IO into stream IO.
  `read_at`/`write_at` now complete `EOVERFLOW` up front for offsets that do
  not round-trip.
- **`fs::event_range`** clamps to the buffer end when a record's length
  field lies or a read was truncated (was: walk past the buffer).
- **`stop_cb_slot` copies** are un-armed by construction (was: copied the
  armed bit → double-unregister hazard); the `immediate` fast path now
  releases the stop callback before completing (P2300 symmetry).
- **errno names**: table extended (ESRCH/ECHILD/EXDEV/ENOTTY/…);
  `io::signal`'s header comment corrected — io_uring *waits* on an empty
  signalfd, it does not surface EAGAIN (drain by counting, not until-error).
- **ioxpump** counts bytes when they land at the sink, not when read
  (mid-write cancellation used to over-report).

## Accepted risks / documented contracts

- `ctx.stop()` before `run()` still pre-stops that run (feature, now
  documented); `run_for`/`sync_wait` always leave the loop runnable.
- Detached work abandoned at teardown leaks by design (fire-and-forget);
  ioxpump demonstrates the unwind-via-blocked-signal pattern for watchers.
- `batch_scope` over the SQ ring size degrades to extra flushes (correct,
  observable).
- One pump per sink fd (above).

## Design proposals deferred to M5 (not implemented here)

From the design review, in priority order:

1. **Typed fd escape hatch** (`raw_fd`/`stream_fd`/`message_fd`) so the
   bypass can't silently change kernel semantics (SIGPIPE was one instance).
2. **`loop_ctl<T>`**: make io::loop's contracts mechanical — `step(v)` /
   `fail(e)` instead of bool-and-capture discipline; the eof-dangle trap
   becomes a compile error. Machine-held state; completion can carry a value
   (giving write_all/pump progress-in-completions).
3. **`sync_wait_result` → `std::expected` shape** (kills the
   `!r → r.error->` empty-deref trap; 62 `get<0>` call sites migrate).
4. **Error domains** (system/net/fs/driver bits packed into `error`,
   sizeof unchanged) — the M2 promise, still unfulfilled.
5. Vocabulary gaps: `io::shutdown` first (25-line verified sketch exists),
   then multishot accept, setsockopt, open_at, readv.

## Verification

- New `tests/test_redteam.cc`: 11 cases, one per fix.
- Suite: 86/86 cases, 482 assertions, regular + ASan/UBSan/LeakSan clean.
- All red-team repros recompiled against the fixed library: 10/11 clean;
  the 11th (t7) exercises the *borrowing* close with a dying handle — now a
  documented precondition violation whose owning form is covered green.
- echo bench re-run at 100 % ratio (no hot-path regression from the
  seen-before-dispatch copy).

## Addendum (post-M5): full repro re-verification, two escaped fix-regressions

After M5 (which rewrote every vocabulary op header for the Driver SPI),
the entire `.attack/` corpus — all 42 runnable bug-repros across the four
groups — was rebuilt against the current library and re-run. Two hangs
surfaced that were **regressions of this round's own fixes**, escaped
because the stress group hit its usage quota before finishing its
re-verification pass; both are now fixed and carry regression tests in
`tests/test_redteam.cc`:

1. **sync_wait inside a live batch_scope spun forever** (stress a2e). The
   "drive until real completion" fix loops `run()`, but `run()` refuses to
   pump behind a live `batch_scope` — the loop could never terminate. The
   old pass was accidental (single-pass sync_wait returned not-done; the
   repro's check was vacuously true). Fix: `sync_wait` rejects up front with
   `EDEADLK` — before connect/start, so nothing is left in the ring (erroring
   after start would strand an SQE pointing into a dead op state, the t8
   trap).

2. **run_for re-entered from inside a completion hung the outer loop**
   (stress a9e). The nested `run_for`'s exit bumped `run_epoch_`, retiring
   the OUTER frame's still-in-flight deadline; the outer loop then blocked
   past its own deadline forever. Fix: run-loop depth tracking — only the
   outermost `run_for` frame does the exit bookkeeping (`stopped_` reset +
   epoch retirement). A stop from a nested frame legitimately ends the whole
   stack early (pinned by the regression test).

Two repro expectations were written against **pre-fix** behavior and are
superseded on purpose: a9d's trailing bare `run()` relied on the stale
deadline stopping the loop (exactly what t1/t1b removed — a9c pins the new
contract); it now needs an explicit bound (`run_for`). t7 remains dirty by
design (borrowing close with a dying handle — documented precondition; the
owning form is the tested green path).

Corpus status after this round: 42/42 runnable repros CLEAN; not-runnable
by design: iso3/iso4b (negative doc examples), loop_ctl / sync_wait-shape /
loop-trap demos (deferred proposals below).
