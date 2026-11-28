#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from urllib.parse import urlparse, unquote
from xml.sax.saxutils import escape
import tempfile
import subprocess
import hashlib
import sys


class Server(ThreadingHTTPServer):
    def __init__(self, patch):
        super().__init__(("127.0.0.1", 0), Handler)
        self.files = {"/dav/a.txt": b"0123456789"}
        self.patch = patch
        self.puts = 0
        self.patches = 0
        self.copies = 0
        self.lose_response = False


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def path_value(self):
        return unquote(urlparse(self.path).path).rstrip("/")

    def etag(self, path):
        return '"' + hashlib.sha256(self.server.files[path]).hexdigest() + '"'

    def response(self, status, body=b"", **headers):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        for key, value in headers.items():
            self.send_header(key.replace("_", "-"), value)
        self.end_headers()
        if body:
            try:
                self.wfile.write(body)
            except (ConnectionResetError, BrokenPipeError):
                pass

    def condition(self, path):
        if path in self.server.files:
            return self.headers.get("If-Match") in (None, self.etag(path))
        return not self.headers.get("If-Match")

    def do_OPTIONS(self):
        self.response(204, DAV="1, 2, sabredav-partialupdate" if self.server.patch else "1, 2")

    def do_PROPFIND(self):
        self.body()
        path = self.path_value()
        fs = self.server.files
        if path != "/dav" and path not in fs:
            return self.response(404)
        paths = [path]
        if self.headers.get("Depth") == "1":
            paths += [p for p in fs if p.startswith(path + "/") and "/" not in p[len(path) + 1:]]
        rows = []
        for p in paths:
            if p == "/dav":
                props = "<d:resourcetype><d:collection/></d:resourcetype>"
            else:
                props = "<d:resourcetype/><d:getcontentlength>%d</d:getcontentlength><d:getetag>%s</d:getetag>" % (len(fs[p]), escape(self.etag(p)))
            rows.append("<d:response><d:href>%s</d:href><d:propstat><d:prop>%s</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>" % (escape(p), props))
        self.response(207, ('<d:multistatus xmlns:d="DAV:">' + "".join(rows) + "</d:multistatus>").encode(), Content_Type="application/xml")

    def do_GET(self):
        path = self.path_value()
        if path not in self.server.files:
            return self.response(404)
        if not self.condition(path):
            return self.response(412)
        data = self.server.files[path]
        if self.headers.get("Range"):
            a, b = map(int, self.headers["Range"][6:].split("-"))
            return self.response(206, data[a:b+1], Content_Range=f"bytes {a}-{b}/{len(data)}")
        self.response(200, data)

    def do_PATCH(self):
        path = self.path_value()
        data = self.body()
        if not self.server.patch:
            return self.response(405)
        if not self.condition(path):
            return self.response(412)
        assert self.headers["Content-Type"] == "application/x-sabredav-partialupdate"
        a, b = map(int, self.headers["X-Update-Range"][6:].split("-"))
        assert len(data) == b - a + 1
        value = bytearray(self.server.files[path])
        value.extend(b"\0" * max(0, b + 1 - len(value)))
        value[a:b+1] = data
        self.server.files[path] = bytes(value)
        self.server.patches += 1
        self.response(204)

    def do_PUT(self):
        path = self.path_value()
        data = self.body()
        if not self.condition(path) or (self.headers.get("If-None-Match") == "*" and path in self.server.files):
            return self.response(412)
        self.server.files[path] = data
        self.server.puts += 1
        if self.server.lose_response:
            self.server.lose_response = False
            self.close_connection = True
            return
        self.response(201)

    def do_COPY(self):
        path = self.path_value()
        dest = unquote(urlparse(self.headers["Destination"]).path)
        if dest in self.server.files and self.headers.get("Overwrite") == "F":
            return self.response(412)
        self.server.files[dest] = self.server.files[path]
        self.server.copies += 1
        self.response(201)

    def do_MOVE(self):
        path = self.path_value()
        dest = unquote(urlparse(self.headers["Destination"]).path)
        self.server.files[dest] = self.server.files.pop(path)
        self.response(201)

    def do_DELETE(self):
        self.server.files.pop(self.path_value())
        self.response(204)


def exercise(patch, mode, patches, puts):
    server = Server(patch)
    worker = Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}", directory, mode], check=True, timeout=30)
        assert server.files == {"/dav/a.txt": b"012AB5"}
        assert (server.patches, server.puts) == (patches, puts), (server.patches, server.puts)
        if mode == "winfsp":
            assert server.copies == 1
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


def recovery(mode, expected):
    server = Server(True)
    server.lose_response = mode == "lost-response"
    worker = Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}", directory, mode], check=True, timeout=30)
        assert server.files == {"/dav/a.txt": expected}
        assert server.puts == 1, server.puts
    finally:
        server.shutdown()
        server.server_close()
        worker.join()

recovery("recover", b"012AB56789")
recovery("conflict", b"XXXXXXXXXX")
recovery("lost-response", b"012AB56789")
exercise(True, "auto", 2, 2)
exercise(False, "auto", 0, 4)
exercise(True, "writeback", 0, 4)
print("PASS HTTP PATCH selected only for advertised capability; cache policy and whole-file fallback")
if len(sys.argv)>2 and sys.argv[2]=="--winfsp":
    exercise(True, "winfsp", 2, 2)
