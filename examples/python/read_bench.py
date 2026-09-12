#!/usr/bin/env python3
"""read_bench — the Python-boundary cost probe: iox read_at vs the builtin.

Honest numbers, on purpose: on hot page-cache sequential reads the builtin
wins — each builtin read() is one bare syscall, while every iox call pays
the sender → sync_wait → CQE machinery at the boundary. Where iox from
Python DOES pay off today: blocking waits release the GIL (see echo_server
/ http_get), positional ops, and speaking the same vocabulary as the C++
zero-copy paths. A batch API (N ops per wait) is the future fix for this
gap. Gated methodology lives in bench/, not here.

    python3 read_bench.py [MIB]
"""
import os
import tempfile
import time

import iox


def read_via_iox(path: str, chunk: int) -> int:
    ctx = iox.Context()
    f = iox.open_file(ctx, path, iox.Mode.read)
    total = 0
    offset = 0
    while True:
        data = f.read_at(chunk, offset)
        if not data:
            break
        offset += len(data)
        total += len(data)
    f.close()
    return total


def read_via_builtin(path: str, chunk: int) -> int:
    total = 0
    with open(path, "rb", buffering=0) as f:
        while True:
            data = f.read(chunk)
            if not data:
                break
            total += len(data)
    return total


def main() -> int:
    mib = int(sys.argv[1]) if len(sys.argv) > 1 else 128
    chunk = 1024 * 1024

    path = os.path.join(tempfile.mkdtemp(), "iox_bench.bin")
    with open(path, "wb") as f:
        block = os.urandom(1 << 20)
        for _ in range(mib):
            f.write(block)

    results = {}
    for name, fn in (("iox (io_uring read_at)", read_via_iox),
                     ("builtin (buffering=0)", read_via_builtin)):
        best = 0.0
        for _ in range(3):
            t0 = time.monotonic()
            total = fn(path, chunk)
            best = max(best, total / (1024 * 1024) / (time.monotonic() - t0))
        results[name] = best

    for name, speed in results.items():
        print(f"{name:28s} {speed:7.0f} MiB/s")
    os.unlink(path)
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
