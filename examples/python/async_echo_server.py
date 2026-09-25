#!/usr/bin/env python3
"""async_echo_server — a concurrent TCP echo server on the iox asyncio bridge.

Model: one iox.AsyncContext owns one internal io thread (one io_uring
instance) for its whole lifetime. Every `await` on an iox handle submits the
operation to that ring and marshals the completion back onto the asyncio
loop through loop.call_soon_threadsafe — so the asyncio loop itself may run
on any thread (here: the main thread), and many coroutines share the one
ring without ever blocking it.

That is what the synchronous surface (examples/python/echo_server.py)
cannot do: one coroutine per connection, real concurrency from a single
Python thread.

    python3 async_echo_server.py [PORT]
"""
import asyncio
import sys

import iox


async def serve(conn) -> None:
    try:
        while data := await conn.arecv(4096):
            await conn.asend(data)
    finally:
        await conn.aclose()


async def main(port: int) -> None:
    async_context = iox.AsyncContext()
    listener = async_context.listen("127.0.0.1", port)
    print(f"echoing on 127.0.0.1:{port} (ctrl-c to stop)", flush=True)
    while True:
        conn = await listener.aaccept()
        asyncio.create_task(serve(conn))


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    try:
        asyncio.run(main(port))
    except KeyboardInterrupt:
        pass
