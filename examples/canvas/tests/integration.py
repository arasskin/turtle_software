#!/usr/bin/env python3
"""Real native store + Janet HTTP/WebSockets. Python standard library only."""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, port, key, actor, cursor):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.sock.settimeout(5)
        nonce = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET /sync HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n'
            f'Origin: http://127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
            f'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
            f'Sec-WebSocket-Key: {nonce}\r\n\r\n').encode())
        header = b''
        while not header.endswith(b'\r\n\r\n'):
            header += self.exact(1)
        assert header.startswith(b'HTTP/1.1 101'), header
        accept = base64.b64encode(hashlib.sha1((nonce + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        assert accept in header
        self.send({'type': 'hello', 'key': key, 'actor': actor, 'cursor': cursor})
        self.records = {}
        while True:
            message = self.read()
            assert message['type'] != 'error', message
            if message['type'] == 'snapshot':
                self.records.update((r['id'], r) for r in message['records'])
            if message['type'] == 'ready':
                break

    def exact(self, length):
        out = b''
        while len(out) < length:
            data = self.sock.recv(length - len(out))
            if not data:
                raise EOFError('WebSocket disconnected')
            out += data
        return out

    def send(self, message):
        payload = json.dumps(message, separators=(',', ':')).encode()
        mask = os.urandom(4)
        length = len(payload)
        header = bytes([0x81, 0x80 | length]) if length < 126 else b'\x81\xfe' + struct.pack('!H', length)
        self.sock.sendall(header + mask + bytes(v ^ mask[i % 4] for i, v in enumerate(payload)))

    def read(self):
        first, second = self.exact(2)
        assert first == 0x81 and not second & 0x80, (first, second)
        length = second & 127
        if length == 126:
            length, = struct.unpack('!H', self.exact(2))
        elif length == 127:
            length, = struct.unpack('!Q', self.exact(8))
        return json.loads(self.exact(length))

    def wait_record(self, record_id, version):
        if self.records.get(record_id, {}).get('version', 0) >= version:
            return self.records[record_id]
        for _ in range(100):
            message = self.read()
            assert message['type'] != 'error', message
            if message['type'] == 'record':
                record = message['record']
                self.records[record['id']] = record
                if record['id'] == record_id and record['version'] >= version:
                    return record
        raise AssertionError('Expected record did not arrive')

    def put(self, record):
        self.send({'type': 'put', 'record': record})

    def close(self):
        self.sock.close()


def stroke(record_id, points, version=1):
    return dict(id=record_id, kind='stroke', version=version, points=points,
                color='#608cae', width=5, complete=False, deleted=False)


def launch(data, port, log):
    process = subprocess.Popen([str(ROOT / 'build/janet'), 'server.janet', '--port', str(port),
                                '--data', str(data), '--sync', 'off'], cwd=ROOT, stdout=log, stderr=log)
    try:
        for _ in range(100):
            if process.poll() is not None:
                log.seek(0)
                raise RuntimeError(log.read().decode())
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/health', timeout=.2) as response:
                    assert json.load(response)['ok']
                    return process
            except OSError:
                time.sleep(.05)
        raise RuntimeError('Host did not become ready')
    except BaseException:
        stop(process)
        raise


def stop(process):
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main():
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='canvas-test-') as directory, tempfile.TemporaryFile() as log:
        data = Path(directory)
        process = launch(data, port, log)
        clients = []
        try:
            key = (data / 'invite-key').read_text().strip()
            # A mismatched invitation must fail before a snapshot is disclosed.
            try:
                bad = Client(port, '0' * 64, 'unwanted-1', 'cursor-bad')
            except AssertionError as error:
                assert 'invite' in str(error).lower()
            else:
                bad.close()
                raise AssertionError('Invalid invitation accepted')
            a = Client(port, key, 'alice-001', 'cursor-alice')
            b = Client(port, key, 'bob-00001', 'cursor-bob')
            clients += [a, b]
            a.put(stroke('stroke-alice', [[10, 10], [20, 20]]))
            b.put(stroke('stroke-bob', [[10, 20], [20, 10]]))
            a.wait_record('stroke-alice', 1)
            a.wait_record('stroke-bob', 1)
            b.wait_record('stroke-alice', 1)
            b.wait_record('stroke-bob', 1)
            assert a.records == b.records
            assert len(a.records) == 2
            a.put(stroke('stroke-alice', [[10, 10], [20, 20], [30, 30]], 2))
            assert len(b.wait_record('stroke-alice', 2)['points']) == 3
            cursor = dict(id='cursor-alice', kind='cursor', version=1, name='Alice',
                          color='#608cae', x=42, y=50, active=True, online=True)
            a.put(cursor)
            assert b.wait_record('cursor-alice', 1)['x'] == 42
            a.put(dict(cursor, version=2, x=99))
            assert b.wait_record('cursor-alice', 2)['x'] == 99
            # Only the owner may edit this stroke.
            b.put(stroke('stroke-alice', [[0, 0]], 3))
            assert b.read()['type'] == 'error'
            a.put(dict(stroke('stroke-alice', [[10, 10], [20, 20], [30, 30]], 3), complete=True, deleted=True))
            assert b.wait_record('stroke-alice', 3)['deleted']
            assert not b.records['stroke-bob']['deleted']
            # Late join gets current native records rather than replaying previews.
            c = Client(port, key, 'carol-001', 'cursor-carol')
            clients.append(c)
            assert c.records['cursor-alice']['x'] == 99
            assert len(c.records) == 3
            a.close()
            assert not b.wait_record('cursor-alice', 3)['online']
            expected = b.records.copy()
        finally:
            for client in clients:
                client.close()
            stop(process)
        # Persistence survives process termination; retries remain idempotent.
        process = launch(data, port, log)
        try:
            d = Client(port, key, 'bob-00001', 'cursor-bob-new')
            assert d.records == expected, (d.records, expected)
            d.put(stroke('stroke-bob', [[10, 20], [20, 10]]))
            d.wait_record('stroke-bob', 1)
            assert len(d.records) == 3
            d.close()
        finally:
            stop(process)
    print('PASS: overlapping writers, live stroke/cursor records, ownership, late join, disconnect, restart, retry')


if __name__ == '__main__':
    main()
