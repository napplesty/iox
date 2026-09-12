#!/usr/bin/env python3
"""http_get — fetch a URL over raw TCP through iox.

Everything after name resolution is iox: connect() through io_uring, send
via write_all, recv until the peer closes. Name resolution uses
socket.gethostbyname because iox binds the async DNS bridge only on the
C++ side so far — and DNS is a blocking control-path call anyway.

    python3 http_get.py example.com [/path]
"""
import socket
import sys

import iox


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    host = sys.argv[1]
    path = sys.argv[2] if len(sys.argv) == 3 else "/"

    address = socket.gethostbyname(host)  # control path: blocking, plain
    ctx = iox.Context()
    conn = iox.connect(ctx, address, 80)
    conn.send(f"GET {path} HTTP/1.0\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())

    chunks = []
    while True:
        data = conn.recv(65536)
        if not data:  # server closed: EOF
            break
        chunks.append(data)
    conn.close()

    response = b"".join(chunks)
    head, _, body = response.partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n")[0].decode(errors="replace")
    print(status_line)
    print(f"(headers: {len(head)} B, body: {len(body)} B)")
    preview = body[:200].decode(errors="replace")
    if preview:
        print(preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
