#!/usr/bin/env python3
"""End-to-end test of the iox asyncio bridge (tests/test_python_async.py).

Run via ctest (PYTHONPATH points at the build dir) or directly:
    PYTHONPATH=build-py python3 tests/test_python_async.py
"""
import asyncio
import errno
import os
import socket
import sys
import tempfile
import time

import iox


def expect(cond, msg):
    if not cond:
        print(f"FAIL: {msg}", file=sys.stderr)
        sys.exit(1)


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def test_file_roundtrip():
    async def run():
        actx = iox.AsyncContext()
        path = os.path.join(tempfile.mkdtemp(), "iox_py_async.bin")
        f = actx.open_file(path, iox.Mode.rw | iox.Mode.create | iox.Mode.truncate)
        expect(f.valid(), "file should be valid")

        payload = os.urandom(256 * 1024)
        n = await f.awrite_at(payload, 0)
        expect(n == len(payload), f"awrite_at wrote {n} != {len(payload)}")
        await f.afsync()

        got = await f.aread_at(len(payload), 0)
        expect(got == payload, "aread_at must return what awrite_at wrote")

        parts = await f.aread_at_many([(128, 0), (4096, 1024), (64, len(payload) - 64)])
        expect(len(parts) == 3, "aread_at_many must return one entry per spec")
        expect(parts[0] == payload[:128]
               and parts[1] == payload[1024:1024 + 4096]
               and parts[2] == payload[-64:],
               "aread_at_many must gather exact slices")

        f2 = actx.open_file(path, iox.Mode.read)
        head = await f2.aread(16)
        expect(head == payload[:16], "aread() should stream from offset 0")
        await f2.aclose()
        expect(not f2.valid(), "closed file must be invalid")

        await f.aclose()
        try:
            await f.aread_at(1, 0)
            expect(False, "async ops on a closed file must raise")
        except OSError as e:
            expect(e.errno == errno.EBADF, f"EBADF expected, got {e.errno}")
        actx.close()
        os.unlink(path)

    asyncio.run(run())


def test_tcp_echo():
    async def run():
        actx = iox.AsyncContext()
        port = free_port()
        listener = actx.listen("127.0.0.1", port)

        async def serve(conn):
            try:
                while data := await conn.arecv(4096):
                    await conn.asend(data)
            finally:
                await conn.aclose()

        client = await actx.connect("127.0.0.1", port)
        conn = await listener.aaccept()
        expect(conn.valid(), "accepted socket must be valid")
        task = asyncio.create_task(serve(conn))

        for msg in (b"iox over asyncio", os.urandom(128 * 1024)):
            n = await client.asend(msg)
            expect(n == len(msg), f"asend wrote {n} != {len(msg)}")
            got = b""
            while len(got) < len(msg):
                chunk = await client.arecv(len(msg) - len(got))
                expect(chunk != b"", "peer closed mid-echo")
                got += chunk
            expect(got == msg, "echo must return the same bytes")

        await client.aclose()
        await asyncio.wait_for(task, 5)  # serve sees EOF, closes, finishes
        expect(not client.valid(), "closed socket must be invalid")
        actx.close()

    asyncio.run(run())


def test_sleep():
    async def run():
        actx = iox.AsyncContext()
        t0 = time.monotonic()
        await actx.sleep(0.05)
        dt = time.monotonic() - t0
        expect(0.04 <= dt < 2.0, f"sleep(0.05) slept {dt}s")
        try:
            await actx.sleep(-1)
            expect(False, "negative sleep must raise")
        except ValueError:
            pass
        actx.close()

    asyncio.run(run())


def test_cancel():
    async def run():
        actx = iox.AsyncContext()
        port = free_port()
        listener = actx.listen("127.0.0.1", port)
        client = await actx.connect("127.0.0.1", port)
        conn = await listener.aaccept()  # the peer that never sends

        fut = asyncio.ensure_future(conn.arecv(4096))
        await asyncio.sleep(0.05)
        t0 = time.monotonic()
        fut.cancel()
        try:
            await fut
            expect(False, "a cancelled arecv must raise CancelledError")
        except asyncio.CancelledError:
            pass
        dt = time.monotonic() - t0
        expect(dt < 1.0, f"cancel took {dt}s")

        n = await client.asend(b"still alive")  # the pair survives a cancelled recv
        expect(n == 11, "peer send must still work")
        got = await conn.arecv(64)
        expect(got == b"still alive", "recv after cancel must work")

        await client.aclose()
        await conn.aclose()
        actx.close()

    asyncio.run(run())


def test_recv_into():
    async def run():
        actx = iox.AsyncContext()
        port = free_port()
        listener = actx.listen("127.0.0.1", port)
        client = await actx.connect("127.0.0.1", port)
        conn = await listener.aaccept()

        msg = b"into the buffer"
        await client.asend(msg)
        buf = bytearray(64)
        n = await conn.arecv_into(buf)
        expect(bytes(buf[:n]) == msg, "arecv_into must fill the caller buffer")

        await client.aclose()
        await conn.aclose()
        actx.close()

    asyncio.run(run())


def test_close_with_cancelled_accept():
    async def run():
        actx = iox.AsyncContext()
        listener = actx.listen("127.0.0.1", free_port())
        accepter = asyncio.ensure_future(listener.aaccept())
        await asyncio.sleep(0.05)  # let the accept get in flight
        accepter.cancel()
        try:
            await accepter
            expect(False, "a cancelled aaccept must raise CancelledError")
        except asyncio.CancelledError:
            pass
        actx.close()  # must drain the stopped op instead of leaking it
        actx.close()  # idempotent
        try:
            actx.sleep(0.01)
            expect(False, "submit after close must raise RuntimeError")
        except RuntimeError:
            pass

    asyncio.run(run())


def test_close_force_stops_pending():
    async def run():
        actx = iox.AsyncContext()
        listener = actx.listen("127.0.0.1", free_port())
        pending = asyncio.ensure_future(listener.aaccept())
        await asyncio.sleep(0.05)
        actx.close()  # force-stops the pending accept (future never user-cancelled)
        try:
            await pending
            expect(False, "close() must cancel the pending accept's future")
        except asyncio.CancelledError:
            pass

    asyncio.run(run())


def main():
    test_file_roundtrip()
    test_tcp_echo()
    test_sleep()
    test_cancel()
    test_recv_into()
    test_close_with_cancelled_accept()
    test_close_force_stops_pending()
    print("python asyncio bridge: all tests passed")


if __name__ == "__main__":
    main()
