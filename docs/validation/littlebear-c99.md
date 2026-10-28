# Littlebear C99 port — 2026-10-13

The server now builds with `cc -std=c99 -pedantic-errors`. The runtime uses explicit
request state and error returns. C++ remains in Datafriend and in model tests;
those callers use adapters over the same C99 tree, allocator and reconciliation
implementation. LBS5/LBR3 and the existing authentication model are preserved.

Validation used macOS arm64/Apple Clang 17 and Ubuntu 24.04 arm64/GCC 13 in the
local `drawdo-c99-validation` Docker image. Linux server tests ran with one CPU
and 512 MiB RAM. Nothing was deployed.

Passed:

- Seven Littlebear suites, including a directly compiled C99 model/lifetime test.
- Allocation-failure sweeps, randomized tree/table/storage models, bounded
  reconciliation, parser inputs and real io_uring storage completions.
- All seven Littlebear suites with AddressSanitizer and UBSan.
- Five Datafriend native suites on macOS and Linux.
- Signed TCP protocol tests, including replay/tamper rejection, optimistic
  uploads, pipelined mismatch recovery, subscriptions and pending-I/O shutdown.
- Native Datafriend TCP integration against release and sanitized servers,
  including paging, multiplexing, Unicode, deletes, offline/reconnect and edits
  during synchronization.

## Narrow performance comparison

Five interleaved runs of 100,000 records on the local Mac. Both versions use the
same deterministic keys, values, hashes and operation sequence, compiled with
`-O3 -DNDEBUG`. Values below are medians, not a production throughput claim.

| Measurement | Saved C++ implementation | C99 implementation |
| --- | ---: | ---: |
| Insert, ns/record | 183.73 | 178.59 |
| Lookup, ns/record | 84.94 | 85.27 |
| Erase, ns/record | 175.64 | 169.08 |
| Tree pool reserved bytes at full size | 3,997,696 | 3,997,696 |
| Lookup checksum | 5,000,050,000 | 5,000,050,000 |

The generated harnesses, original-source snapshot and raw results are under
`littlebear-benchmarks/build/c99-comparison/`. The tree layout remains unchanged:
24-byte records and node headers, 8-byte slots, and 24-byte namespace tree headers.

On Linux arm64, GNU `size` reported 101,882 bytes of text/data/bss for the saved
C++ server and 68,867 bytes for the C99 server (about 32% smaller). This measures
executable sections, not process RSS. `ldd` lists libc, liburing and libsodium;
there is no C++ runtime dependency.

## Reproduce correctness checks

```sh
python3 datafriend/tool/generate_attributes.py --check
make -C littlebear BUILD=build/c99-linux server test debug sanitize
python3 scripts/test_littlebear_tcp.py littlebear/build/c99-linux/server
cmake -S datafriend -B datafriend/build/c99-linux -DCMAKE_BUILD_TYPE=Release
cmake --build datafriend/build/c99-linux -j 2
ctest --test-dir datafriend/build/c99-linux --output-on-failure
python3 scripts/test_native_sync.py littlebear/build/c99-linux/server \
  datafriend/build/c99-linux/transport_test
ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=16 UBSAN_OPTIONS=halt_on_error=1 \
  python3 scripts/test_littlebear_tcp.py littlebear/build/c99-linux/server-debug
ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=16 UBSAN_OPTIONS=halt_on_error=1 \
  python3 scripts/test_native_sync.py littlebear/build/c99-linux/server-debug \
  datafriend/build/c99-linux/transport_test
```
