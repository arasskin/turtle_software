#!/usr/bin/env python3
"""Two independent native hosts, separate disks, real external Littlebear.

Uses dedicated test identities; never reads or modifies the demo's databases.
"""
import argparse
import json
import socket
import subprocess
import tempfile
import time
import uuid
from pathlib import Path
from integration import ROOT, Client, stroke, stop


def health(port):
    import urllib.request
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/health', timeout=2) as response:
        return json.load(response)


def launch(data, port, endpoint, alias, peer, log, offline=False):
    process = subprocess.Popen([str(ROOT / 'build/janet'), 'server.janet', '--port', str(port),
        '--data', str(data), '--sync', endpoint, '--identity', alias, '--peers', peer,
        '--offline', str(offline).lower()], cwd=ROOT, stdout=log, stderr=log)
    try:
        for _ in range(150):
            if process.poll() is not None:
                log.seek(0)
                raise RuntimeError(log.read().decode())
            try:
                if health(port)['ok']:
                    return process
            except OSError:
                time.sleep(.05)
        raise RuntimeError('Native host did not start')
    except BaseException:
        stop(process)
        raise


def public_key(alias):
    return subprocess.check_output([str(ROOT / 'build/janet'), 'server.janet', '--print-key', alias],
                                   cwd=ROOT, text=True).strip()


def wait_error(client):
    for _ in range(100):
        message = client.read()
        if message['type'] == 'error':
            return message
    raise AssertionError('Expected write rejection')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint', default='147.182.181.182:7777')
    args = parser.parse_args()
    run_id = uuid.uuid4().hex[:12]
    aliases = [f'turtle-canvas-test-{run_id}-alice', f'turtle-canvas-test-{run_id}-bob']
    keys = [public_key(alias) for alias in aliases]
    assert keys[0] != keys[1]
    with socket.socket() as a, socket.socket() as b:
        a.bind(('127.0.0.1', 0)); b.bind(('127.0.0.1', 0))
        ports = [a.getsockname()[1], b.getsockname()[1]]
    processes = [None, None]
    clients = []
    with tempfile.TemporaryDirectory(prefix='canvas-native-sync-') as folder, \
            tempfile.TemporaryFile() as log_a, tempfile.TemporaryFile() as log_b:
        dirs = [Path(folder) / 'alice', Path(folder) / 'bob']
        logs = [log_a, log_b]
        def start(i, offline=False):
            processes[i] = launch(dirs[i], ports[i], args.endpoint, aliases[i], keys[1-i], logs[i], offline)
        def join(i, actor, cursor):
            client = Client(ports[i], (dirs[i] / 'invite-key').read_text(), actor, cursor)
            client.sock.settimeout(20)
            clients.append(client)
            return client
        try:
            start(0); start(1)
            alice = join(0, 'alice-001', 'cursor-alice')
            bob = join(1, 'bob-00001', 'cursor-bob')
            # Simultaneous overlapping records, exchanged only through the remote relay.
            alice.put(stroke('alice-crossing', [[10, 10], [40, 40]]))
            bob.put(stroke('bob-crossing', [[10, 40], [40, 10]]))
            alice.wait_record('alice-crossing', 1)
            alice.wait_record('bob-crossing', 1)
            bob.wait_record('alice-crossing', 1)
            bob.wait_record('bob-crossing', 1)
            for record_id in ['alice-crossing', 'bob-crossing']:
                assert alice.records[record_id] == bob.records[record_id]
            assert alice.records['alice-crossing']['writer'] == keys[0]
            assert alice.records['bob-crossing']['writer'] == keys[1]
            # Updates extend the same native entity, and native watches publish to browsers.
            alice.put(stroke('alice-crossing', [[10, 10], [40, 40], [80, 80]], 2))
            assert len(bob.wait_record('alice-crossing', 2)['points']) == 3
            cursor = dict(id='cursor-alice', kind='cursor', version=1, name='Alice',
                          color='#608cae', x=90, y=60, active=True, online=True)
            alice.put(cursor)
            assert bob.wait_record('cursor-alice', 1)['x'] == 90
            alice.put(dict(cursor, version=2, x=150))
            assert bob.wait_record('cursor-alice', 2)['x'] == 150
            bob.put(dict(stroke('alice-crossing', [[10, 10], [40, 40], [80, 80]], 3), deleted=True))
            assert 'another native host' in wait_error(bob)['message']
            alice.put(dict(stroke('alice-crossing', [[10, 10], [40, 40], [80, 80]], 3), deleted=True, complete=True))
            assert bob.wait_record('alice-crossing', 3)['deleted']
            assert not bob.records['bob-crossing']['deleted']
            # Restart one independent host without networking; keep drawing on both.
            bob.close(); stop(processes[1]); processes[1] = None
            start(1, offline=True)
            offline_bob = join(1, 'bob-00001', 'cursor-bob-offline')
            offline_bob.put(stroke('bob-offline', [[60, 60], [80, 80]]))
            offline_bob.wait_record('bob-offline', 1)
            assert not health(ports[1])['sync']['connected']
            alice.put(stroke('alice-while-bob-offline', [[20, 60], [60, 20]]))
            alice.wait_record('alice-while-bob-offline', 1)
            observer = join(0, 'observer-001', 'cursor-observer')
            assert 'bob-offline' not in observer.records
            observer.close()
            # Reconnect restores both directions without replacing overlapping work.
            offline_bob.close(); stop(processes[1]); processes[1] = None
            start(1)
            restored_bob = join(1, 'bob-00001', 'cursor-bob-return')
            alice.wait_record('bob-offline', 1)
            restored_bob.wait_record('alice-while-bob-offline', 1)
            expected = ['alice-crossing', 'bob-crossing', 'bob-offline', 'alice-while-bob-offline']
            for record_id in expected:
                restored_bob.wait_record(record_id, 3 if record_id == 'alice-crossing' else 1)
                assert restored_bob.records[record_id] == alice.records[record_id]
            for i in [0, 1]:
                state = health(ports[i])['sync']
                assert state['connected'] and state['sent'] > 0 and state['received'] > 0
                assert (dirs[i] / 'owner/data.mdb').is_file()
                assert (dirs[i] / f'replicas/{keys[1-i]}/data.mdb').is_file()
            print(json.dumps({'status': 'pass', 'endpoint': args.endpoint, 'checks': [
                'independent native databases', 'bidirectional external sync', 'overlapping writers',
                'incremental stroke records', 'cursor records', 'writer ownership', 'undo propagation',
                'offline local edits', 'restart and reconnect', 'converged replicas']}, indent=2))
        finally:
            for client in clients:
                client.close()
            for process in processes:
                if process:
                    stop(process)


if __name__ == '__main__':
    main()
