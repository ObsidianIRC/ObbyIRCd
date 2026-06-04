import asyncio
import os
import sys

UNIX_SOCK = os.environ.get("STUB_UNIX_SOCK", "/run/obbyirc/voice.sock")


async def main():
    unix_writers = set()

    async def on_unix(reader, writer):
        print("unix accepted", file=sys.stderr, flush=True)
        unix_writers.add(writer)
        try:
            while True:
                raw = await reader.readline()
                if not raw:
                    return
                sys.stdout.buffer.write(raw)
                sys.stdout.buffer.flush()
        finally:
            unix_writers.discard(writer)
            writer.close()

    async def stdin_loop():
        loop = asyncio.get_running_loop()
        reader = asyncio.StreamReader()
        await loop.connect_read_pipe(
            lambda: asyncio.StreamReaderProtocol(reader), sys.stdin
        )
        while True:
            raw = await reader.readline()
            if not raw:
                return
            for writer in list(unix_writers):
                try:
                    writer.write(raw)
                    await writer.drain()
                except (BrokenPipeError, ConnectionResetError):
                    unix_writers.discard(writer)

    if os.path.exists(UNIX_SOCK):
        os.unlink(UNIX_SOCK)
    server = await asyncio.start_unix_server(on_unix, path=UNIX_SOCK)
    os.chmod(UNIX_SOCK, 0o666)
    print("socket-ready", file=sys.stderr, flush=True)
    async with server:
        await asyncio.gather(server.serve_forever(), stdin_loop())


asyncio.run(main())
