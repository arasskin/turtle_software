#!/usr/bin/env python3
"""Check Littlebear's operation boundaries without compiling or opening Visualize."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
BEAR = ROOT / 'littlebear'
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)


def check():
    files = {path.resolve() for pattern in ('*.h', '*.c') for path in BEAR.glob(pattern)}
    graph = {}

    def discover(path):
        if path in graph:
            return
        graph[path] = []
        for name in INCLUDE.findall(path.read_text()):
            dependency = (path.parent / name).resolve()
            assert dependency.is_file(), f'{path.name}: missing include {name}'
            graph[path].append(dependency)
            discover(dependency)

    for path in sorted(files):
        discover(path)

    active, complete = set(), set()

    def acyclic(path):
        assert path not in active, f'Include cycle through {path.relative_to(ROOT)}'
        if path in complete:
            return
        active.add(path)
        for dependency in graph[path]:
            acyclic(dependency)
        active.remove(path)
        complete.add(path)

    for path in graph:
        acyclic(path)

    def reachable(path):
        result = set()
        pending = list(graph[path])
        while pending:
            dependency = pending.pop()
            if dependency not in result:
                result.add(dependency)
                pending.extend(graph[dependency])
        return result

    operations = {path for path in files if re.fullmatch(r'op_[0-8]_.+\.h', path.name)}
    assert len(operations) == 8
    assert {int(path.name.split('_')[1]) for path in operations} == {0, 1, 2, 3, 5, 6, 7, 8}
    server = (BEAR / 'server.c').resolve()
    assert operations <= set(graph[server]), 'Server must directly import all eight operations'
    assert (BEAR / 'websocket.h').resolve() in graph[server], 'Server owns the WebSocket gate'
    models = (BEAR / 'models.h').resolve()
    assert models in graph[server], 'Server imports the shared specification directly'
    assert not set(graph[models]) & files, 'Models must not import Littlebear implementation tools'
    assert all(models in graph[op] for op in operations), 'Operations import the shared specification directly'
    for retired in ('request_envelope.h', 'operation_ids.h', 'operation_result.h', 'namespace_state.h'):
        assert not (BEAR / retired).exists(), f'{retired} should be coalesced into its owner'
    assert re.search(r'static bool parse_request_envelope\(', server.read_text()), \
        'The envelope parser is private to server'
    assert all('parse_request_envelope' not in p.read_text() for p in files - {server}), \
        'Only server consumes the envelope parser'
    forbidden = {BEAR / name for name in (
        'server.c', 'client_connections.h', 'websocket.h', 'storage_uring.h')}
    for operation in operations:
        dependencies = reachable(operation)
        assert not dependencies & operations, f'{operation.name} imports another operation'
        assert not dependencies & forbidden, f'{operation.name} imports a dispatcher or transport'
    for tool in files - operations - {server}:
        assert not reachable(tool) & operations, f'Tool {tool.name} imports operation policy'
    # Lifetime and write scheduling must not choose reconciliation or subscription responses.
    for name in ('request_lifetime.h', 'write_record_batch.h'):
        assert not reachable(BEAR / name) & {BEAR / dependency for dependency in (
            'exchange.h', 'reconcile.h', 'index_sync.h', 'subscriber_delivery.h',
            'owner_authorization.h')}, f'{name} imports operation response policy'
    lifetime = (BEAR / 'request_lifetime.h').read_text()
    assert not re.search(r'\blb_reply_(?:header|rejected)\s*\(', lifetime), \
        'Request lifetime must not construct protocol responses'
    assert not re.search(r'\blb_task_(?:restart|reject|unlock)\s*\(', lifetime), \
        'Operation response and namespace-release policy belongs to operations'
    writer = (BEAR / 'write_record_batch.h').read_text()
    assert not re.search(r'\blb_task\b|\blb_directory\b|\blb_store_create_namespace\b|->revision|->writing\s*=', writer), \
        'Batch execution must not own requests, namespace creation, guards or revisions'
    heartbeat = BEAR / 'op_7_heartbeat.h'
    assert not reachable(heartbeat) & {BEAR / 'owner_authorization.h', BEAR / 'subscriber_delivery.h'}
    for source in files:
        assert not re.search(r'\blb_(?:service|operation_context)\b', source.read_text()), \
            f'{source.name} reintroduces an ambient service/context'
        assert not any((BEAR / 'tests') in dependency.parents for dependency in reachable(source)), \
            f'Production file {source.name} imports test support'
        if source.suffix != '.h':
            continue
        body = re.sub(r'/\*.*?\*/|//[^\n]*', '', source.read_text(), flags=re.DOTALL)
        lines = [line.strip() for line in body.splitlines() if line.strip()]
        forwarding = any(line.startswith('#include') for line in lines) and all(
            re.fullmatch(r'#\s*(?:ifndef\s+\w+|define\s+\w+|endif|include\s+["<].+[">])', line)
            for line in lines)
        assert not forwarding, f'{source.name} is a forwarding-only header; import its target directly'
    print('PASS: shared models, server-owned parser, eight direct operations, acyclic tools and explicit operation boundaries')


if __name__ == '__main__':
    check()
