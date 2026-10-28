"""Check the LBS5 request loop with a read-only connection heartbeat."""
import argparse
from pathlib import Path
import re
import socket
import struct
import sys


def blob(value):
    return struct.pack('!I', len(value)) + value


def exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise ValueError('Incomplete health response')
        data.extend(part)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=7777)
    parser.add_argument('--host', default='127.0.0.1')
    args = parser.parse_args()
    try:
        header = Path(__file__).resolve().parents[1] / 'datafriend/schema.h'
        deployed_schema = Path(__file__).resolve().parents[1] / 'SCHEMA'
        schema = (deployed_schema.read_text().strip() if deployed_schema.exists() else
                  re.search(r'DATAFRIEND_SCHEMA "([0-9a-f]+)"', header.read_text()).group(1)).encode()
        request = (b'LBS\5' + struct.pack('!IIB', 1, 0, 7) + blob(b'') + blob(schema)
                   + struct.pack('!QQ', 0, 0) + blob(b''))
        with socket.create_connection((args.host, args.port), timeout=2) as connection:
            connection.sendall(blob(request))
            length, = struct.unpack('!I', exact(connection, 4))
            if length != 25 or exact(connection, length) != struct.pack('!IIBQQ', 1, 0, 0, 0, 0):
                raise ValueError('Unexpected health response')
    except (OSError, ValueError, AttributeError) as error:
        print(f'Littlebear health check failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
