# SPDX-License-Identifier: GPL-3.0-only
import socket
import struct
import subprocess
import sys
import threading
import shlex
import time

EXE = sys.argv[1]
NAME = "中文 '文件\n😀.bin"
META = b"81a4 4294967552 1700000000 1 99"
DEVICES = b"serial-1\tdevice product:p model:Phone transport_id:1\nserial-2\toffline transport_id:2\n"

def exact(conn, n):
    b = bytearray()
    while len(b) < n:
        part = conn.recv(n - len(b))
        if not part:
            raise EOFError()
        b.extend(part)
    return bytes(b)

def service(conn):
    return exact(conn, int(exact(conn, 4), 16)).decode("utf-8")

def packet(conn, channel, data):
    conn.sendall(struct.pack("<BI", channel, len(data)) + data)

def test(mode):
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    if mode == "absent":
        sock.close()
        subprocess.run([EXE, str(port), mode], check=True, timeout=8)
        return
    sock.listen()
    sock.settimeout(5)
    errors = []
    commands = []
    def server():
        try:
            count = 1 if mode in ("empty", "timeout") else 2 if mode == "denied" else 4
            for n in range(count):
                conn, _ = sock.accept()
                with conn:
                    conn.settimeout(4)
                    if n == 0:
                        assert service(conn) == "host:devices-l"
                        if mode == "timeout":
                            time.sleep(1.7)
                            return
                        payload = b"" if mode == "empty" else DEVICES
                        conn.sendall(b"OKAY" + f"{len(payload):04x}".encode() + payload)
                        continue
                    assert service(conn) == "host:transport:serial-1"
                    conn.sendall(b"OKAY")
                    command = service(conn)
                    assert command.startswith("shell,v2,raw:")
                    command = command[len("shell,v2,raw:"):]
                    if mode in ("root", "denied"):
                        args = shlex.split(command)
                        assert args[:2] == ["su", "-c"] and len(args) == 3
                        assert '$(id -u)' in args[2]
                        command = args[2]
                    else:
                        assert not command.startswith("su ")
                    commands.append(command)
                    conn.sendall(b"OKAY")
                    assert exact(conn, 5) == b"\x04\x00\x00\x00\x00"
                    if mode == "denied":
                        packet(conn, 2, b"Permission denied")
                        packet(conn, 3, b"\x4d")
                        continue
                    if n == 1:
                        assert "printf" in command and "/storage" in command
                        data = NAME.encode() + b"\0" + META + b"\0dir\0" + b"41ed 0 1700000000 1 100\0"
                    elif n == 2:
                        assert "stat -L" in command
                        assert shlex.split(command)[-1] == "/storage/" + NAME
                        data = META + b"\n"
                    else:
                        assert "skip=4294967299" in command and "count=9" in command
                        assert "if=/storage/" + NAME in shlex.split(command)
                        data = bytes(i * 29 for i in range(9))
                    # Deliberately split a UTF-8 filename and a binary read between packets.
                    packet(conn, 1, data[:1])
                    packet(conn, 1, data[1:])
                    packet(conn, 3, b"\0")
        except BaseException as exc:
            errors.append(exc)
    thread = threading.Thread(target=server)
    thread.start()
    try:
        subprocess.run([EXE, str(port), mode], check=True, timeout=10)
    finally:
        thread.join(6)
        sock.close()
    if errors:
        raise errors[0]
    assert not thread.is_alive()

for mode in ("absent", "empty", "normal", "root", "denied", "timeout"):
    test(mode)
print("PASS ADB server absence, empty list, UTF-8, binary range, Root authorization and deadlines")
