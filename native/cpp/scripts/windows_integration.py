#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import concurrent.futures
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
cli = app / 'archivefs.exe'

def invoke(*args, timeout=180, ok=True):
    result = subprocess.run([str(cli), *map(str, args)], capture_output=True, timeout=timeout)
    if (result.returncode == 0) != ok:
        raise AssertionError((result.args, result.returncode, result.stdout, result.stderr))
    return result.stdout.decode('utf-8').strip()

def wait_until(fn, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if fn():
                return
        except OSError:
            pass
        time.sleep(.1)
    raise TimeoutError('condition not satisfied')

rng = random.Random(2951)
with tempfile.TemporaryDirectory(prefix='WinArchiveFSPro-') as tmp:
    root = Path(tmp)
    src = root / 'sources'
    src.mkdir()
    data = rng.randbytes(1 << 18) + b'\r\n\x1a\0' * 8192
    (src / '文件.bin').write_bytes(data)
    other = src / '目录'
    other.mkdir()
    (other / 'empty.txt').write_bytes(b'')
    (other / 'hello.txt').write_bytes('你好 WinArchiveFSPro\r\n'.encode())
    images = []
    for fmt, suffix in [('SquashFS','squashfs'), ('DwarFS','dwarfs'), ('EROFS','erofs')]:
        for preset in range(1,9):
            image = root / f'{fmt}-{preset}.{suffix}'
            invoke('create', fmt, preset, image, src / '文件.bin', other)
            assert invoke('info',image).startswith(fmt + ' ')
            assert '文件.bin' in invoke('list',image)
            assert invoke('verify',image).startswith('Verified 3 files')
            for start, size in [(0,1),(1023,513),(65530,37),(len(data)-8,16),(len(data),64)]:
                assert invoke('read', image, '文件.bin', start, size) == data[start:start+size].hex().upper()
            output = root / f'extract-{fmt}-{preset}'
            invoke('unpack',image,output)
            assert (output/'文件.bin').read_bytes() == data
            assert (output/'目录'/'hello.txt').read_bytes() == (other/'hello.txt').read_bytes()
            assert (output/'目录'/'empty.txt').stat().st_size == 0
            if preset == 4:
                images.append(image)
    for image in images:
        process = subprocess.Popen([str(cli),'mount',str(image)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
        try:
            drive = process.stdout.readline().strip().rstrip('\\') + '\\'
            path = Path(drive) / '文件.bin'
            wait_until(path.is_file)
            def read_at(offset):
                with path.open('rb') as stream:
                    stream.seek(offset)
                    return stream.read(4096)
            offsets = [rng.randrange(0,len(data)-4096) for _ in range(200)]
            with concurrent.futures.ThreadPoolExecutor(8) as executor:
                for offset, value in zip(offsets,executor.map(read_at,offsets)):
                    assert value == data[offset:offset+4096]
            try:
                path.open('wb')
            except OSError:
                pass
            else:
                raise AssertionError('mounted volume allowed writes')
            invoke('eject',drive[:2])
            wait_until(lambda: not path.exists())
        finally:
            if process.poll() is None:
                process.terminate()
            process.communicate(timeout=30)
    # Explicit temporary share exercises both first access and source deletion.
    process = subprocess.Popen([str(cli),'serve','test-native',str(root),'1'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    try:
        for image in images:
            path=Path(r'\\sfs$\test-native')/image.name/'文件.bin'
            wait_until(path.is_file)
            assert path.read_bytes()==data
            image.unlink()
            wait_until(lambda:not path.exists())
    finally:
        process.terminate();process.communicate(timeout=30)
print('PASS real codec creation, Unicode extraction, random access, WinFsp mounting, eject and UNC source removal')
