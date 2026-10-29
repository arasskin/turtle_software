# External Littlebear server

The existing DrawDo deployment is at **147.182.181.182:7777**. Its workflow and
bootstrap instructions are in the sibling `../drawdo/.github/workflows/deploy.yml`
and `../drawdo/deploy/`. The reusable `deploy.sh` and `package.py` here were copied
from that deployment; the packager now reads this repository's schema directly.

The SSH deployment user is `drawdo-deploy`. On the original development machine,
the authorized identity is `~/.ssh/dodo`. Do not copy that private key into source
archives. Use the existing verified SSH host-key record (`StrictHostKeyChecking=yes`).

## Layout

- `/opt/littlebear/incoming/`: uploaded archives and source build directories.
- `/opt/littlebear/releases/RELEASE_ID/`: immutable packaged server releases.
- `/opt/littlebear/current`: symlink to the active release.
- `/var/lib/littlebear/littlebear.values`: the service's values cache.
- `littlebear.service`: runs as the unprivileged `littlebear` user.

The deployment account can restart/stop only this service through its existing
sudo rule. It has no general administrative privileges. Existing clients reconnect
after a service restart and owners restore their namespaces.

## Upload, compile, and test current sources

Package the current working tree's `littlebear/`, `containers/`,
`third_party/xxhash/`, `datafriend/schema.h`, `scripts/`, and `deploy/`. Exclude
build output, Git metadata, credentials, and app data. Keep their relative paths:
Littlebear uses sibling headers. Record SHA-256 hashes of the uploaded sources
when deploying uncommitted working-tree changes; a Git commit alone is insufficient
provenance for such a snapshot.

Upload the archive into a unique incoming path, extract into a new directory,
and build there while the existing service continues running. The current Ubuntu
x86_64 host already has `cc`, `g++`, Make, Python, liburing-dev, and libsodium-dev.
Use `-j 1` to limit build pressure on the small host.

From the uploaded source root on the server:

```sh
make -C littlebear server test -j 1
python3 scripts/test_littlebear_tcp.py littlebear/build/server
python3 scripts/test_littlebear_delivery.py littlebear/build/server
python3 scripts/test_littlebear_websocket.py littlebear/build/server
```

The transport checks start their own temporary test servers and do not use the
production service's values file. Do not run another writable server against
`/var/lib/littlebear/littlebear.values` while the service is running.

## Package and activate

Choose a unique release ID in the form `40_HEX_GIT_HEAD-NUMBER-NUMBER`; for a manual
working-tree build the middle number can be the build timestamp. Keep the source
manifest alongside the source build so the exact bytes remain auditable.

```sh
python3 deploy/package.py littlebear/build/server \
  /opt/littlebear/incoming/RELEASE_ID.tar.gz
bash deploy/deploy.sh RELEASE_ID
```

`deploy.sh` serializes deployment, validates the package's checksums and native ELF
architecture, preserves the prior release, switches `current`, restarts the service,
and checks the actual protocol. It automatically restores the previous symlink if
startup checks fail. The package includes a compatibility launcher for the existing
unit's `/opt/littlebear/current/littlebear/core` path. A fresh installation can use
the `deploy/littlebear.service` unit directly after an administrator provisions the
service account and dependencies using DrawDo's bootstrap instructions.

Verify from the development machine:

```sh
python3 deploy/healthcheck.py --host 147.182.181.182 --port 7777
./examples/canvas/test --external
```

The health check is a read-only LBS5 heartbeat. The canvas integration test proves
signed writes, subscription imports, native persistence, and reconnect through
the externally reachable endpoint. These checks serve different purposes.

## Enabling WebSockets

New builds support an optional `--ws-port PORT` in addition to the existing TCP
`--port`. The recorded deployment below predates this feature; deploying a new
binary alone does not enable the second listener. Add the flag to the service's
actual server command and restart after testing the build. The legacy `core`
compatibility launcher in `deploy.sh` currently supplies only TCP flags, so a
service using that launcher must instead use the packaged `littlebear/build/server`
path with explicit arguments (or update its launcher configuration).

For a fresh service binding both listeners only to loopback, an example command is:

```ini
ExecStart=/opt/littlebear/current/littlebear/build/server --bind 127.0.0.1 --port 7777 --ws-port 7779 --values /var/lib/littlebear/littlebear.values
```

Choose the bind address to suit existing TCP clients; `--bind` applies to both
listeners. If TCP must remain publicly reachable with `--bind 0.0.0.0`, restrict
the WebSocket port to the proxy with the host firewall. Changing the systemd unit
or firewall requires the server administrator; the deployment user's restart-only
sudo rule does not grant those configuration permissions.

For browser applications served over HTTPS, use an HTTPS reverse proxy with a
certificate for your hostname. Forward `wss://YOUR_HOST/sync` to the plain local
listener. An nginx location inside an existing TLS-enabled server block is:

```nginx
location /sync {
    proxy_pass http://127.0.0.1:7779;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header Host $host;
    proxy_read_timeout 60s;
}
```

Clients still send an LBS heartbeat at intervals shorter than Littlebear's
30-second inactivity timeout. Proxying does not replace Littlebear's signed write
authorization. See the [transport contract](../littlebear/README.md#websocket-listener)
for binary framing and limits. Test through the final `wss://` URL after enabling
TLS; local transport regressions do not validate a proxy or certificate setup.

## Deployment on 2026-10-28

- Release: `564750d2f6c7617f2b7fd51c5fb4d2dc295c5575-1793211721-1`.
- Source build: `/opt/littlebear/incoming/canvas-source-1793211721/`.
- Its `SOURCE_MANIFEST.json` records and verifies all 55 uploaded source files.
- Server binary SHA-256: `c8b2372e3a0654da07776ab931900764605f51c924e92f1712713844df49d101`.
- Previous release retained: `633d74ad56dde61f8a60ffb108e50fac93fcf2ac-40531874540-1`.
- Native build, all ten component suites, signed TCP checks, and delivery tests passed
  before activation. The service restarted successfully and both local and external
  protocol health checks passed. The canvas's independent-host external integration
  also passed, including offline edits and reconnect.

Littlebear provides public reads and signed owner writes. Its native endpoint is
plain TCP, without TLS or authenticated server responses. It is a disposable sync
cache, not the only durable copy of application data. Preserve native owner data
and identities; server cache contents can be reconstructed from connected owners.
