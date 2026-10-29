#!/usr/bin/env python3
"""Package the tested Linux executable and protocol health check, without sources."""
import argparse
import hashlib
import io
from pathlib import Path
import re
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
header = (root / 'datafriend/schema.h').read_text()
schema = re.search(r'DATAFRIEND_SCHEMA "([0-9a-f]{64})"', header).group(1)
files = {
    'littlebear/build/server': (args.binary.read_bytes(), 0o755),
    'deploy/healthcheck.py': ((root/'deploy/healthcheck.py').read_bytes(), 0o644),
    'SCHEMA': ((schema+'\n').encode(), 0o644),
}
files['SHA256SUMS'] = (''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                             for name, (data, _) in files.items()).encode(), 0o644)
with tarfile.open(args.output, 'w:gz') as archive:
    for name, (data, mode) in files.items():
        entry = tarfile.TarInfo(name)
        entry.size, entry.mode = len(data), mode
        archive.addfile(entry, io.BytesIO(data))
