# ADR-007: Signals, processes, inotify and the zero-copy pump (M4)

Date: 2026-09-04
Status: accepted

## Context

M4 covers the remaining "control-plane" IO of the coverage matrix (§五): signals,
processes, directory events, and the zero-copy family (splice/tee, sendfile and
copy_file_range shapes). The design constraint throughout: **nothing new in the
vocabulary** — every addition must be an ordinary fd behind the existing ops,
and every new op must follow the fd_sender skeleton (policy per operation).

## Decision

### 1. Signals are readable data (`signal/set.h`, `signal/watcher.h`, `ops/signal.h`)

signalfd turns a signal into a readable record, so signal handling becomes an
io::read-shaped completion (`io::signal` completes with the consumed
`signalfd_siginfo`; plain `io::read` also works and yields raw bytes).
Blocking discipline lives in `signal::set`: block on the calling thread at
construction, surgically `SIG_UNBLOCK` exactly the blocked set at destruction.
Because **reading the signalfd consumes the signal**, the documented lifetime
rule is: destroy the watcher before the set — a consumed signal can never
deliver its default disposition at unblock time.

### 2. Processes are pollable handles (`process/process.h`, `ops/wait_pid.h`)

`process::process::spawn` is posix_spawnp (PATH resolution, no
fork-with-threads hazard) with pipe wiring via spawn file actions: handed-over
pipe ends are dup2'd onto the child's stdio and closed on the parent side when
spawn returns. The child is tracked by a **pidfd**: `io::wait_pid` polls it
(POLLIN = death), then `waitid(P_PIDFD, WEXITED)` reaps and completes with a
parsed `exit_status`. pidfd semantics eliminate both SIGCHLD handling and the
classic kill-a-recycled-pid race (`kill()` routes through
`pidfd_send_signal`). One wait per process: the reap is the read.
Destruction neither kills nor reaps — daemons must survive handle drops.

### 3. inotify needs no new op (`fs/watcher.h`, `fs/inotify_event.h`)

The inotify fd is a readable handle; `io::read` (the ordinary vocabulary)
fills a byte buffer and `fs::event_range` walks the variable-length records
(name, mask, cookie). Adding a bespoke "wait for event" op would have
duplicated read; this is the unified-frontend thesis applied to itself.

### 4. The zero-copy pump: bounce pipe + probe + lossless fallback (`ops/splice.h`, `ops/tee.h`, `compose/pump.h`)

splice needs one pipe end, so `io::pump` owns a **bounce pipe**: per chunk,
`splice(in→pipe)` then `splice(pipe→out)` — the exact shape sendfile and
copy_file_range use internally. Consequences: file→socket **is** sendfile,
file→file **is** copy_file_range, pipe↔anything is one splice shorter than it
sounds, and no per-mode vocabulary exists.

- **Probe**: a one-byte synchronous splice of the *source* decides support
  before anything moves. A source that cannot splice completes
  `set_error(EINVAL-family)` with **zero bytes moved**, so callers can fall
  back to the userspace read/write pump with nothing lost (ioxpump does).
- **Sink refusal cannot be probed losslessly**, so the loop armours itself
  with `let_error`: on a mid-run splice failure everything stuck in the
  bounce pipe is drained and delivered with plain writes before the error
  propagates. The source position always matches exactly what reached the
  sink — the fallback can always resume.
- **Single sender type**: the whole pump is one lambda in one non-template
  inline function (the write_all trick), so every `io::pump` call shares one
  sender type; the probe verdict rides in the args and the `immediate` hook
  completes failed probes without touching the kernel.

Three kernel behaviours the debugging surfaced, now encoded in the design:

- **Pipe capacity counts in pages.** A bounce pipe sized exactly to the
  chunk leaves zero free pages once the probe byte occupies one, and the
  kernel splice then waits forever (nothing to move, nothing to return).
  The pipe is therefore never sized below the 64 KiB default and always
  gets one page of slack.
- **io_uring does not retry EAGAIN for splice** (unlike read/write). The
  bounce pipe's *write* end must stay blocking — otherwise "waiting for
  source data" turns into a spurious error completion. Only the *read* end
  is O_NONBLOCK, so the let_error recovery drain terminates on EAGAIN
  instead of parking the io thread on an empty pipe.
- **A cancelled blocking splice completes with -ERESTARTSYS (512)**, not
  -ECANCELED, when io_uring cancels it inside an io-wq worker. The
  fd_sender skeleton treats both as cancellation (`set_stopped`) for every
  operation — a cancelled pump unwinds as stopped, never as an error.

`io::tee` is IORING_OP_TEE verbatim (pipe→pipe duplication without
consuming — the tap/multicast primitive). `copy_file_range` has no io_uring
opcode at all; the bounce-pipe pump is the same zero-copy page-ref mechanism,
so the matrix cell is covered by composition rather than by a wrapper.

### 5. Graceful exit: stop tokens must flow through combinators

The SIGINT path exposed a real gap: `io::loop`'s child receiver returned an
empty environment, so **stop tokens never reached operations inside a loop**
— loop bodies were uncancellable. `child_receiver::get_env` now forwards the
downstream receiver's environment. ioxpump's shutdown is then three ordinary
pieces: a detached `io::signal` watcher flips an `inplace_stop_source`;
`sync_wait(ctx, src, pump)` exposes it; the in-flight splice is cancelled
(`IORING_OP_ASYNC_CANCEL`) and unwinds `set_stopped` — at most the in-flight
chunk is lost, everything that reached the sink stays.

### 6. run_for leaves the loop runnable (runtime fix)

A trap found by the signal tests: `run_for` returned with the internal
stopped flag set (the deadline's stop), so a following `run()`/`sync_wait`
exited instantly without pumping — in-flight SQEs stranded, senders never
completed. `run_for` now clears the flag on return; `sync_wait` restarts
defensively before pumping. Regression: "run_for leaves the loop runnable
for a later run()".

### 7. Two completion-semantics fixes the SIGINT path exposed

- **`sync_wait_result::stopped` was computed from the wrong signal.** The
  old `!done && !error` could never be true for a real completion; the
  correct reading is "completed with neither a value nor an error" — an
  engaged empty tuple (void sender) is a VALUE, not a stop. ioxpump's
  interrupted path read `stopped == false` and fell into its shutdown-raise
  with fatal consequences.
- **Detached watchers need an unwind story.** Closing an fd does NOT wake
  an in-flight io_uring read (the ring pins the struct file), so
  "close and pump the loop" cannot reap a detached watcher. ioxpump raises
  a real — blocked — signal at shutdown: the signalfd read completes, the
  loop returns done, the detached op frees itself. Gated on "not already
  interrupted": after a real SIGINT the watcher has already exited, and an
  unconsumed raise would be delivered fatally when the mask teardown
  unblocks (exactly the exit-130 bug the test caught).

## Consequences

- +9 minimal-unit headers (`signal/`, `process/`, `fs/watcher` + `fs/inotify_event`,
  `ops/{signal,wait_pid,splice,tee}`, `compose/pump`), each one concern, none
  over ~160 lines.
- Cancellation now works uniformly through `loop`/`write_all`/`pump` (env
  forwarding), which M3 tests had never exercised.
- ioxpump demonstrates the M4 acceptance: zero-copy by default, userspace
  fallback automatically, SIGINT graceful exit with byte accounting.
- Kernel floor: pidfd (5.3), splice/tee ops (5.2); the project targets 6.x+.
