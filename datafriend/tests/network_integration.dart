import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import '../../src/app/lib/data/datafriend.dart';

void check(bool ok, String message) {
  if (!ok) throw StateError(message);
}

// Pause one fetch before it reaches Littlebear, so an owner update deterministically
// overtakes it. Responses, including the new opening, flow without delay.
Future<void> restartDuringFetch(
  int port,
  Datafriend replica,
  Future<void> Function() update,
  bool Function() converged,
) async {
  final listener = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
  final sockets = <Socket>[];
  final tasks = <Future<void>>[];
  var overtaken = false;
  Future<void> relay(Socket client) async {
    sockets.add(client);
    final upstream = await Socket.connect('localhost', port);
    sockets.add(upstream);
    upstream.listen(client.add, onError: (Object _) => client.destroy());
    final buffered = BytesBuilder(copy: false);
    var pending = Uint8List(0);
    await for (final chunk in client) {
      buffered
        ..add(pending)
        ..add(chunk);
      pending = buffered.takeBytes();
      while (pending.length >= 4) {
        final length = ByteData.sublistView(pending).getUint32(0);
        if (pending.length < length + 4) break;
        final packet = Uint8List.sublistView(pending, 0, length + 4);
        pending = Uint8List.sublistView(pending, length + 4);
        if (!overtaken && packet[16] == 2) {
          overtaken = true;
          await update();
        }
        upstream.add(packet);
      }
    }
  }

  final accepted = listener.listen((client) {
    tasks.add(relay(client));
  });
  try {
    final network = replica.sync.subscribe(
      LittlebearConfig(host: '127.0.0.1', port: listener.port),
    );
    await network.syncNow();
    check(
      overtaken && converged(),
      'stale fetch did not restart to latest state',
    );
    check(
      network.exchanges == 1,
      'stale snapshot was published before restart',
    );
  } finally {
    replica.disconnect();
    await accepted.cancel();
    await listener.close();
    for (final socket in sockets) {
      socket.destroy();
    }
    await Future.wait(tasks);
  }
}

Future<void> framingCheck(int port, String namespace) async {
  final socket = await Socket.connect('localhost', port);
  final chunks = StreamIterator<Uint8List>(socket);
  var pending = Uint8List(0), offset = 0;
  Future<Uint8List> read(int n) async {
    final output = BytesBuilder();
    while (output.length < n) {
      if (offset == pending.length) {
        if (!await chunks.moveNext()) throw StateError('Unexpected EOF');
        pending = chunks.current;
        offset = 0;
      }
      final remaining = n - output.length;
      final count = remaining < pending.length - offset
          ? remaining
          : pending.length - offset;
      output.add(Uint8List.sublistView(pending, offset, offset + count));
      offset += count;
    }
    return output.takeBytes();
  }

  Uint8List number(int x) =>
      (ByteData(4)..setUint32(0, x)).buffer.asUint8List();
  final body = BytesBuilder()..add([76, 66, 83, 5, 0, 0, 0, 1, 0, 0, 0, 1, 0]);
  for (final bytes in [
    [
      for (var i = 0; i < 64; i += 2)
        int.parse(namespace.substring(i, i + 2), radix: 16),
    ],
    utf8.encode(datafriendAttributeSchema),
  ]) {
    body
      ..add(number(bytes.length))
      ..add(bytes);
  }
  body
    ..add(Uint8List(16))
    ..add(number(1))
    ..addByte(0);
  final bytes = body.takeBytes();
  final framed =
      (BytesBuilder()
            ..add(number(bytes.length))
            ..add(bytes))
          .takeBytes();
  try {
    // Split a frame header, then coalesce the tail and an entire second request.
    socket.add(framed.sublist(0, 3));
    await socket.flush();
    await Future<void>.delayed(const Duration(milliseconds: 10));
    socket
      ..add(framed.sublist(3))
      ..add(framed);
    await socket.flush();
    for (var i = 0; i < 2; i++) {
      final length = ByteData.sublistView(await read(4)).getUint32(0);
      final reply = await read(length);
      check(
        reply.length == 25 && reply[8] == 0,
        'fragmented/coalesced request failed',
      );
    }
    socket.add(number(8 * 1024 * 1024 + 1));
    await socket.flush();
    try {
      check(
        !await chunks.moveNext().timeout(const Duration(seconds: 5)),
        'oversized frame accepted',
      );
    } on SocketException {
      /* Peer rejected the frame by closing TCP. */
    }
  } finally {
    socket.destroy();
    await chunks.cancel();
  }
}

Future<void> main(List<String> args) async {
  final root = Directory(args[0]);
  final repo = args[1];
  Process? server;
  var port = 0;
  Future<void> start() async {
    final process = server = await Process.start(
      Platform.environment['LITTLEBEAR_SERVER'] ??
          '$repo/littlebear/build/server',
      ['--values', '${root.path}/values', '--port', '$port'],
    );
    process.stderr.transform(utf8.decoder).listen(stderr.write);
    final line = await process.stdout
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .first
        .timeout(const Duration(seconds: 10));
    port = int.parse(line.split(':').last);
    // Docker Desktop may print the listener announcement before its host port
    // forwarding is ready, especially immediately after restarting a container.
    for (var attempt = 0; ; attempt++) {
      try {
        final probe = await Socket.connect(
          'localhost',
          port,
          timeout: const Duration(milliseconds: 250),
        );
        probe.destroy();
        break;
      } on SocketException {
        if (attempt == 99) rethrow;
        await Future<void>.delayed(const Duration(milliseconds: 50));
      }
    }
  }

  Future<void> stop() async {
    server!.kill();
    await server!.exitCode;
    server = null;
  }

  final identityLibrary =
      Platform.environment['DATAFRIEND_LIBRARY'] ??
      '$repo/datafriend/build/native/libdatafriend.${Platform.isMacOS ? 'dylib' : 'so'}';
  final identity = LittlebearIdentity.ephemeralForTesting(
    libraryPath: identityLibrary,
  );
  final otherIdentity = LittlebearIdentity.ephemeralForTesting(
    libraryPath: identityLibrary,
  );
  LittlebearConfig config() => LittlebearConfig(host: 'localhost', port: port);
  Datafriend open(String name, {String? namespace}) => Datafriend.open(
    '${root.path}/$name',
    namespace: namespace ?? identity.namespace,
    libraryPath: identityLibrary,
  );
  Future<void> until(bool Function() condition, String message) async {
    final watch = Stopwatch()..start();
    while (!condition()) {
      if (watch.elapsed > const Duration(seconds: 25))
        throw StateError(message);
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }
  }

  Datafriend? owner, replica, secondOwner, secondReplica;
  try {
    await start();
    owner = open('owner');
    replica = open('replica');
    var notifications = 0;
    final view = replica.subscribe(
      const DatafriendQuery.all(),
      onChanged: () {
        notifications++;
      },
    );
    final live = view.result();
    // Subscribe before the namespace exists. No read or subscription creates it.
    replica.edit(add: [Triple(7777, Attribute.title, 'last known state')]);
    final pull = replica.sync.subscribe(config());
    await pull.syncNow();
    check(
      pull.waitingForOwner && live.length == 1,
      'absent subscription erased replica',
    );
    owner.edit(
      add: [
        for (var i = 0; i < 1000; i++) ...[
          Triple(i, Attribute.title, 'task $i — ☀️'),
          Triple(i, Attribute.done, false),
        ],
      ],
    );
    var push = owner.sync.push(config(), identity: identity);
    check(
      push.sharesConnectionWith(pull),
      'namespaces opened separate sockets',
    );
    await push.syncNow();
    // No pull.syncNow(): the server must send the opening and wake the subscriber.
    await until(
      () => live.length == 2000 && !pull.syncing,
      'server did not initiate initial sync',
    );
    check(
      push.connectionsOpened == 1 && pull.challenges == 0,
      'connection reuse/public read',
    );
    final beforeChallenges = push.challenges,
        beforeNotifications = notifications;
    await push.syncNow();
    check(
      push.challenges == beforeChallenges,
      'equal push requested authentication',
    );
    await Future<void>.delayed(const Duration(milliseconds: 100));
    check(notifications == beforeNotifications, 'equal sync notified');
    owner.edit(add: [Triple(1, Attribute.title, 'updated')]);
    owner.deleteEntity(2);
    await push.syncNow();
    await until(
      () => live.length == 1998 && live.value(1, 'title') == 'updated',
      'automatic edit/delete subscription',
    );
    await framingCheck(port, identity.namespace);
    final idleRequests = pull.requests;
    await Future<void>.delayed(const Duration(milliseconds: 250));
    check(pull.requests == idleRequests, 'idle subscriber polled');
    // A second namespace uses the same TCP socket and stays independent.
    secondOwner = open('empty-owner', namespace: otherIdentity.namespace);
    secondReplica = open('empty-replica', namespace: otherIdentity.namespace);
    secondReplica.edit(add: [Triple(1, Attribute.title, 'old local state')]);
    final secondView = secondReplica
        .subscribe(const DatafriendQuery.all())
        .result();
    final secondPull = secondReplica.sync.subscribe(config());
    await secondPull.syncNow();
    final secondPush = secondOwner.sync.push(config(), identity: otherIdentity);
    check(
      secondPush.sharesConnectionWith(pull) &&
          secondPull.sharesConnectionWith(push),
      'multiplexing failed',
    );
    await secondPush.syncNow();
    await until(
      () => secondView.length == 0 && !secondPull.waitingForOwner,
      'signed empty creation did not clear replica',
    );
    check(
      secondPush.challenges == 1 && secondPush.signedBatches == 1,
      'empty creation authentication',
    );
    secondOwner.edit(
      add: [
        for (var i = 0; i < 160; i++) Triple(i, Attribute.title, 'x' * 60000),
      ],
    );
    await secondPush.syncNow();
    await until(
      () => secondView.length == 160 && !secondPull.syncing,
      'paged subscription failed',
    );
    check(secondView.value(159, 'title') == 'x' * 60000, 'paged value corrupt');
    check(
      secondPush.challenges == 2 && secondPush.signedBatches > 2,
      'paged push added challenges',
    );
    check(
      secondPush.requests ==
          secondPush.exchanges +
              secondPush.reconciliationRequests +
              secondPush.signedBatches,
      'push added authentication round trips',
    );
    check(live.length == 1998, 'namespace isolation failed');
    final delayedReplica = open(
      'delayed-replica',
      namespace: otherIdentity.namespace,
    );
    final delayedView = delayedReplica
        .subscribe(const DatafriendQuery.all())
        .result();
    try {
      await restartDuringFetch(
        port,
        delayedReplica,
        () async {
          secondOwner!.edit(
            add: [Triple(159, Attribute.title, 'changed during fetch')],
          );
          await secondPush.syncNow();
        },
        () =>
            delayedView.length == 160 &&
            delayedView.value(159, 'title') == 'changed during fetch',
      );
    } finally {
      delayedView.close();
      delayedReplica.close();
    }
    // Updates during transfer must settle on the newest owner state.
    secondOwner.edit(add: [Triple(0, Attribute.title, 'intermediate')]);
    final changing = secondPush.syncNow();
    secondOwner.edit(add: [Triple(0, Attribute.title, 'latest')]);
    await changing;
    await until(
      () => secondView.value(0, 'title') == 'latest',
      'update during sync failed to converge',
    );
    // Unsubscribe only releases that channel, leaving other namespaces connected.
    secondReplica.disconnect();
    final retained = secondView.value(0, 'title');
    secondOwner.edit(add: [Triple(0, Attribute.title, 'after unsubscribe')]);
    await secondPush.syncNow();
    await Future<void>.delayed(const Duration(milliseconds: 100));
    check(
      secondView.value(0, 'title') == retained,
      'unsubscribed namespace changed',
    );
    secondView.close();
    secondReplica.close();
    secondReplica = null;
    secondOwner.close();
    secondOwner = null;
    // The subscriber reconnects and waits without losing its last known facts.
    owner.disconnect();
    await stop();
    owner.edit(add: [Triple(1, Attribute.title, 'offline edit')]);
    push = owner.sync.push(config(), identity: identity);
    var offline = false;
    try {
      await push.syncNow();
    } catch (_) {
      offline = true;
    }
    check(offline, 'offline push unexpectedly succeeded');
    owner.disconnect();
    await start();
    await until(
      () => pull.waitingForOwner,
      'subscriber did not resubscribe after restart',
    );
    check(
      live.length == 1998 && live.value(1, 'title') == 'updated',
      'restart erased last known state',
    );
    push = owner.sync.push(config(), identity: identity);
    await push.syncNow();
    await until(
      () => live.value(1, 'title') == 'offline edit',
      'restart did not repopulate subscriber',
    );
    owner.edit(add: [Triple(1, Attribute.title, 'automatic push')]);
    await until(
      () => live.value(1, 'title') == 'automatic push',
      'automatic owner/subscriber scheduling failed',
    );
    // Closing while a refresh is in flight must not touch freed native state.
    final closing = pull.syncNow();
    replica.close();
    replica = null;
    try {
      await closing;
    } catch (_) {}
    live.close();
    print(
      jsonEncode({
        'status': 'pass',
        'facts': 1998,
        'notifications': notifications,
        'connectionsOpened': push.connectionsOpened,
        'subscriberRestarts': pull.restarts,
        'checks': [
          'persistent multiplexed connection',
          'server-initiated reconciliation',
          'absent namespace preservation',
          'signed empty creation',
          'no subscriber polling',
          'paged transfer',
          'updates during sync',
          'stale fetch discarded before publication',
          'unsubscribe isolation',
          'public reads',
          'piggybacked challenges',
          'zero authentication round trips',
          'framing',
          'Unicode',
          'same-key updates',
          'deletion',
          'automatic push',
          'offline edits',
          'reconnect/resubscribe',
          'close during I/O',
        ],
      }),
    );
  } finally {
    owner?.close();
    replica?.close();
    secondOwner?.close();
    secondReplica?.close();
    identity.close();
    otherIdentity.close();
    if (server != null) await stop();
  }
}
