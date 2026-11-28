#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import concurrent.futures
import os
from pathlib import Path
import random
import re
import json
import shutil
import uuid
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
            with concurrent.futures.ThreadPoolExecutor(1) as reader:
                future = reader.submit(process.stdout.readline)
                try:
                    drive = future.result(timeout=30).strip().rstrip('\\') + '\\'
                except concurrent.futures.TimeoutError:
                    process.terminate()
                    process.wait(timeout=30)
                    raise
            assert re.fullmatch(r'[A-Za-z]:\\', drive), (drive, process.poll())
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
    # A real SMB share exercises the redirector, including hidden shares.
    share_name = 'archivefs-' + uuid.uuid4().hex[:10] + '$'
    settings_path = Path(os.environ['LOCALAPPDATA']) / 'WinArchiveFSPro' / 'unc.json'
    old_settings = settings_path.read_bytes() if settings_path.exists() else None
    aliases_path = settings_path.parent / 'remote-aliases.json'
    old_aliases = aliases_path.read_bytes() if aliases_path.exists() else None
    invoke('unc-stop')
    try:
        subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',
            f"New-SmbShare -Name '{share_name}' -Path '{str(root).replace(chr(39), chr(39)*2)}' -FullAccess ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -CachingMode None | Out-Null"], check=True, timeout=60)
        settings_path.parent.mkdir(parents=True,exist_ok=True)
        settings_path.write_text(json.dumps({'Root': str(root), 'Alias': 'path-test', 'IncludeDrives': False,
            'IdleSeconds': 1, 'MaxIdleImages': 2, 'StartAtLogin': False, 'Language': 'en-US'}),encoding='utf-8')
        invoke('unc-start')
        for image in images:
            copy = root / (image.name + '.raw')
            shutil.copyfile(image,copy)
            remote = Path(r'\\localhost') / share_name / copy.name
            assert invoke('read',remote,'文件.bin',0,17) == data[:17].hex().upper()
            for server in ('sfs$', 'sfs.localhost'):
                projected = Path(f'\\\\{server}\\UNC\\localhost') / share_name / copy.name / '文件.bin'
                wait_until(projected.is_file)
                assert projected.read_bytes()==data
            short_path = invoke('unc-alias',remote)
            assert short_path.lower().startswith(('\\\\sfs.localhost.localhost\\'+share_name+'\\').lower()), short_path
            alias_file = Path(short_path) / '文件.bin'
            wait_until(alias_file.is_file)
            assert alias_file.read_bytes()==data
            copy.unlink()
            wait_until(lambda: not projected.exists())
            wait_until(lambda: not alias_file.exists())
        junction = root / 'junction'
        subprocess.run(['cmd.exe','/d','/c','mklink','/J',str(junction),str(root/'sources')],check=True,timeout=30)
        assert (junction/'文件.bin').read_bytes()==data
        remote_image = root/'source-link.dwarfs'
        shutil.copyfile(images[1],remote_image)
        alias_link = root/'image-link.raw'
        subprocess.run(['cmd.exe','/d','/c','mklink',str(alias_link),str(remote_image)],check=True,timeout=30)
        assert invoke('read',alias_link,'文件.bin',0,17)==data[:17].hex().upper()
        alias_link.unlink()
        junction.rmdir()
    finally:
        invoke('unc-stop')
        subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',
            f"Remove-SmbShare -Name '{share_name}' -Force -Confirm:$false -ErrorAction SilentlyContinue"],timeout=60)
        if old_aliases is not None:
            aliases_path.write_bytes(old_aliases)
        else:
            aliases_path.unlink(missing_ok=True)
        if old_settings is not None:
            settings_path.write_bytes(old_settings)
        else:
            settings_path.unlink(missing_ok=True)
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
