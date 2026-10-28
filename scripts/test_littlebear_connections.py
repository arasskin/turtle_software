#!/usr/bin/env python3
"""Connection growth, wakeups, fairness, lifetime and memory-pressure regressions."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from test_littlebear_tcp import Signer, blob, body, connect, exchange, mutation, response, start, stop


def readable(peers, timeout):
    # select() cannot address descriptors >= FD_SETSIZE (typically 1,024).
    poll = select.poll()
    by_fd = {peer.fileno(): peer for peer in peers}
    for fd in by_fd:
        poll.register(fd, select.POLLIN)
    return [by_fd[fd] for fd, _ in poll.poll(int(timeout * 1000))]


def rss(pid):
    status = Path(f'/proc/{pid}/status').read_text()
    return int(re.search(r'^VmRSS:\s+(\d+)', status, re.M).group(1)) * 1024


def cpu(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().split()
    return (int(fields[13]) + int(fields[14])) / os.sysconf('SC_CLK_TCK')


def heartbeat(schema, request=700):
    return body(schema, b'', 7, b'', channel=0, request_id=request)


def run(binary, schema, count):
    result = {}
    with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as peers:
        process, port = start(binary, directory)
        try:
            warm = peers.enter_context(connect(port))
            assert exchange(warm, heartbeat(schema))[0] == 0
            baseline = rss(process.pid)
            idle = [peers.enter_context(connect(port)) for _ in range(count)]
            packet = blob(heartbeat(schema))
            # Drain through more than one SQ/CQ batch, with every client ready.
            for peer in idle:
                peer.sendall(packet)
            for peer in idle:
                assert response(peer)[0] == 0
            result['idle_connections'] = count
            result['rss_growth_bytes'] = rss(process.pid) - baseline
            before = cpu(process.pid)
            time.sleep(1)
            result['idle_cpu_seconds_per_second'] = cpu(process.pid) - before
            assert result['idle_cpu_seconds_per_second'] < .25, 'idle scheduler is spinning'

            owner = peers.enter_context(connect(port))
            signer = Signer(41)
            opening = exchange(owner, body(schema, signer.public, payload=b'\1'))
            epoch, challenge = opening[1], opening[4]
            subscribers = idle[:129]
            # Register while absent, then move channel entries through growth
            # and swap-delete. Notifications must survive both operations.
            for peer in subscribers:
                for channel in (1, 2, 3, 4, 5, 6, 7, 8, 9):
                    assert exchange(peer, body(schema, bytes([channel]) * 32, channel=channel))[0] == 3
                assert exchange(peer, body(schema, signer.public, 8, b'', channel=99))[0] == 3
                assert exchange(peer, body(schema, bytes([1]) * 32, 5, b'', channel=1))[0] == 0
            assert exchange(owner, mutation(schema, signer, challenge, 0, epoch, 0))[0] == 0
            for peer in subscribers:
                event = response(peer, True)
                assert event[:2] == (0, 99) and event[2] in (4, 5) and event[4] == 1, event
            # Buffered requests and blocked sends must not starve other clients.
            slow = peers.enter_context(connect(port))
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.sendall(packet * 6000)
            worst = 0
            for request in range(20):
                tick = time.monotonic()
                assert exchange(warm, heartbeat(schema, request + 1))[0] == 0
                worst = max(worst, time.monotonic() - tick)
            result['heartbeat_max_seconds_under_load'] = worst
            assert worst < 1, 'active client starved unrelated heartbeat'

            # Close subscribers while a silent write wakes their connections;
            # reuse slots immediately and ensure no old subscription follows.
            delta = struct.pack('!QIQB', 0, 1, 42, 0)
            owner.sendall(blob(mutation(schema, signer, challenge, 1, epoch, 1, delta, operation=6)))
            for peer in subscribers:
                peer.close()
            replacements = [peers.enter_context(connect(port)) for _ in subscribers]
            for peer in replacements:
                assert exchange(peer, heartbeat(schema))[0] == 0
            for seq in range(2, 8):
                owner.sendall(blob(mutation(schema, signer, challenge, seq, epoch, 0, delta, operation=6)))
            assert exchange(owner, heartbeat(schema))[0] == 0
            for peer in replacements:
                assert exchange(peer, heartbeat(schema))[0] == 0, 'recycled slot inherited subscription'

            # Idle/partial connections expire without touching a live connection.
            expiring = peers.enter_context(connect(port))
            expiring.sendall(struct.pack('!I', 4096) + b'L')
            deadline = time.monotonic() + 32
            while time.monotonic() < deadline:
                assert exchange(warm, heartbeat(schema))[0] == 0
                if readable([expiring], 1):
                    assert expiring.recv(1) == b''
                    break
            else:
                raise AssertionError('idle timer did not close partial request')
            # Shutdown cancels thousands of receives, more than the SQ size.
            live = [peers.enter_context(connect(port)) for _ in range(count)]
            for peer in live:
                peer.sendall(b'\0\0')
            assert exchange(warm, heartbeat(schema))[0] == 0
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
    return result


def memory_pressure(binary, schema):
    with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as peers:
        process, port = start(binary, directory, ['--memory-budget', str(2 * 1024 * 1024)])
        try:
            warm = peers.enter_context(connect(port))
            assert exchange(warm, heartbeat(schema))[0] == 0
            stalled = [peers.enter_context(connect(port)) for _ in range(12)]
            for peer in stalled:
                peer.sendall(struct.pack('!I', 512 * 1024))
            deadline = time.monotonic() + 5
            closed = 0
            pending = set(stalled)
            while closed < 8 and time.monotonic() < deadline:
                for peer in readable(pending, .1):
                    assert peer.recv(1) == b''
                    pending.remove(peer)
                    closed += 1
            assert closed >= 8, 'buffer memory budget did not reject excess allocations'
            assert exchange(warm, heartbeat(schema))[0] == 0
            for peer in stalled:
                peer.close()
            # Recycled buffers and slots remain usable after the pressure clears.
            for _ in range(20):
                with connect(port) as peer:
                    assert exchange(peer, heartbeat(schema))[0] == 0
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('server')
    parser.add_argument('--clients', type=int, default=2048)
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[1] / 'datafriend/schema.h'
    schema = re.search(r'DATAFRIEND_SCHEMA\s+"([^"]+)"', header.read_text()).group(1)
    result = run(os.path.abspath(args.server), schema, args.clients)
    memory_pressure(os.path.abspath(args.server), schema)
    print(json.dumps({'status': 'pass', **result, 'checks': [
        'connection growth beyond eight', 'idle CPU and RSS', 'SQ batch exhaustion',
        'absent namespace subscriptions', 'channel relocation', 'fanout',
        'slow-reader fairness', 'disconnect/reuse with queued notifications',
        'idle deadline refresh/expiry', 'shutdown with thousands of pending receives',
        'memory-budget rejection and recovery']}))


if __name__ == '__main__':
    main()
