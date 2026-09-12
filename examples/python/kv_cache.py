#!/usr/bin/env python3
"""kv_cache — a tiered, fixed-size KV-block pool for LLM inference, on the
iox Python surface. This is the storage-engine layer of a Mooncake/LMCache
style disaggregated KV cache, distilled to what fixed-size objects allow.

Object model: every value is one BLOCK-byte block — e.g. one layer's KV for
a 16-token page of paged attention. Fixed size collapses the usual storage
hard parts: disk addresses are slot arithmetic, the wire protocol is one
17-byte header + exactly one block, and no layer ever parses a length.

    tier 0  DRAM   LRU dict; a hit costs zero syscalls
    tier 1  slab   preallocated fixed-slot file (O_DIRECT-ready), slot = f(key)
    tier 2  peer   a remote node running this same code as its own pool

The remote node is itself a hybrid memory+disk pool (DRAM tier + slab tier);
the local side treats it as tier 2 and keeps DRAM+slab as a read cache with
write-through puts. Run the scenario:

    python3 kv_cache.py          # self-contained demo + verification
    python3 kv_cache.py serve    # stand up a remote node (accept loop)

Threading: one iox Context per thread — the peer thread owns its Context,
the main thread owns another; they are never shared (that is the iox model).
In C++ the per-decode-step gather of a batch's blocks would be one
batch_scope (N reads, one io_uring_enter); the synchronous Python surface
issues them one by one, which is the boundary cost read_bench measures.
"""
import os
import socket
import struct
import sys
import tempfile
import threading
import time

import iox

BLOCK = 64 * 1024
HEADER = struct.Struct("<BQQ")  # op, key, aux — fixed 17 bytes
OP_GET, OP_PUT = 1, 2
OP_HIT, OP_MISS, OP_OK = 0x81, 0x82, 0x83


def block_for(key: int) -> bytes:
    return struct.pack("<Q", key) * (BLOCK // 8)


class Slab:
    """Fixed-slot slab file: slot i lives at byte offset i * BLOCK."""

    def __init__(self, ctx: iox.Context, path: str, slots: int) -> None:
        self.file = iox.open_file(ctx, path, iox.Mode.rw | iox.Mode.create | iox.Mode.truncate)
        self.free = list(range(slots))
        self.where: dict[int, int] = {}

    def __contains__(self, key: int) -> bool:
        return key in self.where

    def put(self, key: int, block: bytes) -> bool:
        slot = self.where.get(key)
        if slot is None:
            if not self.free:
                return False
            slot = self.free.pop()
            self.where[key] = slot
        self.file.write_at(block, slot * BLOCK)
        return True

    def get(self, key: int) -> bytes | None:
        slot = self.where.get(key)
        return None if slot is None else self.file.read_at(BLOCK, slot * BLOCK)

    def drop(self, key: int) -> None:
        slot = self.where.pop(key, None)
        if slot is not None:
            self.free.append(slot)

    def close(self) -> None:
        self.file.close()


class Peer:
    """Client side of the fixed protocol: one header, one block."""

    def __init__(self, ctx: iox.Context, host: str, port: int) -> None:
        self.sock = iox.connect(ctx, host, port)

    def _recv_exact(self, size: int) -> bytes:
        parts = []
        got = 0
        while got < size:
            chunk = self.sock.recv(size - got)
            if not chunk:
                raise EOFError("peer closed mid-frame")
            parts.append(chunk)
            got += len(chunk)
        return b"".join(parts)

    def get(self, key: int) -> bytes | None:
        self.sock.send(HEADER.pack(OP_GET, key, 0))
        op, _, aux = HEADER.unpack(self._recv_exact(HEADER.size))
        if op != OP_HIT:
            return None
        block = self._recv_exact(aux)
        if len(block) != aux:
            raise EOFError("peer closed mid-block")
        return block

    def put(self, key: int, block: bytes) -> None:
        self.sock.send(HEADER.pack(OP_PUT, key, len(block)) + block)
        op, _, _ = HEADER.unpack(self._recv_exact(HEADER.size))
        if op != OP_OK:
            raise ConnectionError(f"peer rejected put for block {key}")

    def close(self) -> None:
        self.sock.close()


class TieredPool:
    """DRAM LRU → slab → upstream peer. Puts write through to every tier."""

    def __init__(self, ctx: iox.Context, dram_blocks: int, slab: Slab,
                 upstream: Peer | None = None) -> None:
        self.dram: dict[int, bytes] = {}
        self.dram_blocks = dram_blocks
        self.slab = slab
        self.slab_order: list[int] = []
        self.upstream = upstream
        self.hits = {"dram": 0, "slab": 0, "peer": 0, "miss": 0}

    def get(self, key: int) -> bytes | None:
        if key in self.dram:
            self.hits["dram"] += 1
            self.dram[key] = self.dram.pop(key)
            return self.dram[key]
        if key in self.slab:
            self.hits["slab"] += 1
            block = self.slab.get(key)
        elif self.upstream is not None:
            block = self.upstream.get(key)
            if block is None:
                self.hits["miss"] += 1
                return None
            self.hits["peer"] += 1
            self._slab_fill(key, block)
        else:
            self.hits["miss"] += 1
            return None
        self._dram_fill(key, block)
        return block

    def put(self, key: int, block: bytes) -> None:
        self._dram_fill(key, block)
        self._slab_fill(key, block)
        if self.upstream is not None:
            self.upstream.put(key, block)

    def gather(self, keys) -> list[bytes]:
        return [self.get(key) for key in keys]

    def _dram_fill(self, key: int, block: bytes) -> None:
        self.dram.pop(key, None)
        self.dram[key] = block
        while len(self.dram) > self.dram_blocks:
            self.dram.pop(next(iter(self.dram)))

    def _slab_fill(self, key: int, block: bytes) -> None:
        if key not in self.slab:
            while not self.slab.put(key, block):
                victim = self.slab_order.pop(0)
                self.slab.drop(victim)
        else:
            self.slab.put(key, block)
        if key in self.slab_order:
            self.slab_order.remove(key)
        self.slab_order.append(key)


def recv_exact(sock, size: int) -> bytes:
    parts = []
    got = 0
    while got < size:
        chunk = sock.recv(size - got)
        if not chunk:
            raise EOFError("client closed mid-frame")
        parts.append(chunk)
        got += len(chunk)
    return b"".join(parts)


def serve_connection(conn, pool: TieredPool) -> None:
    while True:
        op, key, aux = HEADER.unpack(recv_exact(conn, HEADER.size))
        if op == OP_GET:
            block = pool.get(key)
            if block is not None:
                conn.send(HEADER.pack(OP_HIT, key, len(block)) + block)
            else:
                conn.send(HEADER.pack(OP_MISS, key, 0))
        elif op == OP_PUT:
            block = recv_exact(conn, aux)
            pool.put(key, block)
            conn.send(HEADER.pack(OP_OK, key, aux))
    # unreachable: the loop ends only when the client disconnects (EOFError)


def start_peer(port: int, slab_path: str, state: dict) -> threading.Thread:
    ready = threading.Event()

    def run() -> None:
        ctx = iox.Context()  # this thread's own ring — never shared
        slab = Slab(ctx, slab_path, slots=256)
        pool = TieredPool(ctx, dram_blocks=16, slab=slab)
        listener = iox.listen(ctx, "127.0.0.1", port)
        state["pool"] = pool
        ready.set()
        conn = listener.accept()
        try:
            serve_connection(conn, pool)
        except EOFError:
            pass
        finally:
            conn.close()
            slab.close()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    ready.wait()
    return thread


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "serve":
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 9020
        ctx = iox.Context()
        slab = Slab(ctx, "/tmp/kv-peer.slab", slots=4096)
        pool = TieredPool(ctx, dram_blocks=1024, slab=slab)
        listener = iox.listen(ctx, "0.0.0.0", port)
        print(f"kv peer on 0.0.0.0:{port} (dram 1024 blocks, slab 4096 slots)", flush=True)
        while True:
            conn = listener.accept()
            try:
                serve_connection(conn, pool)
            except EOFError:
                pass
            finally:
                conn.close()

    workdir = tempfile.mkdtemp(prefix="iox-kv-")
    port = free_port()
    peer_state: dict = {}
    peer_thread = start_peer(port, os.path.join(workdir, "peer.slab"), peer_state)

    ctx = iox.Context()
    slab = Slab(ctx, os.path.join(workdir, "edge.slab"), slots=8)
    pool = TieredPool(ctx, dram_blocks=4, slab=slab,
                       upstream=Peer(ctx, "127.0.0.1", port))
    print("edge  (this node): dram 4 blocks, slab 8 slots, upstream peer")
    print("peer  (remote):    dram 16 blocks, slab 256 slots")

    keys = list(range(24))
    hot = keys[-8:]  # the working set a decode step keeps touching
    expected = {key: block_for(key) for key in keys}

    started = time.perf_counter()
    for key, block in expected.items():
        pool.put(key, block)
    print(f"put   24 blocks ({24 * BLOCK // 1024} KiB) in "
          f"{(time.perf_counter() - started) * 1e3:.1f} ms (write-through)")

    def round_gather(name: str, gather_keys: list[int]) -> None:
        before = dict(pool.hits)
        started = time.perf_counter()
        blocks = pool.gather(gather_keys)
        elapsed = (time.perf_counter() - started) * 1e3
        assert all(blocks[i] == expected[key] for i, key in enumerate(gather_keys)), \
            "block corrupted in transit or in a tier"
        tiers = ", ".join(f"{tier}={pool.hits[tier] - before[tier]}" for tier in pool.hits)
        print(f"{name}: {len(gather_keys)}/{len(gather_keys)} verified in {elapsed:.1f} ms"
              f"  [{tiers}]")

    round_gather("round 1 cold sweep of 24", keys)   # wider than every tier:
    round_gather("round 2 hot subset of 8  ", hot)   # LRU thrashes down to
    round_gather("round 3 hot core of 4    ", hot[-4:])  # the peer; shrinking
    # the working set sinks it through the tiers — slab in round 2, and in
    # round 3 the last 4 touched blocks still sit in DRAM (dram hits, zero IO).

    peer_pool = peer_state.get("pool")
    if peer_pool is not None:
        tiers = ", ".join(f"{tier}={peer_pool.hits[tier]}" for tier in peer_pool.hits)
        print(f"peer tiers after serving: [{tiers}]")

    pool.upstream.close()  # EOF to the peer thread — it winds down cleanly
    slab.close()
    peer_thread.join(timeout=5)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
