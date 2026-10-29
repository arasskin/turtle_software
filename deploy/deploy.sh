#!/usr/bin/env bash
# Invoked over SSH as drawdo-deploy after uploading the tested binary bundle.
set -euo pipefail

release_id=${1:?Expected a release ID}
if [[ ! $release_id =~ ^[0-9a-f]{40}-[0-9]+-[0-9]+$ ]]; then
  echo "Invalid release ID" >&2
  exit 1
fi

deploy_root=/opt/littlebear
archive="$deploy_root/incoming/$release_id.tar.gz"
release="$deploy_root/releases/$release_id"
next_link="$deploy_root/current.next.$$"

# Also serialize deployments started outside GitHub Actions.
exec 9> "$deploy_root/deploy.lock"
flock -w 120 9
trap 'rm -f "$archive" "$next_link"' EXIT

test -f "$archive"
mkdir "$release"
tar --extract --gzip --file="$archive" --directory="$release" --no-same-owner
test -x "$release/littlebear/build/server"
test -f "$release/SCHEMA"
(cd "$release" && sha256sum --check SHA256SUMS)
# Reject cross-built binaries before disturbing the running service.
python3 - "$release/littlebear/build/server" <<'PY'
import platform, struct, sys
with open(sys.argv[1], 'rb') as binary:
    header = binary.read(20)
expected = {'x86_64': 62, 'aarch64': 183}.get(platform.machine())
if (len(header) != 20 or header[:6] != b'\x7fELF\x02\x01' or expected is None or
        struct.unpack_from('<H', header, 18)[0] != expected):
    raise SystemExit('Server binary does not match the host architecture')
PY
# Support the currently installed unit's legacy executable path as well as the
# new unit, without needing privileged service-file changes for each release.
cat > "$release/littlebear/core" <<'WRAPPER'
#!/bin/sh
exec "$(dirname "$0")/build/server" --bind 0.0.0.0 --port 7777 --values /var/lib/littlebear/littlebear.values
WRAPPER
chmod 755 "$release/littlebear/core"
printf '%s\n' "${release_id:0:40}" > "$release/REVISION"

previous=$(readlink -f "$deploy_root/current" || true)
ln -s "$release" "$next_link"
mv -Tf "$next_link" "$deploy_root/current"

check_release() {
  local attempt
  for attempt in {1..10}; do
    echo "Checking Littlebear startup ($attempt/10)..."
    if systemctl is-active --quiet littlebear.service &&
       python3 "$release/deploy/healthcheck.py"; then
      return 0
    fi
    sleep 1
  done
  return 1
}

if sudo -n /usr/bin/systemctl restart littlebear.service && check_release; then
  echo "Littlebear deployed: $release_id"
else
  echo "Startup check failed; restoring the previous release." >&2
  systemctl --no-pager --full status littlebear.service || true
  if [[ -n $previous && -d $previous ]]; then
    ln -s "$previous" "$next_link"
    mv -Tf "$next_link" "$deploy_root/current"
    # A failing executable may have exhausted systemd's restart rate limit.
    # Wait for its window to expire without requiring additional sudo privileges.
    for attempt in {1..13}; do
      if sudo -n /usr/bin/systemctl restart littlebear.service; then break; fi
      sleep 5
    done
    systemctl is-active --quiet littlebear.service
  else
    sudo -n /usr/bin/systemctl stop littlebear.service
    rm -f "$deploy_root/current"
  fi
  exit 1
fi
