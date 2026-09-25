#!/usr/bin/env python3
"""echo_server — a TCP echo server through iox (Python twin of
examples/echo_server.cc).

One Context, one thread: accept() parks on io_uring with the GIL released,
each connection is echoed read→write_all until EOF, then the server goes
back to accepting. Sequential by design — the iox context is
single-threaded; serve many clients concurrently by giving each thread
its own Context.

    python3 echo_server.py [PORT]
"""
import sys

import iox


def serve_connection(context: iox.Context, conn) -> None:
    peer_gone = False
    while not peer_gone:
        data = conn.recv(4096)
        if not data:  # EOF
            break
        conn.send(data)
    conn.close()


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000

    context = iox.Context()
    listener = iox.listen(context, "127.0.0.1", port)
    print(f"echoing on 127.0.0.1:{port} (ctrl-c to stop)", flush=True)
    while True:
        conn = listener.accept()  # blocks with the GIL released
        serve_connection(context, conn)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
