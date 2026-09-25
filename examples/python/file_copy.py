#!/usr/bin/env python3
"""file_copy — chunked async file copy through iox (Python twin of
examples/file_copy.cc).

Positional reads and writes go straight to io_uring (read_at/write_at);
each call blocks this thread but releases the GIL. Watch the throughput:
it is the same zero-syscall-per-op path the C++ benchmark uses.

    python3 file_copy.py SRC DST [CHUNK_KIB]
"""
import sys
import time

import iox


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    source_path, destination_path = sys.argv[1], sys.argv[2]
    chunk = int(sys.argv[3]) * 1024 if len(sys.argv) == 4 else 1024 * 1024

    context = iox.Context()
    source = iox.open_file(context, source_path, iox.Mode.read)
    destination = iox.open_file(context, destination_path, iox.Mode.rw | iox.Mode.create | iox.Mode.truncate)

    offset = 0
    total = 0
    started = time.monotonic()
    while True:
        data = source.read_at(chunk, offset)
        if not data:  # EOF rides as an empty read, same as the C++ vocabulary
            break
        written = destination.write_at(data, offset)
        if written != len(data):
            raise OSError(f"short write: {written} of {len(data)}")
        offset += len(data)
        total += len(data)
    destination.fsync()
    elapsed = time.monotonic() - started

    mib = total / (1024 * 1024)
    print(f"copied {mib:.1f} MiB in {elapsed:.3f}s ({mib / elapsed:.0f} MiB/s), fsynced")
    source.close()
    destination.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
