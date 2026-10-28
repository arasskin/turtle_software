# Optimistic fact synchronization

The original protocol was deployed on 2026-10-08. The shared-delivery changes
described below are local changes from 2026-10-27, not yet deployed.
Local edits remain blocking; disk flushing
and socket I/O remain asynchronous. No UI API change is required.

- Owner batches carry exact committed put/delete changes and the resulting full
  namespace fingerprint. Successful batches need one signed message, with no success reply.
- Server queues verified batches for current opted-in subscribers. Subscribers
  verify the full fingerprint inside the transaction before updating indexes or
  publishing views. Mismatches use ordinary negentropy.
- Owner tracking stores at most 4096 changed keys, with revision tags. Large
  batches use existing paged reconciliation. The server shares one retained copy
  among current subscribers, with bounded per-connection delivery queues; slow
  subscribers reconcile when their unsent chain must be dropped.
- Existing connection challenges and monotonically increasing signature sequences
  authenticate optimistic writes. Reconnection establishes a new challenge.

## Wire and transaction behavior

Operation 6 is a signed optimistic apply: target XOR fingerprint plus ordinary
key/presence/record entries. Every field is covered by the owner signature. A
matching result produces no reply. A mismatch produces unsolicited status 6
(reconciliation required), then the server consumes but discards subsequent signed
deltas until the owner's fresh start. TCP ordering makes that start a barrier
behind queued writes. Reconciliation uses the latest local snapshot. New local
edits remain in the journal for the following batch. The fingerprint
comes from AELMDB's cached aggregate, not a scan or a new hash algorithm.

Operation 8 subscribes to direct batch delivery. The server emits unsolicited
status 5 with its resulting fingerprint and changes when the subscriber's queued
or announced revision matches the batch's base revision. Successive batches stay
ordered even while the socket is busy. Shared bodies are released after sends
finish, including partial-send/cancellation cleanup. No history is retained for
future subscribers. Each connection has a 16 MiB logical backlog limit, and all
shared bodies and queue nodes count against the server's memory budget. Overflow
or a revision gap drops only the affected subscription's unsent chain and uses an
ordinary reconciliation opening. There is no global 8 MiB delta-cache purge.
Operation 4 retains ordinary opening-only subscriptions.

The native client queues delivered deltas in wire order instead of replacing an
unapplied delta with the next one. Pump budget exhaustion preserves that queue;
each applied delta checks its own target fingerprint before publishing. Queued
updates schedule an immediate continuation without another socket read event.
The queue is bounded to 16 MiB per channel including node storage. Overflow
retains only the latest update and uses the existing fingerprint fallback if
predecessors are missing. Ordinary reconciliation openings and server epoch
changes supersede the old chain. Updates the server could not queue, or updates
missed while disconnected, can still require reconciliation.

The subscriber stages changes in one AELMDB write transaction and compares the
resulting aggregate before changing query indexes or results. A mismatch aborts
that transaction, then starts negentropy without publishing the candidate state.
The server retains its existing eventual-coherence behavior if file I/O fails
mid-batch; the following exchange repairs it. It does not acquire a new durable
transaction guarantee from this optimization.

The initial connection/reconnect still performs normal reconciliation and gets
an authentication challenge. Subsequent matching writes reuse that challenge
with increasing signature sequence numbers. Owners queue new batches without
waiting for a success reply. Socket backpressure coalesces edits in the bounded
journal; reconnect always reconciles. `settled()` means local output drained and
no pending reconciliation, not confirmed delivery. Ordinary reconciliation pages
retain request/reply exchanges. Upload timing samples end at socket handoff,
with no round-trip duration. The document demo has no receipt facts or tracking.

The phone numbers below predate acknowledgment removal; see
[datafriend-noack.md](datafriend-noack.md) for the new measurements.

## Validation

- Five core and five demo model tests pass. New component cases cover duplicate
  batches, deletions, mismatch rollback including live views, owner fallback
  against a retained snapshot, intervening edits and journal overflow.
- Production TCP/auth checks pass: optimistic signatures, tampered requests,
  same/cross-connection replay, direct forwarding, fingerprint mismatch opening,
  and stale revisions whose resulting fingerprint matches.
- Native real-TCP tests assert exactly one upload and a direct publication for a
  normal edit, and exercise corrupt-replica recovery, large/paged batches,
  concurrent edits, offline/reconnect and CRDT stale drafts.
- Server TCP/auth and native integration also pass under ASan/UBSan on native
  Linux ARM64. x86 emulation could compile but could not initialize io_uring;
  the x86 release was subsequently checked on the actual public server.
- Public x86-64 release: `d891d8b86795aafed8162f71644356c67e040b4e-20261008-3`.
  This is a local working-tree build, not a GitHub Actions deployment.

[Native checks](validation/datafriend-optimistic-native.json),
[sanitized server integration](validation/datafriend-optimistic-asan.json).

## Phone measurements

Public server, 1,000-character seed, 67 edits per phone every 150 ms, concurrent
editing. These are individual runs on a variable network, not a controlled study.
Acknowledgment includes the peer's application-level merge and return trip;
one-way remote presentation was not measured in these runs. The subsequent
[calibrated one-way benchmark](datafriend-one-way.md) measures receive and display
separately from the return acknowledgment.

| No-probe run: median / p95 | Moto G7 | iPhone 13 |
| --- | ---: | ---: |
| Previous acknowledgment | 1166 / 1333 ms | 969 / 1336 ms |
| Optimistic acknowledgment | **330 / 768 ms** | **317 / 718 ms** |
| Optimistic local edit | 0.837 / 1.115 ms | 0.803 / 1.054 ms |

All 134 edits acknowledged, identical final text, no pending work/errors/restarts,
zero optimistic mismatches. Each phone completed 90 optimistic uploads; subscriber
direct publications were 91/90. Both presented at about 60 Hz with full coverage
and no >25 ms display gaps. Local edit maxima: 1.233/1.358 ms. Acknowledgment
maxima: 835/869 ms, so the tail is not yet consistently low.

[No-probe run](benchmarks/datafriend-optimistic-sync/3463faefc5b543e3a832552313dd74f4-summary.json),
[previous no-probe baseline](benchmarks/datafriend-document-yrs/d1abe2a2199c41d98b049d4c6870681a-summary.json).

A separate diagnostic run with a heartbeat every 250 ms measured acknowledgment
medians 338/335 ms, p95 415/450 ms, versus the earlier equally instrumented
983/952 ms. Every phone sent only **one start, one reconcile, one normal apply and
one fetch**, all during initialization. Thereafter Android/iPhone completed
87/97 optimistic uploads and 98/87 direct publications, with zero mismatches.
Optimistic upload median response time was 113/100 ms; median heartbeat remained
about 100 ms. This confirms that the gain came from removing exchanges, not a
faster public network path.

[Diagnostic run](benchmarks/datafriend-optimistic-sync/ee32fe2aa06244c7a13e156285954563-summary.json),
[previous diagnostic baseline](benchmarks/datafriend-request-latency/ec74d07a494a41dd98759c487f90df9a-summary.json).

The [offline run](benchmarks/datafriend-optimistic-sync/56fc63f0f3024680b2ba00ae95dfd4b2-summary.json)
disconnected both phones for five seconds while editing. Both reconnected,
converged and acknowledged all 134 edits. Android exercised two optimistic
mismatch fallbacks and three exchange restarts; both finished without errors or
pending work. This validates fallback on the actual phones, not just in tests.
Both phones are restored to the interactive `yrs-document-demo` room.

The faster path sends more intermediate document states: ordinary-run signed
batches rose from 31/36 to 91/91. Summed phone traffic rose from 382 KB to 905 KB
(sent plus received, counting both endpoints), about 2.4 times as much. CRDT state
and materialized text are still transmitted in full. This is the latency/batching
tradeoff in this demo; it is not an increase in record count per document.

Remaining latency includes two public-network legs in each direction, frame
scheduling and waiting behind an already outstanding owner request. Exact tail
attribution requires more detailed tracing; this change does not add pipelined
owner requests or incremental CRDT payloads.
