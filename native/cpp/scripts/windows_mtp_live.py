#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only MTP/PTP transfer and random-access verification."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import random
import shutil
import subprocess
import sys
import tempfile
import time


def relative_parts(value):
    path = PureWindowsPath(value)
    if path.drive or path.root or not path.parts or any(
        p in (".", "..") or ":" in p or "\0" in p for p in path.parts
    ):
        raise ValueError("Expected a relative device path")
    return path.parts


def verify(path, size, expected):
    if path.stat().st_size != size:
        raise ValueError("Unexpected file size")
    with path.open("rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
    if digest != expected.lower():
        raise ValueError("Unexpected SHA-256")


def copy_projection(source, destination):
    start = time.perf_counter()
    with open(source, "rb") as src, open(destination, "xb") as dst:
        shutil.copyfileobj(src, dst, 1024 * 1024)
    return time.perf_counter() - start


def random_reads(source, reference, size):
    positions = [0, max(0, size - 65536)]
    rng = random.Random(2026)
    positions.extend(rng.randrange(size) for _ in range(32))

    def read(offset):
        length = min(65536, size - offset)
        with open(reference, "rb") as ref, open(source, "rb") as src:
            ref.seek(offset)
            expected = ref.read(length)
            start = time.perf_counter()
            src.seek(offset)
            actual = src.read(length)
            elapsed = time.perf_counter() - start
        if actual != expected or len(actual) != length:
            raise ValueError(f"Random read failed at {offset}")
        return elapsed

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        times = list(pool.map(read, positions))
    return {"requests": len(times), "mean_ms": sum(times) * 1000 / len(times),
            "max_ms": max(times) * 1000}


def explorer_copy(parts, destination, size, timeout):
    # Use the shell's portable-device namespace; do not invoke the archive driver.
    script = r"""
$ErrorActionPreference='Stop'
$parts=ConvertFrom-Json $env:ARCHIVEFS_MTP_SHELL_PATH
$shell=New-Object -ComObject Shell.Application
$folder=$shell.NameSpace(17)
if($null -eq $folder){throw 'This PC is unavailable'}
for($i=0;$i -lt $parts.Count;$i++){
    $matches=@($folder.Items() | Where-Object {$_.Name -ceq $parts[$i]})
    if($matches.Count -ne 1){throw "Missing or ambiguous device item: $($parts[$i])"}
    $item=$matches[0]
    if($i -lt $parts.Count-1){$folder=$item.GetFolder;if($null -eq $folder){throw 'Expected a device directory'}}
}
if($item.IsFolder){throw 'Expected a file'}
$target=$shell.NameSpace($env:ARCHIVEFS_MTP_DEST)
if($null -eq $target){throw 'Cannot open local destination'}
$clock=[Diagnostics.Stopwatch]::StartNew()
$target.CopyHere($item,4+16+1024)
$path=Join-Path $env:ARCHIVEFS_MTP_DEST $item.Name
while($clock.Elapsed.TotalSeconds -lt [double]$env:ARCHIVEFS_MTP_TIMEOUT){
    if(Test-Path -LiteralPath $path -PathType Leaf){
        try{
            $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
            try{if($stream.Length -eq [long]$env:ARCHIVEFS_MTP_SIZE){
                Write-Output ($clock.Elapsed.TotalSeconds.ToString([Globalization.CultureInfo]::InvariantCulture))
                exit 0
            }}finally{$stream.Dispose()}
        }catch [IO.IOException]{}
    }
    Start-Sleep -Milliseconds 100
}
throw 'Portable-device copy did not complete'
"""
    env = dict(os.environ, ARCHIVEFS_MTP_SHELL_PATH=json.dumps(parts),
               ARCHIVEFS_MTP_DEST=str(destination), ARCHIVEFS_MTP_SIZE=str(size),
               ARCHIVEFS_MTP_TIMEOUT=str(timeout))
    completed = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
        env=env, check=True, capture_output=True, text=True, timeout=timeout + 15
    )
    return destination / parts[-1], float(completed.stdout.strip().splitlines()[-1])


def worker(args):
    if args[0] == "copy":
        print(json.dumps(copy_projection(args[1], args[2])))
    elif args[0] == "random":
        print(json.dumps(random_reads(args[1], args[2], int(args[3]))))
    else:
        raise ValueError("Unknown worker")


def run_worker(timeout, *args):
    result = subprocess.run([sys.executable, __file__, "--worker", *map(str, args)],
                            check=True, capture_output=True, text=True, timeout=timeout)
    return json.loads(result.stdout)


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "source.bin"
        target = Path(tmp) / "target.bin"
        data = random.Random(2026).randbytes(300001)
        source.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        assert run_worker(20, "copy", source, target) >= 0
        verify(target, len(data), digest)
        assert run_worker(20, "random", source, target, len(data))["requests"] == 34
        try:
            verify(target, len(data), "0" * 64)
        except ValueError:
            pass
        else:
            raise AssertionError("Incorrect digest was accepted")
        for value in ("../file", r"C:\file", r"\\host\share"):
            try:
                relative_parts(value)
            except ValueError:
                pass
            else:
                raise AssertionError("Absolute or traversal path accepted")
    print("PASS transfer, SHA-256, random-read and path validation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", type=Path)
    parser.add_argument("--shell-path")
    parser.add_argument("--projected-path")
    parser.add_argument("--size", type=int)
    parser.add_argument("--sha256")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--worker", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.worker:
        worker(args.worker)
        return
    if args.self_test:
        self_test()
        return
    if os.name != "nt":
        parser.error("Physical MTP/PTP testing requires Windows")
    if not (args.app_directory and args.shell_path and args.projected_path and args.output
            and args.size and args.size > 0 and args.sha256 and len(args.sha256) == 64
            and all(c in "0123456789abcdefABCDEF" for c in args.sha256) and args.timeout > 0):
        parser.error("Specify application, both paths, fixture size, SHA-256 and output")
    parts = relative_parts(args.shell_path)
    projected = str(PureWindowsPath(r"\\sfs$\mtp").joinpath(*relative_parts(args.projected_path)))
    cli = args.app_directory.resolve() / "archivefs.exe"
    def invoke(*command):
        return subprocess.run([str(cli), "--lang", "en-US", *command], check=True,
                              capture_output=True, text=True, timeout=60).stdout.strip()
    if invoke("unc-status") != "Stopped":
        raise RuntimeError("Use a dedicated test account with the UNC agent stopped")
    settings = Path(os.environ["LOCALAPPDATA"]) / "WinArchiveFSPro" / "unc.json"
    original = settings.read_bytes() if settings.exists() else None
    with tempfile.TemporaryDirectory(prefix="archivefs-mtp-") as tmp:
        root = Path(tmp)
        settings.parent.mkdir(parents=True, exist_ok=True)
        try:
            settings.write_text(json.dumps({"Root": str(root), "Alias": "mtp-test",
                                            "IncludeDrives": False, "EnableMtp": True,
                                            "EnableAdb": False, "EnableAdbRoot": False}), encoding="utf-8")
            invoke("unc-start")
            print(invoke("devices", "mtp"), flush=True)
            results = []
            for projected_first in (False, True):
                folder = root / ("projection-first" if projected_first else "explorer-first")
                folder.mkdir()
                target = folder / "projected.bin"
                shell_folder = folder / "explorer"
                shell_folder.mkdir()
                if projected_first:
                    pt = run_worker(args.timeout, "copy", projected, target)
                    reference, et = explorer_copy(parts, shell_folder, args.size, args.timeout)
                else:
                    reference, et = explorer_copy(parts, shell_folder, args.size, args.timeout)
                    pt = run_worker(args.timeout, "copy", projected, target)
                verify(reference, args.size, args.sha256)
                verify(target, args.size, args.sha256)
                reads = run_worker(args.timeout, "random", projected, reference, args.size)
                results.append({"projected_first": projected_first, "bytes": args.size,
                                "explorer_seconds": et, "projection_seconds": pt,
                                "explorer_mib_s": args.size / (1024**2 * max(et, 1e-9)),
                                "projection_mib_s": args.size / (1024**2 * max(pt, 1e-9)),
                                "random_reads": reads})
                target.unlink()
                reference.unlink()
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
            print("PASS physical device data integrity and transfer measurements")
        finally:
            try:
                invoke("unc-stop")
            finally:
                if original is None:
                    settings.unlink(missing_ok=True)
                else:
                    settings.write_bytes(original)


if __name__ == "__main__":
    main()
