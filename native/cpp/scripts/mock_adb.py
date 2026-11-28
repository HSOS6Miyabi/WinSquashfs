# SPDX-License-Identifier: GPL-3.0-only
"""In-process ADB protocol fixture for WinFsp integration tests. No adb.exe."""
import socketserver
import struct
import shlex
import threading
import time

SERIAL = "archivefs-test"
MODEL = "Fixture"
COMPONENT = f"{MODEL} ({SERIAL})"
STAMP = 1700000000

def exact(conn, count):
    result = bytearray()
    while len(result) < count:
        part = conn.recv(count - len(result))
        if not part:
            raise EOFError()
        result += part
    return bytes(result)

def service(conn):
    return exact(conn, int(exact(conn, 4), 16)).decode("utf-8")

def message(conn, data):
    conn.sendall(b"OKAY" + f"{len(data):04x}".encode() + data)

def packet(conn, channel, data):
    conn.sendall(struct.pack("<BI", channel, len(data)) + data)

class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, port, files):
        self.files = dict(files)
        self.files["/root-only.txt"] = b"root authorized"
        self.attached = False
        self.commands = []
        self.errors = []
        super().__init__(("127.0.0.1", port), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(5)
        if self.errors:
            raise AssertionError(self.errors)

    def metadata(self, name):
        if name == "/":
            return 0x41ed, 0, 1
        if name not in self.files:
            raise FileNotFoundError(name)
        return 0x81a4, len(self.files[name]), 2+list(self.files).index(name)

    def stat_record(self, name):
        mode, size, inode = self.metadata(name)
        return struct.pack("<IQQIIIIQqqq",0,1,inode,mode,1,0,0,size,STAMP,STAMP,STAMP)

    def children(self, root):
        return [name for name in self.files if root or name != "/root-only.txt"]

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(20)
        try:
            self.dispatch()
        except (EOFError, ConnectionError, OSError):
            # The client may close a sync session as soon as its response arrives.
            return
        except Exception as error:
            self.server.errors.append(repr(error))

    def dispatch(self):
        conn = self.request
        server = self.server
        request = service(conn)
        server.commands.append(request)
        if request == "host:devices-l":
            data = f"{SERIAL}\tdevice product:fixture model:{MODEL} transport_id:1\n".encode() if server.attached else b""
            message(conn, data)
            return
        if request == f"host-serial:{SERIAL}:features":
            message(conn, b"stat_v2,ls_v2")
            return
        assert request == f"host:transport:{SERIAL}", request
        conn.sendall(b"OKAY")
        request = service(conn)
        server.commands.append(request)
        conn.sendall(b"OKAY")
        if request == "sync:":
            while True:
                tag = exact(conn,4)
                name = exact(conn,struct.unpack("<I",exact(conn,4))[0]).decode("utf-8")
                if tag == b"LIS2":
                    assert name == "/", name
                    for child in server.children(False):
                        raw = child[1:].encode("utf-8")
                        conn.sendall(b"DNT2"+server.stat_record(child)+struct.pack("<I",len(raw))+raw)
                    conn.sendall(b"DONE"+bytes(72))
                elif tag == b"STA2":
                    if name == "/root-only.txt" or name not in server.files and name != "/":
                        conn.sendall(b"STA2"+struct.pack("<I",13 if name == "/root-only.txt" else 2)+bytes(64))
                    else:
                        conn.sendall(b"STA2"+server.stat_record(name))
                else:
                    raise AssertionError(tag)
        assert request.startswith("shell,v2,raw:"), request
        assert exact(conn,5) == b"\x04\x00\x00\x00\x00"
        command = request[len("shell,v2,raw:"):]
        root = command.startswith("su ")
        if root:
            tokens = shlex.split(command)
            assert tokens[:2] == ["su","-c"]
            command = tokens[2].split("exit 77; ",1)[1]
        if "printf" in command:
            data = bytearray()
            for name in server.children(root):
                mode,size,inode = server.metadata(name)
                data += name[1:].encode("utf-8")+b"\0"+f"{mode:x} {size} {STAMP} 1 {inode} {STAMP}".encode()+b"\0"
        elif command.startswith("stat "):
            name = shlex.split(command)[-1]
            if not root and name == "/root-only.txt":
                packet(conn,2,b"Permission denied");packet(conn,3,b"\x4d");return
            mode,size,inode = server.metadata(name)
            data = f"{mode:x} {size} {STAMP} 1 {inode} {STAMP}\n".encode()
        elif command.startswith("dd "):
            args = dict(token.split("=",1) for token in shlex.split(command)[1:])
            name = args["if"]
            if not root and name == "/root-only.txt":
                packet(conn,2,b"Permission denied");packet(conn,3,b"\x4d");return
            offset = int(args["skip"])
            data = server.files[name][offset:offset+int(args["count"])]
        else:
            raise AssertionError(command)
        for i in range(0,len(data),65536):
            packet(conn,1,data[i:i+65536])
        packet(conn,3,b"\0")
