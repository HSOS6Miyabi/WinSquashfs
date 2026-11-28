#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate actual native decoder binaries, rather than the test ABI mocks."""
import io
from pathlib import Path
import random
import struct
import subprocess
import sys
import tarfile
import tempfile

root = Path(sys.argv[1]).resolve()
arch = sys.argv[2]
machine = {"x86": 0x014C, "arm64": 0xAA64}[arch]
for binary in root.rglob("*"):
    if binary.suffix.lower() not in {".dll", ".exe"}:
        continue
    with binary.open("rb") as stream:
        assert stream.read(2) == b"MZ", binary
        stream.seek(60)
        (offset,) = struct.unpack("<I", stream.read(4))
        stream.seek(offset)
        assert stream.read(4) == b"PE\0\0", binary
        assert struct.unpack("<H", stream.read(2))[0] == machine, binary
cli = root / "archivefs.exe"

def run(*args, **kwargs):
    result = subprocess.run([str(a) for a in args], capture_output=True,
                            check=False, timeout=180, **kwargs)
    if result.returncode:
        raise RuntimeError((args, result.returncode, result.stdout, result.stderr))
    return result.stdout

with tempfile.TemporaryDirectory(prefix="archivefs-native-readers-") as temp:
    work = Path(temp)
    source = work / "文件.bin"
    content = random.Random(2026).randbytes(1048576) + b"native\0reader"
    source.write_bytes(content)
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w", format=tarfile.PAX_FORMAT) as tar:
        info = tarfile.TarInfo(source.name)
        info.size = len(content)
        tar.addfile(info, io.BytesIO(content))
    for format, suffix in (("SquashFS", "squashfs"), ("DwarFS", "dwarfs"), ("EROFS", "erofs")):
        image = work / ("native." + suffix)
        if format == "SquashFS":
            run(root / "tools/tar2sqfs.exe", "-c", "zstd", image, input=stream.getvalue())
        else:
            run(cli, "create", format, "4", image, source)
        assert run(cli, "info", image).decode("utf-8").startswith(format + " ")
        for offset, size in ((0, 1), (17, 4096), (65521, 100), (len(content) - 8, 32)):
            assert run(cli, "read", image, source.name, offset, size).decode().strip() == content[offset:offset + size].hex().upper()
        destination = work / suffix
        run(cli, "unpack", image, destination)
        assert (destination / source.name).read_bytes() == content
        print(f"PASS {arch} {format}: native decoder, Unicode names and exact random reads")
