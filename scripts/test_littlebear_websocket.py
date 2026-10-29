#!/usr/bin/env python3
"""Real io_uring WebSocket framing, shared TCP/WS sync, and backpressure tests.
Only Python's standard library and the existing libsodium/xxhash test helpers.
"""
import argparse
import base64
import contextlib
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import sys
import tempfile

sys.dont_write_bytecode = True
import test_littlebear_tcp as tcp
import test_littlebear_delivery as delivery

RAW_START, RAW_CONNECT = tcp.start, tcp.connect
WS_PORTS = {}


def wire(data=b'', opcode=2, fin=True, mask=True):
    n = len(data)
    header = bytes([(0x80 if fin else 0) | opcode])
    header += bytes([(0x80 if mask else 0) | (n if n < 126 else 126 if n <= 65535 else 127)])
    if n >= 126:
        header += struct.pack('!H' if n <= 65535 else '!Q', n)
    if not mask:
        return header + data
    key = b'\x17\x29\x83\x41'
    return header + key + bytes(b ^ key[i % 4] for i, b in enumerate(data))


def request(key=None, extra='Sec-WebSocket-Protocol: binary\r\n'):
    key = key or base64.b64encode(os.urandom(16)).decode()
    return (f'GET /sync HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
            f'Connection: keep-alive, Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
            f'Sec-WebSocket-Version: 13\r\n{extra}\r\n').encode(), key


def http_response(peer):
    result = b''
    while not result.endswith(b'\r\n\r\n'):
        result += tcp.exact(peer, 1)
        assert len(result) <= 8192
    return result


def read_frame(peer):
    header = tcp.exact(peer, 2)
    assert header[0] & 0x80 and not header[0] & 0x70 and not header[1] & 0x80
    n = header[1]
    if n == 126:
        n, = struct.unpack('!H', tcp.exact(peer, 2))
        assert n >= 126
    elif n == 127:
        n, = struct.unpack('!Q', tcp.exact(peer, 8))
        assert n >= 65536
    assert n <= 8*1024*1024 + 4
    return header[0] & 15, tcp.exact(peer, n)


class WebSocket:
    """Socket-shaped adapter lets the existing protocol regressions run intact."""
    def __init__(self, port, extra=b'', split=False, protocol=True):
        self.socket = RAW_CONNECT(port)
        self.pending = bytearray()
        header, key = request(extra='Sec-WebSocket-Protocol: binary\r\n' if protocol else '')
        if split:
            for byte in header[:-1]:
                self.socket.sendall(bytes([byte]))
            self.socket.sendall(header[-1:] + extra)
        else:
            self.socket.sendall(header + extra)
        reply = http_response(self.socket)
        assert reply.startswith(b'HTTP/1.1 101 '), reply
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        assert b'Sec-WebSocket-Accept: ' + accept + b'\r\n' in reply
        assert (b'Sec-WebSocket-Protocol: binary\r\n' in reply) == protocol

    def __getattr__(self, name):
        return getattr(self.socket, name)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def sendall(self, data):
        self.socket.sendall(wire(data))

    def recv(self, size):
        while not self.pending:
            try:
                opcode, payload = read_frame(self.socket)
            except EOFError:
                return b''
            if opcode == 8:
                return b''
            assert opcode == 2, opcode
            self.pending.extend(payload)
        result = bytes(self.pending[:size])
        del self.pending[:size]
        return result


def start(binary, directory, extra=()):
    process, port = RAW_START(binary, directory, (*extra, '--ws-port', '0'))
    line = process.stdout.readline()
    assert line.startswith('Littlebear WebSocket listening '), line
    WS_PORTS[port] = int(line.rsplit(':', 1)[1])
    return process, port


def ws_connect(port):
    return WebSocket(WS_PORTS[port])


def check_close(port, frame, code):
    with WebSocket(port) as peer:
        peer.socket.sendall(frame)
        op, reply = read_frame(peer.socket)
        assert op == 8 and struct.unpack('!H', reply[:2])[0] == code, (op, reply)
        assert peer.socket.recv(1) == b''


def framing(binary, schema):
    with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as sockets:
        process, port = start(binary, directory)
        ws = WS_PORTS[port]
        heartbeat = tcp.blob(tcp.body(schema, b'', 7, b'', channel=0, request_id=71))
        try:
            # HTTP split across reads, and first frame coalesced with the upgrade.
            for split in (False, True):
                with WebSocket(ws, wire(heartbeat), split=split, protocol=split) as peer:
                    assert tcp.response(peer, True)[:3] == (71, 0, 0)
                    # LBS framing is independent of WS frames/messages. A ping can
                    # interrupt a fragmented message in the middle of its prefix.
                    peer.socket.sendall(wire(heartbeat[:2], fin=False) + wire(b'hello', 9) +
                                        wire(heartbeat[2:], opcode=0))
                    assert read_frame(peer.socket) == (10, b'hello')
                    assert tcp.response(peer, True)[:3] == (71, 0, 0)
                    peer.socket.sendall(wire(b'') + wire(heartbeat * 200))
                    for _ in range(200):
                        assert tcp.response(peer)[0] == 0
                    # Each application byte in a separate binary message.
                    peer.socket.sendall(b''.join(wire(bytes([b])) for b in heartbeat))
                    assert tcp.response(peer)[0] == 0
                    peer.socket.sendall(wire(b'ignored', 10) + wire(heartbeat))
                    assert tcp.response(peer)[0] == 0
                    # Extended 16- and 64-bit frame sizes, across recv buffer boundaries.
                    for length in (126, 65535, 65536, 100000):
                        data = tcp.body(schema, bytes(32)) + bytes(length)
                        assert tcp.exchange(peer, data)[0] == 2
                    peer.socket.sendall(wire(b'\3\350bye', 8))
                    assert read_frame(peer.socket) == (8, b'\3\350bye')
                    assert peer.socket.recv(1) == b''
            for frame, code in [
                (wire(b'x', mask=False), 1002), (wire(b'x', 1), 1003),
                (wire(b'x', 3), 1002), (wire(b'x', 0), 1002),
                (wire(b'x', 9, fin=False), 1002), (wire(bytes(126), 9), 1002),
                (wire(b'x', 8), 1002), (wire(b'\3\355', 8), 1002),
                (wire(b'\3\350\xff', 8), 1007),
                (b'\xc2\x80abcd', 1002),
                (b'\x82\xfe\x00\x01abcd', 1002),
                (b'\x82\xff' + struct.pack('!Q', 65535) + b'abcd', 1002),
                (b'\x82\xff' + struct.pack('!Q', 1 << 63) + b'abcd', 1002),
                (b'\x82\xff' + struct.pack('!Q', 8*1024*1024+5) + b'abcd', 1009),
                (wire(b'x', fin=False) + wire(b'y'), 1002),
            ]:
                check_close(ws, frame, code)
            header, _ = request()
            bad = [header.replace(b'13\r\n', b'12\r\n'), header.replace(b'GET', b'POST'),
                   header.replace(b'Host:', b'Bad:'), header.replace(b'Upgrade: websocket', b'Upgrade: other'),
                   header.replace(b'Connection: keep-alive, Upgrade', b'Connection: close'),
                   header[:-2] + b'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n',
                   header[:-2] + b'Content-Length: 1\r\n\r\n',
                   header.replace(b'Host: localhost', b'Host: local\x00host'), b'G' * 8192]
            for i, data in enumerate(bad):
                with RAW_CONNECT(ws) as peer:
                    peer.sendall(data)
                    reply = http_response(peer)
                    assert reply.startswith(b'HTTP/1.1 ' + (b'426' if i == 0 else b'431' if i == len(bad)-1 else b'400')), reply
                    assert peer.recv(1) == b''
            # Shared state and unchanged authentication in both directions.
            xxhash = ctypes.CDLL(ctypes.util.find_library('xxhash'))
            xxhash.XXH64.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulonglong]
            xxhash.XXH64.restype = ctypes.c_ulonglong
            for writer_ws in (False, True):
                with (ws_connect(port) if writer_ws else RAW_CONNECT(port)) as writer, \
                     (RAW_CONNECT(port) if writer_ws else ws_connect(port)) as reader:
                    signer = tcp.Signer(110 + writer_ws)
                    opening = tcp.exchange(writer, tcp.body(schema, signer.public, payload=b'LBR\3\1\0\0'))
                    epoch, challenge = opening[1], opening[4]
                    assert opening[0] == 3 and len(challenge) == 32
                    assert tcp.exchange(reader, tcp.body(schema, signer.public, 8, b'\1'))[0] == 3
                    text = b'native and web share this record' * 1500
                    values = []
                    fingerprint = 0
                    for key in (1, 2):
                        hashed = struct.pack('!Q', key) + text
                        record_hash = xxhash.XXH64(hashed, len(hashed), 0)
                        fingerprint ^= record_hash
                        values.append(struct.pack('!Q', record_hash) + text)
                    records = struct.pack('!I', 2) + b''.join(
                        struct.pack('!QB', key, 1) + tcp.blob(value)
                        for key, value in enumerate(values, 1))
                    delta = struct.pack('!Q', fingerprint) + records
                    signed = tcp.mutation(schema, signer, challenge, 0, epoch, 0, delta, operation=6)
                    writer.sendall(tcp.blob(signed))
                    event = tcp.response(reader)
                    assert event[:3] == (5, epoch, 1) and event[3] == delta
                    fetched = tcp.exchange(reader, tcp.body(schema, signer.public, 2, struct.pack('!IQQ', 2, 1, 2), epoch, 1))
                    assert fetched[3] == struct.pack('!I', 2) + b''.join(struct.pack('!Q', key) + tcp.blob(value) for key, value in enumerate(values, 1))
                    assert tcp.exchange(reader, signed)[0] == 2, 'connection replay crossed transports'
                    assert tcp.exchange(reader, tcp.body(schema, signer.public, 5, b'', epoch, 1))[0] == 0
            # Shutdown cancels both listeners, HTTP handshakes and partial frames.
            for data in (b'GET /', b''):
                peer = sockets.enter_context(RAW_CONNECT(ws)); peer.sendall(data)
            for data in (b'\x82', wire(heartbeat)[:9]):
                peer = sockets.enter_context(WebSocket(ws)); peer.socket.sendall(data)
            tcp.stop(process)
        finally:
            if process.poll() is None:
                process.kill(); process.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('server')
    parser.add_argument('--framing-only', action='store_true')
    args = parser.parse_args()
    binary = str(Path(args.server).resolve())
    schema = re.search(r'DATAFRIEND_SCHEMA "([^"]+)"', (Path(__file__).resolve().parents[1] / 'datafriend/schema.h').read_text()).group(1)
    framing(binary, schema)
    if not args.framing_only:
        # The entire TCP authentication/reconciliation regression over WebSocket.
        tcp.start, tcp.connect = start, ws_connect
        tcp.run(binary, schema)
        # TCP writer and WS subscribers: shared payload partial sends, slow-peer
        # overflow/fallback, unsubscribe, disconnect. Then all-WS during shutdown.
        delivery.start = start
        serial = 0
        def mixed_connect(port):
            nonlocal serial
            serial += 1
            return RAW_CONNECT(port) if serial == 1 else ws_connect(port)
        delivery.connect = mixed_connect
        delivery.run(binary, schema)
        delivery.connect = ws_connect
        delivery.run(binary, schema, shutdown=True)
    print(json.dumps({'status': 'pass', 'checks': ['HTTP upgrade', 'fragmentation and coalescing',
        'controls and protocol errors', 'bounded input', 'bidirectional TCP/WebSocket records',
        'signed auth and cross-transport replay rejection', 'dual-listener shutdown'] +
        ([] if args.framing_only else ['full protocol regression over WS', 'WS shared delivery and backpressure'])}))


if __name__ == '__main__':
    main()
