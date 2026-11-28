#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate actual native decoder binaries, rather than the test ABI mocks."""
from pathlib import Path
import random
import struct
import subprocess
import sys
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
        actual, = struct.unpack("<H", stream.read(2))
        # ARM64X DLLs can also be loaded by native ARM64 processes.
        compatible = actual == machine or (arch == "arm64" and binary.suffix.lower() == ".dll"
                                           and actual == 0xA64E)
        assert compatible, (binary, f"machine=0x{actual:04x}", f"expected=0x{machine:04x}")
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
    for format, suffix in (("SquashFS", "squashfs"), ("DwarFS", "dwarfs"), ("EROFS", "erofs")):
        image = work / ("native." + suffix)
        run(cli, "create", format, "4", image, source)
        assert run(cli, "info", image).decode("utf-8").startswith(format + " ")
        for offset, size in ((0, 1), (17, 4096), (65521, 100), (len(content) - 8, 32)):
            assert run(cli, "read", image, source.name, offset, size).decode().strip() == content[offset:offset + size].hex().upper()
        destination = work / suffix
        run(cli, "unpack", image, destination)
        assert (destination / source.name).read_bytes() == content
        print(f"PASS {arch} {format}: native decoder and creator, Unicode names and exact random reads")
    for preset in range(1, 9):
        image = work / f"中文镜像-{preset}.squashfs"
        run(cli, "create", "SquashFS", str(preset), image, source)
        assert run(cli, "read", image, source.name, 17, 4096).decode().strip() == content[17:4113].hex().upper()
        if preset == 1:
            flags = struct.unpack_from("<H", image.read_bytes(), 24)[0]
            assert flags & 0x0003 == 0x0003, flags
        print(f"PASS {arch} SquashFS preset {preset}")
