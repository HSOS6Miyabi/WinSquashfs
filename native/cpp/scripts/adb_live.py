#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare an isolated fixture on an explicitly selected Android test device."""
import argparse
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import uuid

parser = argparse.ArgumentParser()
parser.add_argument("executable", type=Path)
parser.add_argument("--serial", required=True)
parser.add_argument("--root", action="store_true", help="Also test an already authorized Magisk/KernelSU su")
parser.add_argument("--port", type=int, default=5037)
args = parser.parse_args()
if not 1 <= args.port <= 65535:
    parser.error("Invalid ADB server port")
# The test client uses the already running ADB server, just like the product.
with socket.create_connection(("127.0.0.1", args.port), timeout=3):
    pass
adb = ["adb", "-P", str(args.port), "-s", args.serial]
def run(*command):
    return subprocess.run([*adb, *command], check=True, capture_output=True, timeout=60).stdout
def shell(command):
    return run("shell", command)
def root_shell(command):
    return shell("su -c " + shlex.quote(command))
directory = "/data/local/tmp/archivefs-live-" + uuid.uuid4().hex
quoted = shlex.quote(directory)
assert run("get-state").strip() == b"device"
shell("mkdir " + quoted)
private = directory + "/private"
private_created = False
try:
    with tempfile.TemporaryDirectory(prefix="archivefs-adb-") as temp:
        path = Path(temp) / "payload.bin"
        path.write_bytes(bytes((i*29+(i>>8))&255 for i in range(262144+37)))
        filename = "中文 '文件\n😀.bin"
        run("push", os.fspath(path), directory + "/" + filename)
    shell("mkdir " + shlex.quote(directory + "/空目录"))
    # Create a sparse file and write a small marker beyond the 4 GiB boundary.
    # No multi-gigabyte transfer or allocation is needed.
    shell("printf ADB-large | dd of=" + shlex.quote(directory + "/sparse.bin") +
          " bs=1 seek=4294967299 count=9 status=none")
    subprocess.run([os.fspath(args.executable.resolve()), args.serial, directory,
                    "plain", str(args.port)], check=True, timeout=120)
    if args.root:
        assert root_shell("id -u").strip() == b"0", "su must be authorized and run as root"
        private_created = True
        root_shell("mkdir " + shlex.quote(private) + " && chmod 700 " + shlex.quote(private)
                   + " && printf ArchiveFS-root-only > " + shlex.quote(private + "/marker.bin")
                   + " && chmod 600 " + shlex.quote(private + "/marker.bin"))
        try:
            shell("cat " + shlex.quote(private + "/marker.bin"))
        except subprocess.CalledProcessError:
            pass
        else:
            raise AssertionError("Root fixture is readable without su")
        subprocess.run([os.fspath(args.executable.resolve()), args.serial, directory,
                        "root", str(args.port)], check=True, timeout=120)
finally:
    try:
        if private_created:
            root_shell("rm -rf -- " + shlex.quote(private))
    finally:
        shell("rm -rf -- " + quoted)
