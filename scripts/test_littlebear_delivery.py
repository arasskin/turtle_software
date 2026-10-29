#!/usr/bin/env python3
"""Real TCP regression for shared, ordered subscriber delivery and backpressure."""
import argparse
import contextlib
import ctypes
import ctypes.util
import json
from pathlib import Path
import re
import socket
import struct
import sys
import tempfile

sys.dont_write_bytecode = True
from test_littlebear_tcp import Signer, blob, body, connect, exchange, mutation, response, start, stop


def run(binary, schema, shutdown=False):
    xxhash = ctypes.CDLL(ctypes.util.find_library('xxhash'))
    xxhash.XXH64.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulonglong]
    xxhash.XXH64.restype = ctypes.c_ulonglong
    with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as sockets:
        process, port = start(binary, directory)
        try:
            owner = Signer(103)
            writer = sockets.enter_context(connect(port))
            fast = sockets.enter_context(connect(port))
            slow = sockets.enter_context(connect(port))
            abrupt = sockets.enter_context(connect(port))
            for peer in (slow, abrupt):
                peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            opening = exchange(writer, body(schema, owner.public, payload=b'LBR\3\1\0\0'))
            assert opening[0] == 3 and len(opening[4]) == 32
            epoch, challenge = opening[1], opening[4]
            for peer in (fast, slow, abrupt):
                assert exchange(peer, body(schema, owner.public, 8, b'\1'))[0] == 3

            def send_update(sequence):
                text = struct.pack('!Q', sequence) + b'x' * (60000 - 8)
                hashed = struct.pack('!Q', 1) + text
                fingerprint = xxhash.XXH64(hashed, len(hashed), 0)
                value = struct.pack('!Q', fingerprint) + text
                records = struct.pack('!IQB', 1, 1, 1) + blob(value)
                delta = struct.pack('!Q', fingerprint) + records
                writer.sendall(blob(mutation(schema, owner, challenge, sequence, epoch, sequence,
                                             delta, operation=6)))
                event = response(fast)
                assert event[:3] == (5, epoch, sequence + 1), event[:3]
                assert event[3] == delta, 'fast subscriber received wrong update'
                return delta, value

            rounds = 96 if shutdown else 512
            for sequence in range(rounds):
                delta, value = send_update(sequence)
                if sequence == 48:
                    # Abort while a shared payload may be in a partial send.
                    abrupt.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    abrupt.close()
            if shutdown:
                stop(process)  # Cancels active sends and frees all shared references.
                return

            # The slow peer's 16 MiB logical backlog overflows independently.
            # Drain older frames followed by a current reconciliation opening.
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            saw_fallback = False
            previous = 0
            while previous < rounds:
                event = response(slow)
                assert event[0] in (4, 5) and event[1] == epoch
                assert previous < event[2] <= rounds
                previous = event[2]
                if event[0] == 4:
                    saw_fallback = True
                    assert event[3].startswith(b'LBR\3')
                else:
                    # Only the eight-byte sequence inside the record varies.
                    assert struct.unpack('!Q', event[3][33:41])[0] == previous - 1
            assert saw_fallback, 'slow-peer backlog did not exercise bounded fallback'
            # Fetch the current record, then prove later deltas resume normally.
            fetched = exchange(slow, body(schema, owner.public, 2, struct.pack('!IQ', 1, 1),
                                         epoch, rounds))
            assert fetched[:3] == (0, epoch, rounds)
            assert fetched[3] == struct.pack('!IQ', 1, 1) + blob(value)
            delta, _ = send_update(rounds)
            event = response(slow)
            assert event[:3] == (5, epoch, rounds + 1) and event[3] == delta
            assert exchange(slow, body(schema, owner.public, 5, b'', epoch, rounds + 1))[0] == 0
            send_update(rounds + 1)
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('server')
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[1] / 'datafriend/schema.h'
    schema = re.search(r'DATAFRIEND_SCHEMA\s+"([^"]+)"', header.read_text()).group(1)
    run(str(Path(args.server).resolve()), schema)
    run(str(Path(args.server).resolve()), schema, shutdown=True)
    print(json.dumps({'status': 'pass', 'checks': [
        'ordered direct delivery to fast subscriber', 'isolated slow-peer overflow and recovery',
        'disconnect during shared sends', 'unsubscribe cleanup', 'shutdown with pending deliveries']}))


if __name__ == '__main__':
    main()
