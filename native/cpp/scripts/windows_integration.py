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
import socket
import io
import tarfile
from mock_adb import Server as MockAdb, COMPONENT as MOCK_COMPONENT

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
    # Real Linux metadata fixtures: preserve exact symlink targets in TAR
    # rather than depending on the host's ability to represent POSIX devices.
    link_tar=io.BytesIO()
    link_data=b"symbolic link payload"
    with tarfile.open(fileobj=link_tar,mode="w",format=tarfile.PAX_FORMAT) as tar:
        entry=tarfile.TarInfo("folder");entry.type=tarfile.DIRTYPE;tar.addfile(entry)
        entry=tarfile.TarInfo("folder/target.txt");entry.size=len(link_data);tar.addfile(entry,io.BytesIO(link_data))
        for name,target in [("relative","folder/target.txt"),("absolute","/folder/target.txt"),("dir-link","folder"),("cycle","cycle"),("escape","../outside.txt"),("dangling","missing")]:
            entry=tarfile.TarInfo(name);entry.type=tarfile.SYMTYPE;entry.linkname=target;tar.addfile(entry)
        entry=tarfile.TarInfo("fifo");entry.type=tarfile.FIFOTYPE;tar.addfile(entry)
    fixtures=[]
    for suffix in ("squashfs","erofs"):
        fixture=root/("links."+suffix)
        if suffix=="squashfs":
            command=[str(app/"tools"/"mksquashfs.exe"),"-",fixture.as_posix(),"-tar","-noappend","-no-progress","-comp","zstd"]
        else:
            command=[str(app/"tools"/"mkfs.erofs.exe"),"--tar=f","-zlz4",str(fixture)]
        subprocess.run(command,input=link_tar.getvalue(),check=True,capture_output=True,timeout=90)
        assert invoke("readlink",fixture,"absolute")=="/folder/target.txt"
        assert invoke("readlink",fixture,"relative")=="folder/target.txt"
        assert invoke("read",fixture,"relative",0,100)==link_data.hex().upper()
        assert invoke("read",fixture,"dir-link/target.txt",0,100)==link_data.hex().upper()
        assert invoke("read",fixture,"absolute",0,100)==link_data.hex().upper()
        for name in ("cycle","escape","dangling","fifo"):
            invoke("read",fixture,name,0,100,ok=False)
        assert "Special file" in invoke("list",fixture)
        fixtures.append(fixture)
    # The official DwarFS creator accepts native directory input; create
    # relative Windows links for a DwarFS image, then read POSIX targets.
    link_source=root/"dwarfs-links";link_source.mkdir()
    (link_source/"folder").mkdir();(link_source/"folder"/"target.txt").write_bytes(link_data)
    (link_source/"relative").symlink_to(Path("folder")/"target.txt")
    (link_source/"dir-link").symlink_to("folder",target_is_directory=True)
    dwarfs_links=root/"links.dwarfs"
    invoke("create","DwarFS",4,dwarfs_links,link_source)
    assert invoke("readlink",dwarfs_links,"relative")=="folder/target.txt"
    assert invoke("read",dwarfs_links,"relative",0,100)==link_data.hex().upper()
    assert invoke("read",dwarfs_links,"dir-link/target.txt",0,100)==link_data.hex().upper()
    fixtures.append(dwarfs_links)
    print("PASS real SquashFS, EROFS and DwarFS symbolic link decoders")
    # Mutate real decoder inputs, rather than testing signatures alone.
    # Corruption may still describe a valid image; the CLI must either verify
    # it or reject it through a normal error path without crashing or hanging.
    malformed = root / 'malformed.img'
    mutations = 0
    for image in images:
        original = image.read_bytes()
        candidates = [original[:n] for n in (0, 3, 6, 96, 1028, len(original)//2)]
        for _ in range(16):
            changed = bytearray(original)
            for _ in range(1 + rng.randrange(4)):
                at = rng.randrange(len(changed))
                changed[at] ^= 1 << rng.randrange(8)
            candidates.append(changed)
        for candidate in candidates:
            malformed.write_bytes(candidate)
            result = subprocess.run([str(cli), 'verify', str(malformed)], capture_output=True, timeout=20)
            assert result.returncode in (0, 1, 2), (image.name, mutations, result.returncode, result.stderr[-2000:])
            mutations += 1
    malformed.unlink()
    print(f'PASS {mutations} truncated or mutated real archives')
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
            bare_file = Path(r'\\sfs.localhost') / share_name / copy.name / '文件.bin'
            wait_until(bare_file.is_file)
            assert bare_file.read_bytes()==data
            invoke('unc-stop')
            invoke('unc-start')
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
    # Recursive archive projection never opens the agent's own UNC filesystem
    # as a decoder source; the inner images are read through their outer reader.
    nested_source = root / 'nested-source'
    nested_source.mkdir()
    for image in images:
        shutil.copyfile(image,nested_source / image.name)
    outer = root / 'nested.squashfs'
    invoke('create','SquashFS',4,outer,*list(nested_source.iterdir()))
    # Exercise the real UNC filesystem with a local ADB smart-socket server.
    # Use an ephemeral port, leaving any real ADB server untouched.
    with socket.socket() as probe:
        probe.bind(("127.0.0.1",0))
        port=probe.getsockname()[1]
    mock=None
    invoke('unc-stop')
    try:
        settings_path.write_text(json.dumps({'Root':str(root),'Alias':'path-test','IncludeDrives':False,
            'AdbPort':port,'EnableMtp':True,'EnableAdb':True,'EnableAdbRoot':True,
            'ShowOpenProgress':False,'StartAtLogin':False,'Language':'en-US'}),encoding='utf-8')
        invoke('unc-start')
        adb=Path(r'\\sfs$\adb')
        adb_root=Path(r'\\sfs$\adb.r')
        for path in (adb,adb_root):
            try:
                list(path.iterdir())
            except OSError as error:
                assert error.winerror is not None, error
                print(f"PASS absent ADB Server rejected {path}: {error.winerror}")
            else:
                raise AssertionError("Missing ADB Server returned a directory")
        invoke('devices','mtp')  # Physical hardware may be absent on the runner.
        mtp=Path(r'\\sfs$\mtp')
        assert mtp.is_dir()
        list(mtp.iterdir())
        mock=MockAdb(port,{'/中文文件😀.bin':data,'/nested.squashfs':outer.read_bytes()})
        wait_until(lambda: adb.is_dir())
        assert list(adb.iterdir())==[]
        assert list(adb_root.iterdir())==[]
        mock.attached=True
        device=adb/MOCK_COMPONENT
        privileged=adb_root/MOCK_COMPONENT
        wait_until(device.is_dir)
        wait_until(privileged.is_dir)
        assert (device/'中文文件😀.bin').read_bytes()==data
        assert (privileged/'中文文件😀.bin').read_bytes()==data
        assert not (device/'root-only.txt').exists()
        assert (privileged/'root-only.txt').read_bytes()==b'root authorized'
        for image in images:
            nested=device/'nested.squashfs'/image.name/'文件.bin'
            assert nested.read_bytes()==data
        def device_read(offset):
            with (device/'中文文件😀.bin').open('rb') as stream:
                stream.seek(offset)
                return stream.read(4096)
        offsets=[rng.randrange(0,len(data)-4096) for _ in range(64)]
        with concurrent.futures.ThreadPoolExecutor(8) as workers:
            assert list(workers.map(device_read,offsets))==[data[n:n+4096] for n in offsets]
        assert any(command.startswith('shell,v2,raw:su -c ') for command in mock.commands)
        assert 'sync:' in mock.commands
        assert all('start-server' not in command and 'host:kill' not in command for command in mock.commands)
        print("PASS ADB UNC absence/empty/device states, Unicode, Root, concurrent reads and nested images")
    finally:
        invoke('unc-stop')
        if mock is not None:mock.close()
        if old_settings is not None:settings_path.write_bytes(old_settings)
        elif settings_path.exists():settings_path.unlink()
    # Explicit temporary share exercises both first access and source deletion.
    process = subprocess.Popen([str(cli),'serve','test-native',str(root),'1'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    try:
        for fixture in fixtures:
            alias=Path(r'\\sfs$\test-native')/fixture.name
            wait_until(lambda:(alias/'relative').is_file())
            assert (alias/'relative').read_bytes()==link_data
            assert (alias/'dir-link'/'target.txt').read_bytes()==link_data
            if fixture.suffix!=".dwarfs":
                assert (alias/'absolute').read_bytes()==link_data
                names={entry.name for entry in alias.iterdir()}
                assert {"cycle","escape","dangling","fifo"}<=names
                for name in ("cycle","escape","dangling","fifo"):
                    try:(alias/name).read_bytes()
                    except OSError:pass
                    else:raise AssertionError(f"Unsafe link or special object readable: {name}")
        for image in images:
            nested_path=Path(r'\\sfs$\test-native')/outer.name/image.name/'文件.bin'
            wait_until(nested_path.is_file)
            assert nested_path.read_bytes()==data, nested_path
        outer.unlink()
        wait_until(lambda:not nested_path.exists())
        for image in images:
            path=Path(r'\\sfs$\test-native')/image.name/'文件.bin'
            wait_until(path.is_file)
            assert path.read_bytes()==data
            image.unlink()
            wait_until(lambda:not path.exists())
    finally:
        process.terminate();process.communicate(timeout=30)
print('PASS real codec creation, Unicode extraction, malformed images, random access, WinFsp mounting, eject and UNC source removal')
