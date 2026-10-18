#!/usr/bin/env python3
"""TPDD file-mode smoke test. Read-only unless --roundtrip is explicitly set."""
import argparse
import hashlib
from pathlib import Path
import secrets
import sys
import serial


class Client:
    def __init__(self, port, bank):
        self.port, self.bank = port, bank

    def request(self, command, payload=b'', banked=True):
        command |= (self.bank << 6) if banked else 0
        body = bytes([command, len(payload)]) + payload
        self.port.write(b'ZZ' + body + bytes([(~sum(body)) & 255]))
        # Do not tcdrain(): an unasserted hardware CTS could block it forever.
        # read() has a deadline and also waits for the request to reach the peer.
        header = self.port.read(2)
        if len(header) != 2:
            raise RuntimeError('No reply: check service, cable, CTS and selected bank')
        tail = self.port.read(header[1] + 1)
        if len(tail) != header[1] + 1 or sum(header + tail) & 255 != 255:
            raise RuntimeError('Truncated reply or checksum error')
        if header[0] == 0x12 and tail[0] != 0:
            raise RuntimeError(f'Drive error 0x{tail[0]:02x} for command 0x{command:02x}')
        return header[0], tail[:-1]

    def select(self, name='', action=0):
        data = name.encode('ascii')
        if len(data) > 24:
            raise ValueError('Filename too long')
        kind, payload = self.request(0, data.ljust(24, b' ') + b'F' + bytes([action]))
        if kind != 0x11 or len(payload) != 28:
            raise RuntimeError('Unexpected directory response')
        return payload

    def read_file(self, name):
        self.select(name)
        self.request(1, b'\x03')
        result = bytearray()
        try:
            while True:
                kind, payload = self.request(3)
                if kind != 0x10 or len(payload) > 128:
                    raise RuntimeError('Unexpected file-read response')
                result.extend(payload)
                if len(result) > 65534:
                    raise RuntimeError('File exceeds supported limit')
                if len(payload) < 128:
                    return bytes(result)
        finally:
            self.request(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--bank', type=int, choices=[0, 1], default=0)
    parser.add_argument('--read', metavar='NAME.DO')
    parser.add_argument('--expected', type=Path, help='Compare --read with this local binary file')
    parser.add_argument('--roundtrip', action='store_true', help='Create, verify and delete a uniquely named test file; requires a writable bank')
    parser.add_argument('--no-cts', action='store_true', help='Diagnostic PC-side flow-control override; board CTS is configured separately')
    args = parser.parse_args()
    if args.expected and not args.read:
        parser.error('--expected requires --read')
    with serial.Serial(args.port, 19200, timeout=3, write_timeout=3, rtscts=not args.no_cts) as port:
        port.reset_input_buffer()
        client = Client(port, args.bank)
        client.request(7, banked=False)
        kind, version = client.request(0x23, banked=False)
        if kind != 0x14 or len(version) != 15:
            raise RuntimeError('Unexpected TPDD2 version response')
        print('Status and TPDD2 version: PASS')
        entry = client.select(action=1)
        count = 0
        while entry[0]:
            count += 1
            if count > 40:
                raise RuntimeError('Directory did not terminate within 40 entries')
            print(entry[:24].decode('ascii', 'replace').rstrip(), int.from_bytes(entry[25:27], 'big'))
            entry = client.select(action=2)
        print(f'Bank {args.bank} directory: PASS ({count} files)')
        if args.read:
            data = client.read_file(args.read)
            if args.expected and data != args.expected.read_bytes():
                raise RuntimeError('Read data differs from local expected file')
            print(f'Read {len(data)} bytes: PASS, SHA256={hashlib.sha256(data).hexdigest()}')
        if args.roundtrip:
            name = 'T' + secrets.token_hex(3)[:5].upper() + '.DO'
            if client.select(name)[0]:
                raise RuntimeError('Test name exists; nothing overwritten; retry')
            test_data = bytes(range(256)) * 4
            client.request(1, b'\x01')
            try:
                for offset in range(0, len(test_data), 128):
                    client.request(4, test_data[offset:offset + 128])
            finally:
                client.request(2)
            if client.read_file(name) != test_data:
                raise RuntimeError(f'Round trip failed; {name} retained for inspection')
            client.select(name)
            client.request(5)
            print('Binary write/read/delete round trip: PASS (1024 bytes)')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, ValueError, OSError, serial.SerialException) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        sys.exit(1)
