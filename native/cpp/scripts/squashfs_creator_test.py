#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read native-created images using both the application and unsquashfs."""
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

app = Path(sys.argv[1]).resolve()
cli = app / "archivefs"
def run(*args):
    p = subprocess.run(list(map(str, args)), capture_output=True, timeout=120)
    if p.returncode:
        raise RuntimeError((args, p.returncode, p.stdout, p.stderr))
    return p.stdout

with tempfile.TemporaryDirectory(prefix="archivefs-squashfs-native-") as temp:
    root = Path(temp)
    source = root / "源目录"
    source.mkdir()
    data = random.Random(2026).randbytes(180000) + b"\x00" * 65536 + b"abc" * 64000
    filename = "中文 文件😀.bin"
    (source / filename).write_bytes(data)
    (source / "small.txt").write_text("tail fragment", encoding="utf-8")
    (source / "empty.txt").write_bytes(b"")
    for preset in range(1, 9):
        image = root / f"镜像-{preset}.squashfs"
        run(cli, "create", "SquashFS", preset, image, source)
        assert run(cli, "read", image, filename, 65522, 192).decode().strip() == data[65522:65714].hex().upper()
        assert run("unsquashfs", "-cat", image, filename) == data
        assert run("unsquashfs", "-cat", image, "small.txt") == b"tail fragment"
        assert run("unsquashfs", "-cat", image, "empty.txt") == b""
        if preset == 1:
            flags = struct.unpack_from("<H", image.read_bytes(), 24)[0]
            assert flags & 3 == 3, flags
        print(f"PASS native creator preset {preset}", flush=True)
