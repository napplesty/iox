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


def test_gather_and_into():
    ctx = iox.Context()
    path = os.path.join(tempfile.mkdtemp(), "iox_py_many.bin")
    f = iox.open_file(ctx, path, iox.Mode.rw | iox.Mode.create | iox.Mode.truncate)
    payload = os.urandom(64 * 1024)
    f.write_at(payload, 0)

    parts = f.read_at_many([(128, 0), (256, 4096), (64, len(payload) - 64)])
    expect(len(parts) == 3, "read_at_many must return one entry per spec")
    expect(parts[0] == payload[:128]
           and parts[1] == payload[4096:4096 + 256]
           and parts[2] == payload[-64:],
           "read_at_many must gather exact slices")

    buf = bytearray(512)
    n = f.read_into(buf)
    expect(n == 512 and bytes(buf) == payload[:512],
           "read_into must fill the caller buffer from the current offset")
    f.close()
    os.unlink(path)


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

    client.sendall(b"into-buffer")
    view = bytearray(64)
    n = conn.recv_into(view)
    expect(bytes(view[:n]) == b"into-buffer", "recv_into must fill the caller buffer")

    conn.close()
    expect(not conn.valid(), "closed socket must be invalid")
    client.close()


def main():
    test_file_roundtrip()
    test_open_missing()
    test_gather_and_into()
    test_sleep()
    test_tcp_echo()
    print("python bindings: all tests passed")


if __name__ == "__main__":
    main()
