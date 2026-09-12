#!/usr/bin/env python3
"""End-to-end test of the iox nanobind module (tests/test_python.py).

Run via ctest (PYTHONPATH points at the build dir) or directly:
    PYTHONPATH=build python3 tests/test_python.py
"""
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


def test_file_roundtrip():
    ctx = iox.Context()
    path = os.path.join(tempfile.mkdtemp(), "iox_py.bin")
    f = iox.open_file(ctx, path, iox.Mode.rw | iox.Mode.create | iox.Mode.truncate)
    expect(f.valid(), "file should be valid")

    payload = os.urandom(256 * 1024)
    n = f.write_at(payload, 0)
    expect(n == len(payload), f"write_at wrote {n} != {len(payload)}")
    f.fsync()

    got = f.read_at(len(payload), 0)
    expect(got == payload, "read_at must return what write_at wrote")

    f2 = iox.open_file(ctx, path, iox.Mode.read)
    head = f2.read(16)
    expect(head == payload[:16], "read() should stream from offset 0")
    f2.close()
    expect(not f2.valid(), "closed file must be invalid")

    f.close()
    try:
        f.read_at(1, 0)
        expect(False, "ops on a closed file must raise")
    except OSError:
        pass
    os.unlink(path)


def test_open_missing():
    ctx = iox.Context()
    try:
        iox.open_file(ctx, "/nonexistent/iox/definitely/not/here")
        expect(False, "opening a missing file must raise")
    except OSError as e:
        expect(e.errno == 2, f"ENOENT expected, got {e.errno}")


def test_sleep():
    ctx = iox.Context()
    t0 = time.monotonic()
    iox.sleep(ctx, 0.05)
    dt = time.monotonic() - t0
    expect(0.04 <= dt < 2.0, f"sleep(0.05) slept {dt}s")
    try:
        iox.sleep(ctx, -1)
        expect(False, "negative sleep must raise")
    except ValueError:
        pass


def test_tcp_echo():
    ctx = iox.Context()
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    listener = iox.listen(ctx, "127.0.0.1", port)
    client = socket.create_connection(("127.0.0.1", port), timeout=5)
    conn = listener.accept()
    expect(conn.valid(), "accepted socket must be valid")

    msg = b"iox over nanobind"
    expect(conn.send(msg) == len(msg), "send must write everything")
    expect(client.recv(4096) == msg, "peer must see the bytes")

    client.sendall(b"pong")
    expect(conn.recv(4096) == b"pong", "recv must read peer bytes")
    conn.close()
    expect(not conn.valid(), "closed socket must be invalid")
    client.close()


def main():
    test_file_roundtrip()
    test_open_missing()
    test_sleep()
    test_tcp_echo()
    print("python bindings: all tests passed")


if __name__ == "__main__":
    main()
