#!/usr/bin/env python3
"""Signed concurrent file writes, disconnected waiters, and pending-I/O shutdown."""
import argparse
import contextlib
import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import struct
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from test_littlebear_tcp import Signer, blob, body, connect, exchange, mutation, response, start, stop


def batch(records):
    xxhash = ctypes.CDLL(ctypes.util.find_library('xxhash'))
    xxhash.XXH64.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulonglong]
    xxhash.XXH64.restype = ctypes.c_ulonglong
    text = b'x' * 1024
    result = bytearray(struct.pack('!I', records))
    for key in range(1, records + 1):
        hashed = struct.pack('!Q', key) + text
        value = struct.pack('!Q', xxhash.XXH64(hashed, len(hashed), 0)) + text
        result.extend(struct.pack('!QB', key, 1) + blob(value))
    return bytes(result)


def run(binary, schema, shutdown):
    with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as sockets:
        process, port = start(binary, directory)
        try:
            payload = batch(256)
            clients, packets = [], []
            for seed in range(192):
                peer = sockets.enter_context(connect(port))
                peer.settimeout(15)
                signer = Signer(seed)
                opening = exchange(peer, body(schema, signer.public, payload=b'LBR\3\1\0\0'))
                assert opening[0] == 3
                clients.append(peer)
                packets.append(blob(mutation(schema, signer, opening[4], 0, opening[1], 0, payload)))
            # Queue input on every connection before resuming the server. A
            # whole batch need not fit in a host's socket buffers while the
            # receiver is stopped; finish partial sends together after resume.
            with selectors.DefaultSelector() as pending:
                process.send_signal(signal.SIGSTOP)
                try:
                    for peer, packet in zip(clients, packets):
                        peer.setblocking(False)
                        try:
                            sent = peer.send(packet)
                        except BlockingIOError:
                            sent = 0
                        if sent < len(packet):
                            pending.register(peer, selectors.EVENT_WRITE, memoryview(packet)[sent:])
                finally:
                    process.send_signal(signal.SIGCONT)
                deadline = time.monotonic() + 30
                while pending.get_map():
                    remaining = deadline - time.monotonic()
                    assert remaining > 0, 'concurrent request burst timed out'
                    for key, _ in pending.select(remaining):
                        try:
                            sent = key.fileobj.send(key.data)
                        except BlockingIOError:
                            continue
                        assert sent > 0, 'connection closed during request burst'
                        if sent == len(key.data):
                            pending.unregister(key.fileobj)
                        else:
                            pending.modify(key.fileobj, selectors.EVENT_WRITE, key.data[sent:])
            for peer in clients:
                peer.settimeout(15)
            time.sleep(.02)
            if shutdown:
                stop(process)
                return
            for i, peer in enumerate(clients):
                if i % 3 == 0:
                    peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    peer.close()
            for i, peer in enumerate(clients):
                if i % 3:
                    result = response(peer)
                    assert result[0] == 0, f'capacity caused a wire retry: {result[0]}'
            with connect(port) as peer:
                assert exchange(peer, body(schema, b'', 7, b'', channel=0))[0] == 0
            stop(process)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGCONT)
                process.kill()
                process.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('server')
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[1] / 'datafriend/schema.h'
    schema = re.search(r'DATAFRIEND_SCHEMA\s+"([^"]+)"', header.read_text()).group(1)
    run(os.path.abspath(args.server), schema, False)
    run(os.path.abspath(args.server), schema, True)
    print(json.dumps({'status': 'pass', 'checks': [
        '192 simultaneous signed write batches', 'no capacity retry responses',
        'disconnected writers and waiter cleanup', 'shutdown during storage pressure']}))


if __name__ == '__main__':
    main()
