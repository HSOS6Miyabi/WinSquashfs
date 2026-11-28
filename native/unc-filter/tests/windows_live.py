#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise a pre-installed, signed UNC minifilter on a dedicated Windows machine."""
import argparse
import concurrent.futures
import hashlib
import io
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid
import winreg


def execute(args, timeout=60):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError((result.args, result.returncode, result.stdout, result.stderr))
    return result.stdout


def ps(script, **variables):
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", "$ErrorActionPreference='Stop';" + script],
        env=dict(os.environ, **variables), capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError((result.args, result.returncode, result.stdout, result.stderr))
    return result.stdout


def worker(args):
    alias, direct, digest, length = args
    length = int(length)
    path = Path(alias)
    assert path.is_file(), "UNC network query open failed"
    assert path.stat().st_size == length
    assert path.name in {x.name for x in path.parent.iterdir()}
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    rng = random.Random(2026)

    def read(offset):
        with path.open("rb") as file, Path(direct).open("rb") as reference:
            file.seek(offset)
            reference.seek(offset)
            assert file.read(12347) == reference.read(12347)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(read, [rng.randrange(length) for _ in range(24)]))
    try:
        with path.open("r+b"):
            pass
    except OSError:
        pass
    else:
        raise AssertionError("Read-only UNC projection accepted write access")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--worker":
        worker(sys.argv[2:])
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", type=Path, required=True)
    parser.add_argument("--require-unqualified", action="store_true")
    parser.add_argument("--check-unload", action="store_true")
    args = parser.parse_args()
    cli = args.app_directory.resolve() / "archivefs.exe"
    def invoke(*args):
        return execute([cli, "--lang", "en-US", *args]).decode("utf-8").strip()
    if invoke("unc-status") != "Stopped":
        raise RuntimeError("Stop the archive agent before using the dedicated test computer")
    instances = execute(["fltmc.exe", "instances", "-f", "ArchiveFsUncFilter"])
    if b"ArchiveFsUncFilter".lower() not in instances.lower():
        raise RuntimeError("The signed filter must be loaded and attached")
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                        r"SYSTEM\CurrentControlSet\Services\ArchiveFsUncFilter\Parameters") as key:
        unqualified = winreg.QueryValueEx(key, "IncludeUnqualified")[0] == 1
    if args.require_unqualified and not unqualified:
        raise RuntimeError("The signed filter package must enable unqualified aliases")
    settings = Path(os.environ["LOCALAPPDATA"]) / "WinArchiveFSPro" / "unc.json"
    requests = settings.with_name("remote-aliases.json")
    before_settings = settings.read_bytes() if settings.exists() else None
    before_requests = requests.read_bytes() if requests.exists() else None
    share = "archivefs-filter-" + uuid.uuid4().hex[:10] + "$"
    loaded = True
    with tempfile.TemporaryDirectory(prefix="archivefs-filter-") as tmp:
        root = Path(tmp)
        data = random.Random(2026).randbytes(1024 * 1024 + 777)
        digest = hashlib.sha256(data).hexdigest()
        filename = "fixture.bin"
        (root / filename).write_bytes(data)
        archive = root / "source.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(root / filename, arcname=filename)
        host = os.environ["COMPUTERNAME"]
        remote = Path(f"\\\\{host}\\{share}")
        generic = Path(f"\\\\sfs$\\UNC\\{host}\\{share}\\{archive.name}")
        qualified = Path(f"\\\\sfs.{host}.localhost\\{share}\\{archive.name}")
        bare = Path(f"\\\\sfs.{host}\\{share}\\{archive.name}")
        try:
            ps("New-SmbShare -Name $env:TEST_SHARE -Path $env:TEST_ROOT -FullAccess ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -CachingMode None | Out-Null",
               TEST_SHARE=share, TEST_ROOT=str(root))
            settings.parent.mkdir(parents=True, exist_ok=True)
            settings.write_text(json.dumps({"Root": str(root), "Alias": "test", "IncludeDrives": False,
                                            "EnableMtp": False, "EnableAdb": False, "EnableAdbRoot": False,
                                            "StartAtLogin": False, "ShowTrayIcon": False,
                                            "IdleSeconds": 1, "Language": "en-US"}), encoding="utf-8")
            invoke("unc-start")
            # No unc-alias call or generic-path access is made before these opens.
            for alias in ([qualified, bare] if unqualified else [qualified]):
                execute([sys.executable, __file__, "--worker", alias / filename,
                         remote / filename, digest, len(data)], timeout=90)
                print("PASS first UNC alias access, query open, enumeration and concurrent random reads", alias, flush=True)
            assert hashlib.sha256((generic / filename).read_bytes()).hexdigest() == digest
            assert hashlib.sha256((remote / filename).read_bytes()).hexdigest() == digest
            path = invoke("unc-alias", str(remote / archive.name))
            expected = str(bare if unqualified else qualified)
            assert path.casefold() == expected.casefold(), (path, expected)
            assert (requests.read_bytes() if requests.exists() else None) == before_requests, "Per-share alias registration changed"
            print("PASS CLI dynamic aliases without persistent registration and ordinary SMB access", flush=True)
            if args.check_unload:
                execute(["fltmc.exe", "unload", "ArchiveFsUncFilter"])
                loaded = False
                assert hashlib.sha256((generic / filename).read_bytes()).hexdigest() == digest
                assert hashlib.sha256((remote / filename).read_bytes()).hexdigest() == digest
                execute(["fltmc.exe", "load", "ArchiveFsUncFilter"])
                loaded = True
                execute([sys.executable, __file__, "--worker", qualified / filename,
                         remote / filename, digest, len(data)], timeout=90)
                print("PASS unload and reload without disrupting generic UNC or SMB", flush=True)
        finally:
            try:
                invoke("unc-stop")
            finally:
                try:
                    if not loaded:
                        execute(["fltmc.exe", "load", "ArchiveFsUncFilter"])
                    ps("Remove-SmbShare -Name $env:TEST_SHARE -Force -Confirm:$false -ErrorAction SilentlyContinue",
                       TEST_SHARE=share)
                finally:
                    for path, previous in ((settings, before_settings), (requests, before_requests)):
                        if previous is None:
                            path.unlink(missing_ok=True)
                        else:
                            path.write_bytes(previous)


if __name__ == "__main__":
    main()
