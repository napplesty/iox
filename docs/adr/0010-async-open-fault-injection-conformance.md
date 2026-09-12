# ADR-010: Async open, fault injection, conformance quantification

Date: 2026-09-04
Status: accepted

## Context

Three items deferred from earlier milestones as M6 prerequisites: async
open, the fault-injection test strategy (§七.4), and the full conformance
suite (§七.3) — the executable proof of the "unified frontend" claim.
M6's reference drivers (NVMe/RDMA/XDP) extend the conformance suite to
hardware handles, so the fd-side suite must be complete and the failure
paths testable first.

## Decision

### 1. io::open — IORING_OP_OPENAT (the 16th vocabulary CPO)

`io::open(ctx, path, fs::mode)` completes `set_value(fs::file)` (adopted;
`file::adopt_fd_t` mirrors `tcp::socket`'s accept-adoption) or a typed
error. Same tag_invoke seam as the other 15 vocabulary CPOs; the fd default
serves the page-cache path. The path string is caller-owned (zero-copy
contract, like data buffers) and must outlive the sender. `O_CLOEXEC` is
forced, matching every descriptor iox mints.

One CPO lesson recorded: the dispatch call must stay *dependent* —
forwarding `P&& path` (like every other CPO forwards its handle). A
non-dependent `tag_invoke(...)` in the trailing return type resolves at
definition point, where the header's own default (declared below the CPO)
is not yet visible: hard error.

### 2. Fault injection — failpoints at the SQE-reservation seam

`ctx.arm_failpoint(nth, -errno)` makes the nth `acquire_sqe()` call refuse
the reservation; the operation completes inline through fd_sender's
existing no-SQE path (`ctx->acquire_error()`: the injected code, or EBUSY
for an unusable ring). No synthesized CQEs, no double completion, full
stop-callback discipline; the ordinal counts every reservation (vocabulary
ops, timers, cancel receipts — tests control it by arming after setup).
EMFILE/ENOMEM are injected; ENOENT & friends come from real kernel paths.

Real-fault cases ride alongside: mid-operation disconnect (blocked read →
EOF or reset; subsequent writes → typed EPIPE after the FIN/RST race window),
and the cancel-race shape (stop while a pipe read is blocked).

### 3. Conformance quantification

Beyond the existing byte-roundtrip over pipe/raw-fd/file/unix/tcp/
registered-buffers, three behavioral contracts are now asserted across
every applicable backend with shared parametrized helpers:

* **EOF** — after the writer side is cut, a read completes 0 (a VALUE;
  files: read_at past end).
* **Dead reader** — subsequent writes surface typed EPIPE/ECONNRESET,
  never SIGPIPE, never a hang (first write may land in socket buffers;
  the follow-up must fail).
* **Cancellation** — a blocked read under an external stop completes
  `set_stopped`: no value, no error, promptly (pipe/unix/tcp).

### 4. Recorded semantics: when_all under an external stop request

While writing the cancellation conformance we hit (and the red-team design
group's iso3 foreshadowed) stdexec's when_all behavior: a stop REQUEST on
the receiver's token transitions when_all to its stopped state directly,
so it completes `set_stopped` — discarding values — even when every child
converted its cancellation into a value via `upon_stopped`. Void-completing
children make the composed sender non-stopped-capable and values flow;
value-completing children do not. This is stdexec's contract, not a bug to
fix in iox; the conformance suite therefore pins cancellation at the child
level, and composed-stop behavior is documented here for users.

## Verification

* tests/test_fault.cc — 6 cases: Nth-submit injection (ENOMEM), injected
  EMFILE on io::open, detached op under injection (self-frees, ASan-gated),
  real ENOENT, mid-op disconnect, cancel race.
* tests/test_conformance.cc — +4 cases: EOF ×{pipe, unix, tcp}, dead
  reader ×{pipe, tcp}, cancellation ×{pipe, unix, tcp}, file positional
  EOF through the async-opened handle.
* Gates: 105/105 (603 assertions) release + ASan, 3× repeat; bench_echo
  100% of raw liburing; ioxpump file→file byte-identical.
