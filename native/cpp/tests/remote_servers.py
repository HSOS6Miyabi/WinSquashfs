#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import getpass


def execute(*args):
    return subprocess.check_output(args, text=True).strip()


def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait(p):
    end = time.monotonic() + 20
    while time.monotonic() < end:
        try:
            with socket.create_connection(("127.0.0.1", p), timeout=1):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("Test server failed to start")


def test(protocol, **variables):
    env = {k: v for k, v in os.environ.items() if not k.startswith("REMOTE_")}
    env.update({f"REMOTE_{k.upper()}": str(v) for k, v in variables.items()})
    subprocess.run([sys.argv[1], protocol], env=env, check=True, timeout=120)


def ftp(root, tls=False):
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler
    from pyftpdlib.servers import FTPServer
    directory = root / ("ftps" if tls else "ftp")
    directory.mkdir()
    auth = DummyAuthorizer()
    auth.add_user("test", "local-test-password", str(directory), perm="elradfmwMT")
    handler = type("LocalHandler", (TLS_FTPHandler if tls else FTPHandler,), {})
    handler.authorizer = auth
    cert = ""
    if tls:
        cert = root / "cert.pem"
        key = root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-nodes", "-newkey", "rsa:2048", "-days", "1",
                        "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        handler.certfile = str(cert)
        handler.keyfile = str(key)
        handler.tls_control_required = True
        handler.tls_data_required = True
    server = FTPServer(("127.0.0.1", 0), handler)
    p = server.socket.getsockname()[1]
    worker = threading.Thread(target=server.serve_forever, kwargs={"timeout": 0.1, "handle_exit": False}, daemon=True)
    worker.start()
    try:
        test("ftps" if tls else "ftp", endpoint=f"ftp://127.0.0.1:{p}", root="/",
             user="test", secret="local-test-password", ca_file=cert)
    finally:
        server.close_all()
        worker.join(timeout=5)


def s3():
    import boto3
    from moto.server import ThreadedMotoServer
    p = port()
    server = ThreadedMotoServer(ip_address="127.0.0.1", port=p, verbose=False)
    server.start()
    url = f"http://127.0.0.1:{p}"
    try:
        client = boto3.client("s3", endpoint_url=url, region_name="us-east-1",
                              aws_access_key_id="test", aws_secret_access_key="local-test-password")
        client.create_bucket(Bucket="archivefs-test")
        test("s3", endpoint=url, root="/", bucket="archivefs-test", user="test", secret="local-test-password")
        assert not client.list_multipart_uploads(Bucket="archivefs-test").get("Uploads")
        assert not client.list_objects_v2(Bucket="archivefs-test").get("Contents")
    finally:
        server.stop()


def ssh(root):
    directory = root / "ssh-files"
    directory.mkdir()
    key = root / "client"
    host = root / "host"
    subprocess.run(["ssh-keygen", "-q", "-t", "rsa", "-b", "2048", "-m", "PEM", "-N", "", "-f", str(key)], check=True)
    subprocess.run(["ssh-keygen", "-q", "-t", "rsa", "-b", "2048", "-m", "PEM", "-N", "", "-f", str(host)], check=True)
    fingerprint = execute("ssh-keygen", "-lf", str(host) + ".pub", "-E", "sha256").split()[1]
    p = port()
    config = root / "sshd_config"
    config.write_text(f"Port {p}\nListenAddress 127.0.0.1\nHostKey {host}\nAuthorizedKeysFile {key}.pub\nStrictModes no\nPasswordAuthentication no\nPubkeyAuthentication yes\nUsePAM yes\nPermitRootLogin prohibit-password\nSubsystem sftp internal-sftp\nPidFile {root}/sshd.pid\n")
    subprocess.run(["sudo", "mkdir", "-p", "/run/sshd"], check=True)
    process = subprocess.Popen(["sudo", "/usr/sbin/sshd", "-D", "-e", "-f", str(config)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        wait(p)
        settings = dict(endpoint=f"127.0.0.1:{p}", root=str(directory), user=getpass.getuser(),
                        host_key=fingerprint, private_key=str(key))
        test("sftp", **settings, disable_helper="1")
        test("scp", **settings)
        test("scp", **settings, sudo="1")
    finally:
        if process.poll() is None:
            subprocess.run(["sudo", "kill", "-TERM", str(process.pid)], check=False)
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="archivefs-remote-") as tmp:
        root = Path(tmp)
        ftp(root)
        ftp(root, True)
        s3()
        ssh(root)
