#!/usr/bin/env python3
"""Run the real serial CLI against the real native protocol core over a PTY."""
from pathlib import Path
import os
import pty
import subprocess
import sys
import tempfile
import tty

repo = Path(__file__).resolve().parents[1]
binary = repo / 'build/native-server'
binary.parent.mkdir(exist_ok=True)
subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1',
                '-I', str(repo / 'lib/Tpdd'), str(repo / 'lib/Tpdd/Tpdd.cpp'),
                str(repo / 'tests/native_server.cpp'), '-o', str(binary)], check=True)
with tempfile.TemporaryDirectory(prefix='tpdd-serial-') as temp:
    root = Path(temp)
    a, b = root / 'bank0', root / 'bank1'
    a.mkdir(); b.mkdir()
    expected = bytes(range(256)) * 2
    (a / 'HELLO.DO').write_bytes(expected)
    (b / 'OTHER.DO').write_bytes(b'bank one\r\n')
    master, slave = pty.openpty()
    tty.setraw(slave)
    server = subprocess.Popen([str(binary), str(a), str(b)], stdin=master, stdout=master)
    try:
        base = [sys.executable, str(repo / 'tools/serial-smoke.py'), '--port', os.ttyname(slave)]
        subprocess.run(base + ['--read', 'HELLO.DO', '--expected', str(a / 'HELLO.DO')], check=True)
        subprocess.run(base + ['--bank', '1', '--read', 'OTHER.DO', '--expected', str(b / 'OTHER.DO')], check=True)
        subprocess.run(base + ['--roundtrip'], check=True)
        assert sorted(p.name for p in a.iterdir()) == ['HELLO.DO']
        assert (a / 'HELLO.DO').read_bytes() == expected
        assert (b / 'OTHER.DO').read_bytes() == b'bank one\r\n'
        print('PASS: serial CLI / PTY / native-core end-to-end tests; test file cleaned up')
    finally:
        server.terminate(); server.wait(timeout=5)
        os.close(master); os.close(slave)
