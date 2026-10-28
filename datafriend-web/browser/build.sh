#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
component=$(pwd -P)
cd "$component"
output=${DATAFRIEND_WEB_OUTPUT:-"$component/build/browser"}
mkdir -p "$output"
"${EMCC:-emcc}" -std=c99 -O2 -DMDB_HASH_SIZE=8 \
  -Iinclude -Isrc \
  -I../third_party/aelmdb/src -I../third_party/monocypher -I../third_party/xxhash \
  browser/bridge.c browser/flush.c \
  src/db.c src/memory.c src/engine.c \
  src/query.c src/result.c src/sync.c \
  src/identity.c src/network.c \
  ../third_party/aelmdb/src/mdb.c ../third_party/aelmdb/src/midl.c \
  ../third_party/monocypher/monocypher.c ../third_party/monocypher/monocypher-ed25519.c \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sALLOW_MEMORY_GROWTH=1 \
  -sENVIRONMENT=web,worker,node -sFILESYSTEM=1 \
  -sEXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString \
  -sINCOMING_MODULE_JS_API=websocket,locateFile,print,printErr,wasmBinary \
  -sWEBSOCKET_SUBPROTOCOL=binary \
  -o "$output/datafriend.mjs"
cp browser/client.mjs "$output/client.mjs"
