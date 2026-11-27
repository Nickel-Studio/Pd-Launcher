import argparse
from collections import Counter
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
from pathlib import Path
import select
import shutil
import socket
import socketserver
import ssl
import subprocess
import threading
import time
from urllib.parse import urlsplit

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


class State:
    def __init__(self, payload):
        self.lock = threading.Lock()
        self.counts = Counter()
        self.payload = payload
        self.http = ""
        self.http2 = ""
        self.connects = []
        self.closed = Counter()
        self.snapshot_downloads = []

    def count(self, key):
        with self.lock:
            self.counts[key] += 1
            return self.counts[key]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send_body(self, body, status=200, headers=()):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for key, value in headers:
            self.send_header(key, value)
        self.end_headers()
        self.close_connection = True
        if self.command != "HEAD":
            self.wfile.write(body)
            self.wfile.flush()

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "16777216")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/post-retry":
            self.server.state.count("post-retry")
            self.send_body(b"no", 503)
        else:
            self.send_body(body)

    do_PUT = do_POST
    do_PATCH = do_POST
    do_DELETE = do_POST

    def do_GET(self):
        state = self.server.state
        path = urlsplit(self.path).path
        count = state.count(path)
        try:
            if path.startswith("/snapshot"):
                self.snapshot_response(path, count)
            elif path == "/json":
                self.send_body(b'{"ok":true}', headers=(("Content-Type", "application/json"),))
            elif path == "/stats":
                with state.lock:
                    content = json.dumps({**state.counts, "etag304": state.counts["etag304"], "closed": dict(state.closed), "connects": state.connects}).encode()
                self.send_body(content)
            elif path == "/fixture.crl":
                self.send_body(state.crl, headers=(("Content-Type", "application/pkix-crl"),))
            elif path.startswith("/redirect"):
                location = {"/redirect": "/json", "/redirect-loop": "/redirect-loop",
                            "/redirect-private": "http://192.168.1.1/secret",
                            "/redirect-cross": state.http2 + "/json", "/redirect-http": state.http + "/json"}[path]
                self.send_body(b"", 302, (("Location", location),))
            elif path == "/chunked":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n7\r\nchunked\r\n5\r\n body\r\n0\r\n\r\n")
                self.close_connection = True
            elif path == "/duplicate-length":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc")
                self.close_connection = True
            elif path == "/ambiguous-framing":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\n\r\n")
                self.close_connection = True
            elif path == "/encoded":
                self.send_body(b"bad", headers=(("Content-Encoding", "gzip"),))
            elif path == "/truncated":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 200\r\nConnection: close\r\n\r\npartial")
                self.close_connection = True
            elif path == "/big-header":
                self.send_body(b"", headers=(("X-Large", "x" * 70000),))
            elif path == "/folded-header":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nX-Header: value\r\n folded\r\nContent-Length: 0\r\n\r\n")
                self.close_connection = True
            elif path == "/folded-length":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n 0\r\nConnection: close\r\n\r\n")
                self.close_connection = True
            elif path == "/big":
                self.send_body(b"x" * 1000)
            elif path == "/invalid-json":
                self.send_body(b"{invalid")
            elif path == "/retry":
                self.send_body(b"retry" if count < 3 else b"ok", 503 if count < 3 else 200, (("Retry-After", "0"),))
            elif path == "/failure":
                self.send_body(b"unavailable", 503)
            elif path == "/silent":
                self.connection.settimeout(2)
                try:
                    while self.connection.recv(4096):
                        pass
                    with state.lock:
                        state.closed[path] += 1
                except OSError:
                    pass
                self.close_connection = True
            elif path == "/cache":
                self.send_body(b"cached", headers=(("Cache-Control", "public, max-age=60"), ("ETag", '"cache-v1"')))
            elif path == "/no-store":
                self.send_body(b"no-store", headers=(("Cache-Control", "no-store"),))
            elif path == "/etag":
                if self.headers.get("If-None-Match") == '"etag-v1"':
                    state.count("etag304")
                    self.send_body(b"", 304, (("ETag", '"etag-v1"'), ("Cache-Control", "max-age=0")))
                else:
                    self.send_body(b"etag-body", headers=(("ETag", '"etag-v1"'), ("Cache-Control", "max-age=0")))
            elif path == "/slow-cache":
                time.sleep(0.20)
                self.send_body(b"slow-cache", headers=(("Cache-Control", "max-age=60"),))
            elif path == "/abort-cache":
                if count == 1:
                    self.connection.settimeout(2)
                    try:
                        while self.connection.recv(4096):
                            pass
                        with state.lock:
                            state.closed[path] += 1
                    except OSError:
                        pass
                    self.close_connection = True
                else:
                    self.send_body(b"retry-cache", headers=(("Cache-Control", "max-age=60"),))
            elif path == "/invalid-archive":
                self.send_body(b"not-a-zip")
            elif path in ("/payload", "/slow-payload", "/ignore-range", "/bad-range", "/stale-resume"):
                self.payload_response(path, count)
            else:
                self.send_body(b"not found", 404)
        # Windows reports a peer abort as ConnectionAbortedError (WinError 10053),
        # which is a sibling of ConnectionResetError, so match the ConnectionError base.
        except (ConnectionError, ssl.SSLError):
            with state.lock:
                state.closed[path] += 1
            self.close_connection = True

    def snapshot_response(self, path, count):
        agent = self.headers.get("User-Agent", "")
        if path.startswith(("/snapshot-artifact/", "/snapshot-artifact-delay/")):
            expected = "Pd/snapshot-download-" + path.rsplit("-", 1)[-1]
            with self.server.state.lock:
                self.server.state.snapshot_downloads.append((path, agent, self.headers.get("Range", "")))
            if path.startswith("/snapshot-artifact-delay/") or "/failure-" in path:
                time.sleep(0.25)
            if "/failure-" in path:
                self.send_body(b"unavailable", 503)
            else:
                payload = self.server.state.payload if agent == expected else b"w" * len(self.server.state.payload)
                self.payload_response(path, count, payload)
            return
        if path.startswith("/snapshot-redirect/"):
            time.sleep(0.25)
            self.send_body(b"", 302, (("Location", path.replace("/snapshot-redirect/", "/snapshot/", 1)),))
            return
        if path == "/snapshot-partial":
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 131072\r\nConnection: close\r\n\r\n" + b"m" * 65537)
            self.close_connection = True
            return
        if path.startswith("/snapshot-delay/"):
            time.sleep(0.20)
        elif path.startswith("/snapshot-flight/"):
            time.sleep(0.35 if agent.endswith("snapshot-A") else 0.02)
        self.send_body(json.dumps({"agent": agent, "count": count, "path": path}).encode(),
                       headers=(("Cache-Control", "public, max-age=60"),))

    def payload_response(self, path, count, payload=None):
        state = self.server.state
        payload = state.payload if payload is None else payload
        start, end = 0, len(payload) - 1
        etag = '"artifact-v2"' if path == "/stale-resume" and count > 1 else '"artifact-v1"'
        ranged = self.headers.get("Range")
        if path == "/ignore-range" or (self.headers.get("If-Range") and self.headers.get("If-Range") != etag):
            ranged = None
        if ranged:
            bounds = ranged.removeprefix("bytes=").split("-", 1)
            start = int(bounds[0])
            if bounds[1]:
                end = int(bounds[1])
            if start >= len(payload) or end >= len(payload):
                self.send_body(b"", 416)
                return
        status = 206 if ranged else 200
        body = memoryview(payload)[start:end + 1]
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("ETag", etag)
        self.send_header("Connection", "close")
        if ranged:
            self.send_header("Content-Range", f"bytes {start + (1 if path == '/bad-range' else 0)}-{end}/{len(payload)}")
        self.end_headers()
        self.close_connection = True
        for at in range(0, len(body), 16384):
            self.wfile.write(body[at:at + 16384])
            self.wfile.flush()
            if path in ("/slow-payload", "/stale-resume") or path.startswith("/snapshot-artifact/slow-"):
                time.sleep(0.001)


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class ProxyHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(3)
        first = self.rfile.readline(8192).decode("ascii").strip()
        while self.rfile.readline(8192) not in (b"\r\n", b"\n", b""):
            pass
        command, authority, _ = first.split(" ", 2)
        with self.server.state.lock:
            self.server.state.connects.append(authority)
        if command != "CONNECT" or self.server.reject:
            self.wfile.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            return
        host, port = authority.rsplit(":", 1)
        host = host.strip("[]")
        if host not in ("127.0.0.1", "::1") or int(port) not in self.server.allowed_ports:
            self.wfile.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            return
        with socket.create_connection((host, int(port)), timeout=3) as peer:
            self.wfile.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            self.wfile.flush()
            sockets = [self.connection, peer]
            while True:
                ready, _, _ = select.select(sockets, [], [], 3)
                if not ready:
                    return
                for source in ready:
                    data = source.recv(65536)
                    if not data:
                        return
                    (peer if source is self.connection else self.connection).sendall(data)


def certificates(root, crl_url):
    now = datetime.now(timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Nickel loopback fixture CA")])
    point = x509.DistributionPoint(full_name=[x509.UniformResourceIdentifier(crl_url)], relative_name=None,
                                   reasons=None, crl_issuer=None)
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name).public_key(ca_key.public_key())
          .serial_number(x509.random_serial_number()).not_valid_before(now - timedelta(days=1))
          .not_valid_after(now + timedelta(days=2)).add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
          .add_extension(x509.SubjectKeyIdentifier.from_public_key(ca_key.public_key()), critical=False)
          .add_extension(x509.CRLDistributionPoints([point]), critical=False)
          .add_extension(x509.KeyUsage(True, False, False, False, False, True, True, False, False), critical=True)
          .sign(ca_key, hashes.SHA256()))
    (root / "ca.pem").write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    for kind in ("valid", "hostname", "expired", "untrusted"):
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost" if kind != "hostname" else "wrong.invalid")])
        name = x509.DNSName("wrong.invalid") if kind == "hostname" else x509.DNSName("localhost")
        alt = [name] if kind == "hostname" else [name, x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]
        builder = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject if kind == "untrusted" else ca_name)
                   .public_key(key.public_key()).serial_number(x509.random_serial_number())
                   .not_valid_before(now - timedelta(days=3 if kind == "expired" else 1))
                   .not_valid_after(now - timedelta(days=1) if kind == "expired" else now + timedelta(days=1))
                   .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
                   .add_extension(x509.KeyUsage(True, False, True, False, False, False, False, False, False), critical=True)
                   .add_extension(x509.SubjectAlternativeName(alt), critical=False)
                   .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False)
                   .add_extension(x509.CRLDistributionPoints([point]), critical=False)
                   .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False))
        cert = builder.sign(key if kind == "untrusted" else ca_key, hashes.SHA256())
        (root / (kind + ".pem")).write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        (root / (kind + ".key")).write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    crl = (x509.CertificateRevocationListBuilder().issuer_name(ca_name)
           .last_update(now - timedelta(hours=1)).next_update(now + timedelta(days=1))
           .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False)
           .add_extension(x509.CRLNumber(1), critical=False).sign(ca_key, hashes.SHA256()))
    return crl.public_bytes(serialization.Encoding.DER)


def start(server):
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--snapshots", action="store_true")
    args = parser.parse_args()
    root = args.workspace.resolve()
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    payload = bytes(range(256)) * (5 * 1024 * 1024 // 256)
    (root / "payload.bin").write_bytes(payload)
    state = State(payload)
    servers = []
    try:
        config = {"ca_bundle": str(root / "ca.pem"), "snapshot_only": args.snapshots,
                  "alternate_ca_bundle": str(root / "alternate-ca.pem")}
        for name in ("http", "http2"):
            server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.daemon_threads = True
            server.state = state
            config[name] = f"http://127.0.0.1:{server.server_port}"
            servers.append(start(server))
        state.http, state.http2 = config["http"], config["http2"]
        state.crl = certificates(root, state.http + "/fixture.crl")
        shutil.copyfile(root / "ca.pem", root / "alternate-ca.pem")
        for name in ("https", "hostname_https", "expired_https", "untrusted_https"):
            server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.daemon_threads = True
            server.state = state
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            kind = "valid" if name == "https" else name.removesuffix("_https")
            context.load_cert_chain(root / (kind + ".pem"), root / (kind + ".key"))
            server.socket = context.wrap_socket(server.socket, server_side=True)
            config[name] = f"https://127.0.0.1:{server.server_port}"
            servers.append(start(server))
        for name, rejected in (("proxy", False), ("reject_proxy", True)):
            server = Proxy(("127.0.0.1", 0), ProxyHandler)
            server.state = state
            server.reject = rejected
            server.allowed_ports = {item.server_port for item in servers if isinstance(item, ThreadingHTTPServer)}
            config[name] = f"http://127.0.0.1:{server.server_address[1]}"
            servers.append(start(server))
        file = root / "fixture.json"
        file.write_text(json.dumps(config), encoding="utf-8")
        subprocess.run([str(args.executable.resolve()), str(file)], check=True, timeout=120)
        with state.lock:
            if not args.snapshots:
                assert state.counts["post-retry"] == 1, "POST retry replayed"
                assert state.counts["/retry"] == 3, "GET retry count wrong"
                assert state.closed["/silent"] >= 1, "cancel did not close real socket"
            else:
                assert state.counts["/snapshot-flight/generation"] == 2, "snapshot cache generations shared a flight"
                assert state.snapshot_downloads, "explicit download snapshot tests did not run"
                assert all(agent == "Pd/snapshot-download-" + path.rsplit("-", 1)[-1]
                           for path, agent, ranged in state.snapshot_downloads), "download phases used changed defaults"
                assert any(path.endswith("official-A") and ranged for path, agent, ranged in state.snapshot_downloads), "snapshot range workers did not run"
                assert state.closed["/snapshot-artifact/slow-A"] >= 1, "snapshot cancellation did not close download socket"
            assert state.closed["/abort-cache"] >= 1, "last cache subscriber did not close socket"
            assert state.connects and all(value.startswith(("127.0.0.1:", "[::1]:")) for value in state.connects), "CONNECT used unresolved hostname"
        print("PASS Pd socket peer confirms snapshot isolation, explicit download phases, cache generation and physical abort" if args.snapshots else
              "PASS Python socket peer confirms physical abort, numeric CONNECT, retries and actual TLS/download traffic")
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
