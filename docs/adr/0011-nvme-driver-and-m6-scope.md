# ADR-011: NVMe reference driver; M6 scope — RDMA dropped

Date: 2026-09-04
Status: accepted

## Context

M6 plans NVMe → RDMA → XDP reference drivers. Environment reality check on
the dev machine (2026-09-12):

* A real NVMe namespace is present (953.9G, `/dev/nvme0n1` + passthru node
  `/dev/ng0n1`), kernel 7.0, liburing with `io_uring_prep_uring_cmd`.
* The NVMe nodes are root-only (/dev/ng0n1 and /dev/nvme0 are 0600
  root:root; /dev/nvme0n1 is 0660 root:disk, user not in disk); the dev user has no non-interactive
  sudo. A reversible one-liner (`setfacl -m u:<user>:rw /dev/ng0n1`) grants
  what is needed — the user opted to keep the environment untouched for now.
* Soft-RoCE requires `modprobe rdma_rxe` + `rdma link add` (both root) plus
  rdma-core dev headers (apt/root). Hard walls without sudo.

## Decision

### RDMA is dropped from the roadmap

Niche hardware for this library's users; adapting later is unblocked by
design — the M5 SPI is exactly the seam (handle + tag_invoke overloads +
completion_source; an ibverbs completion channel is an fd-mounted source).
Removing it now keeps the milestone honest: no half-maintained driver.

### NVMe driver: uring_cmd passthru, runtime-detected

`include/iox/nvme/device.h` — `nvme::device` opens `/dev/ngXnY`, loads
geometry, and joins the vocabulary through concrete tag_invoke overloads:

* `io::read_at` / `io::write_at` — BYTE offsets (the unified vocabulary)
  mapped to LBAs; `nvme_uring_cmd` rides in the op state, kernel DMAs
  directly into the caller's buffer. Misaligned offset/length completes
  `EINVAL` up front (never silently rounds — same stance as write_at's
  sentinel guard). `len == 0` is a value 0.
* `io::fsync` — NVMe FLUSH passthru.
* `nvme::admin` — raw admin passthru (CAP_SYS_ADMIN escape hatch, IDENTIFY
  & friends), completing with the NVMe result dword.
* `io::close` works through the generic borrowing form (`fd_slot`).
* `io::supports`: zero_copy = true (direct DMA), dma = true; mmap = false.

Key mechanical decisions:

* **SQE128 rings.** `nvme_uring_cmd` (72 B) does not fit a 64-byte SQE's
  inline command area: `uring::ring_params::sqe128 = true` creates the ring
  with `IORING_SETUP_SQE128`, and device ops capture the ring capability at
  `tag_invoke` time, completing `EOPNOTSUPP` up front on plain rings (pinned
  by a hardware-free test).
* **Geometry from sysfs, not IDENTIFY.** Admin passthru is CAP_SYS_ADMIN-
  gated; `/sys/block/nvmeXnY/queue/logical_block_size` + `size` are
  world-readable. nsid comes from the `NVME_IOCTL_ID` ioctl (no caps).
  IDENTIFY remains available through `nvme::admin` for privileged runs.
* **Completion mapping.** uring_cmd CQE `res`: 0 → success (value = the
  requested byte count), negative → typed errno error, positive → an NVMe
  status word → `EIO`. Cancellation rides fd_sender's existing
  ASYNC_CANCEL plumbing unchanged.

### Test & bench policy (read-only against the system disk)

`tests/test_nvme.cc` and `bench/nvme_rw.cc` self-skip with the access
reason when the node is closed; with access they run fully. Writes are
opt-in only (`IOX_NVME_TEST_WRITE`, last LBA) — the dev disk hosts the
system. The ≥85% gate (QD random reads vs a raw-liburing uring_cmd loop)
executes wherever access exists; on this machine it reports skip until the
user grants the ACL.

## Verification

112/112 (606 assertions) release + ASan, both suites 3× stable;
bench_echo still 100% of raw liburing; one nvme vocabulary test
(non-sqe128 → EOPNOTSUPP) runs on every machine; six hardware cases +
bench skip with reasons here, ready to light up on any machine with access.

## Consequences

* XDP remains for M6 (compile-optional, same runtime-detection pattern;
  attach needs CAP_BPF/CAP_NET_ADMIN — likely also skip-on-this-box).
* Granting `setfacl` later requires NO rebuild: the same tests and bench
  start exercising real hardware immediately.

## Addendum: XDP driver (M6 second half)

`include/iox/xdp/socket.h` — AF_XDP (XSK) via the M5 fd-mounted
completion_source: the xsk fd is readable when RX/completion rings advance,
on_ready() drains them and dispatches parked vocabulary ops. This is the
second real consumer of the bridge and the first whose entire data plane
never touches an SQE.

Scope decisions (probed on the dev box): `socket(AF_XDP, SOCK_DGRAM)` is
EPERM without CAP_NET_RAW here — the driver compiles everywhere and the
tests self-skip with the reason (umem + capability cases run cap-free).
TX rides the generic copy path (no BPF program needed); RX requires an
attached XDP program — an ops concern. v1 honesty: TX completions match ops
FIFO (copy mode completes in order; a zerocopy driver that reorders needs a
per-chunk map), and each write triggers one sendto() (batchable later;
copy mode is syscall-dominated anyway). `xdp::write_frame` /
`xdp::read` complete with byte count / `xdp::frame`; chunks recycle on
completion. supports: dma=true, zero_copy=false (copy mode v1).

Verified: 115/115 release + ASan; xdp cases 2/3 run cap-free here, the
socket case skips with "Operation not permitted".

## Addendum 2: red-team round 2 (design / style / docs-truth), 2026-09-12

36 findings across three attackers (.attack2/*/REPORT.md, all
evidence-backed with runnable repros). Fixed this round:

* errno name table collapsed 10 codes into "ENOSPC" (bad fallthrough) and
  missed ENODEV/EACCES/EDEADLK/EAFNOSUPPORT — error messages lied.
* 10 headers were not self-contained; 54/54 now self-include, verified by
  a TU sweep.
* fsync accepted write-only handles per its constraint but hard-errored in
  the body (branch inverted) — pipe write ends now fsync correctly.
* xdp::socket defaulted moves = double-close (trivially-copyable fd) and
  never unmapped its four rings — hand-written move_from/reset with
  ring_maps_ munmap.
* nvme::admin lacked the sqe128 guard: a plain ring memcpy'd the 72-byte
  nvme_uring_cmd into a 64-byte SQE, overwriting 56 bytes of the NEIGHBOR
  SQE (design F1 — the round's nastiest). Now EOPNOTSUPP up front like its
  siblings.
* splice/tee accepted size_t len and truncated to unsigned — len=2^32
  "succeeded" with value 0; now EOVERFLOW (design F3). write(fixed,
  zero-len) lacked its rbytes twin's value-0 guard (design F4).
* test_nvme's EOPNOTSUPP case was gated behind device access, contradicting
  its own "runs on every machine" claim (docs P1) — ungated via
  `nvme::device inert;`.
* README: M1-era tail (net::tcp_socket/RDMA/iox-pump), stale M5 status,
  15-vs-16 CPO count, over-claimed conformance matrix, "idle: no syscalls"
  wording; xdp header example signature; ADR-011 permission detail.

Deferred (recorded, not fixed — joins the ADR-008 deferred list):
write_all/pump bypass the tag_invoke seam (single-type sender by design;
driver handles get the raw fd path — needs a per-handle write_all
decision), fsync domain too wide without a syncable concept, xdp senders
lack error/stopped channels, poll/close/splice domain asymmetries,
sleep_for(negative)→EINVAL vs immediate, warning gate as PUBLIC flags, and
the P3 style tail (dead aliases, .clang-format, mojibake banner).

Gates after fixes: 115/115 (616 assertions) release + ASan; 54/54 headers
self-contained.
