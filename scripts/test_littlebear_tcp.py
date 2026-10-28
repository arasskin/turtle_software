#!/usr/bin/env python3
"""Exercise public TCP sync and Ed25519 writes against the production binary."""
import argparse
import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import tempfile
import time


class Signer:
    sodium = ctypes.CDLL(ctypes.util.find_library('sodium'))
    sodium.crypto_sign_seed_keypair.argtypes = [ctypes.c_void_p] * 3
    sodium.crypto_sign_detached.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                          ctypes.c_ulonglong, ctypes.c_void_p]
    assert sodium.sodium_init() >= 0

    def __init__(self, seed):
        public = ctypes.create_string_buffer(32)
        self.secret = ctypes.create_string_buffer(64)
        assert self.sodium.crypto_sign_seed_keypair(public, self.secret, bytes([seed]) * 32) == 0
        self.public = public.raw

    def sign(self, body):
        signature = ctypes.create_string_buffer(64)
        assert self.sodium.crypto_sign_detached(signature, None, body, len(body), self.secret) == 0
        return signature.raw


def blob(value):
    return struct.pack('!I', len(value)) + value


def body(schema, name, operation=0, payload=b'\0', epoch=0, revision=0, magic=b'LBS\5', channel=1, request_id=1):
    return magic + struct.pack('!II', request_id, channel) + bytes([operation]) + blob(name) + blob(schema.encode()) + struct.pack('!QQ', epoch, revision) + blob(payload)


def mutation(schema, signer, challenge, sequence, epoch, revision, payload=struct.pack('!I', 0), name=None, channel=1, operation=3):
    data = body(schema, signer.public if name is None else name, operation, payload, epoch, revision, channel=channel)
    data += challenge + struct.pack('!Q', sequence)
    return data + signer.sign(data)


def exact(peer, size):
    data = bytearray()
    while len(data) < size:
        chunk = peer.recv(size - len(data))
        if not chunk:
            raise EOFError('server closed before complete reply')
        data.extend(chunk)
    return bytes(data)


def response(peer, envelope=False):
    length, = struct.unpack('!I', exact(peer, 4))
    assert 25 <= length <= 8 * 1024 * 1024
    packet = exact(peer, length)
    request_id, channel = struct.unpack('!II', packet[:8])
    data = packet[8:]
    challenge = data[17:49] if data[0] & 0x80 else b''
    assert not (data[0] & 0x80) or len(challenge) == 32
    result = (data[0] & 0x7f, *struct.unpack('!QQ', data[1:17]), data[17 + len(challenge):], challenge)
    return (request_id, channel, *result) if envelope else result


def exchange(peer, data):
    peer.sendall(blob(data))
    return response(peer)


def connect(port):
    peer = socket.create_connection(('127.0.0.1', port), timeout=5)
    peer.settimeout(5)
    return peer


def start(binary, directory, extra=()):
    process = subprocess.Popen([binary, '--values', str(Path(directory) / 'values'), '--port', '0', *extra],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        if not select.select([process.stdout], [], [], 10)[0]:
            raise TimeoutError('server startup timed out')
        line = process.stdout.readline()
        if not line.startswith('Littlebear TCP listening '):
            raise RuntimeError('server failed: ' + line + process.stderr.read())
        return process, int(line.rsplit(':', 1)[1])
    except BaseException:
        process.kill()
        process.communicate()
        raise


def stop(process):
    process.terminate()
    _, errors = process.communicate(timeout=10)
    assert process.returncode == 0, errors
    assert 'AddressSanitizer' not in errors and 'runtime error:' not in errors, errors


def run(binary, schema):
    alice, bob = Signer(7), Signer(8)
    with tempfile.TemporaryDirectory() as directory:
        process, port = start(binary, directory)
        try:
            subprocess.run(['python3', str(Path(__file__).resolve().parents[1] / 'deploy/healthcheck.py'),
                            '--port', str(port)], check=True, timeout=5)
            # IDs are opaque nonzero uint32 values, not array offsets. Grow
            # beyond 64, remove entries from the middle, and reuse their IDs.
            with connect(port) as many:
                missing = bytes([249]) * 32
                other_missing = bytes([250]) * 32
                channels = [0x80000000 + i * 127 for i in range(159)] + [0xffffffff]
                for channel in channels:
                    assert exchange(many, body(schema, missing, channel=channel))[0] == 3
                for channel in channels[::2]:
                    assert exchange(many, body(schema, missing, 5, b'', channel=channel))[0] == 0
                for channel in channels[1::2]:
                    assert exchange(many, body(schema, missing, channel=channel))[0] == 3
                for channel in channels[::2]:
                    assert exchange(many, body(schema, other_missing, channel=channel))[0] == 3
                assert exchange(many, body(schema, other_missing, channel=channels[-1]))[0] == 2
                assert exchange(many, body(schema, missing, channel=0))[0] == 2
            with connect(port) as peer:
                read = body(schema, alice.public)
                status, epoch, revision, _, no_challenge = exchange(peer, read)
                assert status == 3 and revision == 0 and not no_challenge, 'missing namespace became present'
                def inspect():
                    with connect(port) as observer:
                        return exchange(observer, read)
                # Public push intent cannot create a namespace or allocate owner state.
                frame = blob(body(schema, alice.public, payload=b'\1'))
                peer.sendall(frame[:3])
                time.sleep(.01)
                peer.sendall(frame[3:] + blob(read))
                missing_push = response(peer)
                assert missing_push[0] == 3 and len(missing_push[4]) == 32
                missing_pull = response(peer)
                assert missing_pull[0] == 3 and not missing_pull[4]
                assert exchange(peer, body(schema, alice.public, magic=b'LBS\3'))[0] == 2
                assert exchange(peer, body('wrong-schema', alice.public))[0] == 2
                assert exchange(peer, body(schema, b'alice'))[0] == 2
                assert exchange(peer, body(schema, alice.public, 3, struct.pack('!I', 0), epoch))[0] == 2
                unsigned_challenge = b'\0' * 32
                assert exchange(peer, mutation(schema, alice, unsigned_challenge, 0, epoch, revision))[0] == 2
                # Operation 6 is a signed delta, never an unsigned challenge request.
                assert exchange(peer, body(schema, alice.public, 6, b'', epoch, revision))[0] == 2
                status, _, _, _, challenge = exchange(peer, body(schema, alice.public, payload=b'\1'))
                assert status == 3 and len(challenge) == 32
                # Asking for a challenge itself must not create the namespace.
                assert inspect()[0] == 3
                bad_owner = mutation(schema, bob, challenge, 0, epoch, revision, name=alice.public)
                assert exchange(peer, bad_owner)[0] == 2, 'wrong owner accepted'
                create = mutation(schema, alice, challenge, 0, epoch, revision)
                # Every truncation and a corrupted signature are rejected.
                for size in range(13, len(create)):
                    assert exchange(peer, create[:size])[0] == 2
                assert exchange(peer, create[:-1] + bytes([create[-1] ^ 1]))[0] == 2
                status, _, revision, _, _ = exchange(peer, create)
                assert status == 0 and revision == 1, 'signed empty creation failed'
                assert exchange(peer, create)[0] == 2, 'same-connection replay accepted'
                # A signed delete is a write even when the key is absent.
                deletion = struct.pack('!IQB', 1, 42, 0)
                update = mutation(schema, alice, challenge, 1, epoch, revision, deletion)
                # Tamper with full namespace, boot nonce, revision, payload, nonce and sequence.
                for offset in [17, 12 + 1 + 4 + 32 + 4 + len(schema),
                               len(update) - 104 - len(deletion) - 8,
                               len(update) - 104 - 2, len(update) - 104, len(update) - 65]:
                    altered = bytearray(update)
                    altered[offset] ^= 1
                    assert exchange(peer, bytes(altered))[0] == 2, 'tampered signed request accepted'
                assert inspect()[2] == revision, 'rejected write mutated storage'
                assert exchange(peer, mutation(schema, alice, challenge, 2, epoch, revision, deletion))[0] == 2
                status, _, revision, _, _ = exchange(peer, update)
                assert status == 0 and revision == 2, 'next signed batch failed'
                with connect(port) as another:
                    assert exchange(another, update)[0] == 2, 'cross-connection replay accepted'
                    # All readers see an existing empty namespace without authentication.
                    assert exchange(another, read)[0] == 0
                    assert exchange(another, body(schema, alice.public, 2, struct.pack('!I', 0), epoch, revision))[0] == 0
                # Correct signatures on stale revisions do not bypass consistency.
                stale = mutation(schema, alice, challenge, 2, epoch, 0, deletion)
                assert exchange(peer, stale)[0] == 1
                assert exchange(peer, stale)[0] == 2, 'stale attempt was not consumed'
                replay = mutation(schema, alice, challenge, 3, epoch, revision, deletion)
                # Existing namespaces defer the nonce to a differing reconciliation
                # reply. Equal empty offers preserve the initial-message optimization.
                start_push = exchange(peer, body(schema, alice.public, payload=b'\1'))
                assert start_push[0] == 0 and not start_push[4]
                empty_offer = b'LBR\3\1\0\0'
                equal = exchange(peer, body(schema, alice.public, 1, empty_offer, epoch, revision))
                assert equal[0] == 0 and equal[3] == b'LBR\3\2\0\0' and not equal[4]
                offer = b'LBR\3\1\0\1' + struct.pack('!QQ', 42, 1)
                different = exchange(peer, body(schema, alice.public, 1, offer, epoch, revision))
                assert different[0] == 0 and different[3] == b'LBR\3\2\0\0'
                new_challenge = different[4]
                assert len(new_challenge) == 32
                # Later reconciliation replies neither repeat nor rotate the nonce.
                again = exchange(peer, body(schema, alice.public, 1, offer, epoch, revision))
                assert again[0] == 0 and not again[4]
                assert new_challenge != challenge
                assert exchange(peer, replay)[0] == 2, 'rotated challenge accepted'
                # A new challenge starts its sequence at zero, even on an existing connection.
                fresh = mutation(schema, alice, new_challenge, 0, epoch, revision, deletion)
                assert exchange(peer, fresh)[0] == 0
                # Signing bob's namespace with bob's key cannot change alice's namespace.
                bob_challenge = exchange(peer, body(schema, bob.public, payload=b'\1', channel=2))[4]
                assert exchange(peer, mutation(schema, bob, bob_challenge, 0, epoch, 0, channel=2))[0] == 0
                assert inspect()[2] == 3
                # Two namespaces share one persistent subscriber connection.
                with connect(port) as subscriber:
                    subscriber.sendall(blob(body(schema, alice.public, 4, b'', channel=11, request_id=101)))
                    opening = response(subscriber, True)
                    assert opening[:3] == (101, 11, 0) and opening[5].startswith(b'LBR\3')
                    old_revision = opening[4]
                    subscriber.sendall(blob(body(schema, bob.public, 4, b'', channel=12, request_id=102)))
                    assert response(subscriber, True)[:3] == (102, 12, 0)
                    # Subscriber sends nothing: the owner update must itself wake it.
                    assert exchange(peer, mutation(schema, alice, new_challenge, 1, epoch, 3, deletion))[0] == 0
                    event = response(subscriber, True)
                    assert event[:3] == (0, 11, 4) and event[4] == 4 and event[5].startswith(b'LBR\3')
                    # A stale fetch directly carries the new opening, not just a retry bit.
                    subscriber.sendall(blob(body(schema, alice.public, 2, struct.pack('!I', 0), epoch, old_revision,
                                                 channel=11, request_id=103)))
                    restarted = response(subscriber, True)
                    assert restarted[:3] == (103, 11, 1) and restarted[4] == 4 and restarted[5].startswith(b'LBR\3')
                    subscriber.sendall(blob(body(schema, alice.public, 5, b'', channel=11, request_id=104)))
                    assert response(subscriber, True)[:3] == (104, 11, 0)
                    assert exchange(peer, mutation(schema, alice, new_challenge, 2, epoch, 4, deletion))[0] == 0
                    # Unsubscribed alice cannot generate an event; bob remains live.
                    subscriber.sendall(blob(body(schema, bob.public, 0, b'\0', channel=12, request_id=105)))
                    assert response(subscriber, True)[:3] == (105, 12, 0)

            # Optimistic updates reuse the connection challenge, authenticate the
            # target fingerprint, and forward records without a subscriber fetch.
            fast_owner = Signer(9)
            with connect(port) as subscriber, connect(port) as owner:
                opening = exchange(owner, body(schema, fast_owner.public, payload=b'\1'))
                epoch, challenge = opening[1], opening[4]
                assert exchange(owner, mutation(schema, fast_owner, challenge, 0, epoch, 0))[0] == 0
                subscribed = exchange(subscriber, body(schema, fast_owner.public, 8, b''))
                assert subscribed[0] == 0
                # Deleting a nonexistent record leaves the empty fingerprint at zero.
                delta = struct.pack('!QIQB', 0, 1, 42, 0)
                request = mutation(schema, fast_owner, challenge, 1, epoch, 1, delta, operation=6)
                assert exchange(owner, request[:-1] + bytes([request[-1] ^ 1]))[0] == 2
                # Subscriber precedes owner in the event loop. Silent writes
                # must wake publication without the 100 ms idle-loop timeout.
                sent_at = time.monotonic()
                owner.sendall(blob(request))
                event = response(subscriber, True)
                assert time.monotonic()-sent_at < .09, 'publication waited for idle timeout'
                assert event[:3] == (0, 1, 5) and event[5] == delta
                assert exchange(owner, request)[0] == 2, 'optimistic replay accepted'
                with connect(port) as replay_peer:
                    assert exchange(replay_peer, request)[0] == 2, 'optimistic cross-connection replay accepted'
                # Success must produce no frame. A heartbeat sent afterwards is
                # an ordered test barrier; the next response must belong to it.
                heartbeat = body(schema, b'', 7, b'', channel=0, request_id=700)
                owner.sendall(blob(heartbeat))
                assert response(owner, True)[:3] == (700, 0, 0), 'unexpected upload success reply'
                # Stale server revision is harmless when the fingerprint matches.
                owner.sendall(blob(mutation(schema, fast_owner, challenge, 2, epoch, 0, delta, operation=6)))
                event = response(subscriber, True)
                assert event[:3] == (0, 1, 5) and event[5] == delta
                # Pipeline a mismatch and another write: one recovery event, no
                # reply per write. Later queued writes wait for a fresh start.
                wrong_target = struct.pack('!QI', 1, 0)
                owner.sendall(blob(mutation(schema, fast_owner, challenge, 3, epoch, 0, wrong_target, operation=6)) +
                              blob(mutation(schema, fast_owner, challenge, 4, epoch, 0, delta, operation=6)) + blob(heartbeat))
                mismatch = response(owner, True)
                assert mismatch[:3] == (0, 1, 6) and not mismatch[5], 'missing recovery event'
                assert response(owner, True)[:3] == (700, 0, 0), 'duplicate recovery event'
                started = exchange(owner, body(schema, fast_owner.public, payload=b'\1'))
                offer = b'LBR\3\1\0\1' + struct.pack('!QQ', 42, 1)
                opening = exchange(owner, body(schema, fast_owner.public, 1, offer, started[1], started[2]))
                assert opening[4], 'missing replacement challenge'
                owner.sendall(blob(mutation(schema, fast_owner, opening[4], 0, started[1], started[2], delta, operation=6)) + blob(heartbeat))
                assert response(owner, True)[:3] == (700, 0, 0), 'recovery did not resume silent writes'

            with connect(port) as peer:
                peer.sendall(struct.pack('!I', 8 * 1024 * 1024 + 1))
                assert peer.recv(1) == b''
            for _ in range(64):
                with connect(port) as peer:
                    peer.sendall(b'\0\0')
            time.sleep(.1)
            with connect(port) as peer:
                assert inspect()[0] == 0
            stalled = [connect(port) for _ in range(4)]
            for peer in stalled:
                peer.sendall(struct.pack('!I', 1024) + b'L')
            time.sleep(.1)
            stop(process)
            for peer in stalled:
                peer.close()
            process, port = start(binary, directory)
            with connect(port) as peer:
                assert exchange(peer, fresh)[0] == 2, 'restart replay accepted'
                assert inspect()[0] == 3, 'cache unexpectedly retained namespace'
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('server')
    header = Path(__file__).resolve().parents[1] / 'datafriend/schema.h'
    schema = re.search(r'DATAFRIEND_SCHEMA "([^"]+)"', header.read_text()).group(1)
    parser.add_argument('--schema', default=schema)
    args = parser.parse_args()
    run(os.path.abspath(args.server), args.schema)
    print(json.dumps({'status': 'pass', 'checks': ['public reads', 'signed namespace creation',
        'wrong owner', 'unsigned write denial', 'full request tampering', 'truncation',
        'sequence and connection replay', 'challenge rotation', 'restart replay', 'stale write consumption',
        'server-initiated subscription', 'multiplexed namespaces', 'stale fetch with opening',
        'unsubscribe', '160 dynamic channels with sparse IDs and slot reuse',
        'namespace isolation', 'piggybacked absent/reconciliation challenges',
        'signed optimistic updates', 'optimistic replay/tamper rejection', 'immediate subscriber forwarding before owner slot',
        'silent uploads and pipelined mismatch recovery', 'optimistic stale revision with matching fingerprint',
        'no standalone challenge operation', 'equal offer without challenge', 'fragmented/coalesced frames', 'size/schema rejection',
        'disconnect/slot reuse', 'shutdown with pending receives/accept']}))


if __name__ == '__main__':
    main()
