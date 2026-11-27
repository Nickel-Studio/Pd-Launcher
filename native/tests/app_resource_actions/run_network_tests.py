#!/usr/bin/env python3
"""Run production resource tests behind an isolated, pinned-CA CONNECT/TLS fixture."""

import argparse
import hashlib
import http.client
import io
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit
import zipfile


HOSTS = {"api.modrinth.com", "cdn.modrinth.com", "api.curseforge.com", "mod.mcimirror.top"}
LIMIT = 256 * 1024


def jar(version, padding=0):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_STORED) as archive:
        entries = {
            "fabric.mod.json": json.dumps({"schemaVersion": 1, "id": "fixturemod", "version": version,
                                          "name": "Controlled Network Mod"}),
            "META-INF/MANIFEST.MF": "Manifest-Version: 1.0\n",
            "fixture.txt": "verified production network bytes " + version,
        }
        if padding:
            entries["padding.bin"] = "x" * padding
        for name, content in entries.items():
            entry = zipfile.ZipInfo(name, (2024, 1, 1, 0, 0, 0))
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, content)
    return stream.getvalue()


def hashes(data):
    return {"sha1": hashlib.sha1(data).hexdigest(), "sha512": hashlib.sha512(data).hexdigest()}


def issue_certificates(directory, openssl, crl_url=None):
    directory = Path(directory)
    common = [openssl]
    commands = [
        common + ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem",
                  "-subj", "/CN=Nickel Resource Fixture CA " + directory.name, "-days", "2", "-sha256",
                  "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign",
                  "-addext", "subjectKeyIdentifier=hash"],
        common + ["req", "-newkey", "rsa:2048", "-nodes", "-keyout", "server.key", "-out", "server.csr",
                  "-subj", "/CN=api.modrinth.com"],
        common + ["x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
                  "-out", "server.pem", "-days", "2", "-sha256", "-extfile", "server.ext"],
    ]
    extensions = (
        "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
        "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n"
        "extendedKeyUsage=serverAuth\nsubjectAltName=" + ",".join("DNS:" + host for host in sorted(HOSTS)) + "\n")
    if crl_url:
        parsed = urlsplit(crl_url)
        if parsed.scheme != "http" or parsed.hostname != "127.0.0.1" or not parsed.port:
            raise ValueError("Fixture revocation material must come from its own loopback server")
        extensions += "crlDistributionPoints=URI:" + crl_url + "\n"
        (directory / "index.txt").write_text("", encoding="ascii")
        (directory / "serial").write_text("1000\n", encoding="ascii")
        (directory / "crlnumber").write_text("1000\n", encoding="ascii")
        (directory / "ca.cnf").write_text(
            "[ca]\ndefault_ca=fixture\n[fixture]\ndatabase=index.txt\nserial=serial\ncrlnumber=crlnumber\n"
            "certificate=ca.pem\nprivate_key=ca.key\ndefault_md=sha256\ndefault_crl_days=2\ncrl_extensions=crl_ext\n"
            "[crl_ext]\nauthorityKeyIdentifier=keyid:always\n", encoding="ascii")
        commands += [common + ["ca", "-gencrl", "-config", "ca.cnf", "-out", "ca.crl.pem"],
                     common + ["crl", "-in", "ca.crl.pem", "-outform", "DER", "-out", "ca.crl"]]
    (directory / "server.ext").write_text(extensions, encoding="ascii")
    for command in commands:
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError("Fixture certificate generation failed: " + result.stderr[-2048:])
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(directory / "server.pem", directory / "server.key")

    def approve_sni(connection, name, _context):
        if name not in HOSTS:
            raise ssl.SSLError("Fixture TLS hostname was not approved")
        connection.fixture_host = name

    context.set_servername_callback(approve_sni)
    return context


class RevocationServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    block_on_close = True
    allow_reuse_address = False

    def __init__(self, directory):
        self.directory = Path(directory)
        self.target = "/" + self.directory.name + "/ca.crl"
        self.requests = 0
        self.lock = threading.Lock()
        super().__init__(("127.0.0.1", 0), RevocationHandler)
        self.url = "http://127.0.0.1:" + str(self.server_address[1]) + self.target


class RevocationHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(5)
        request = bytearray()
        while b"\r\n\r\n" not in request:
            block = self.request.recv(4096)
            if not block or len(request) + len(block) > 16384:
                return
            request.extend(block)
        method, target, protocol = bytes(request).split(b"\r\n", 1)[0].decode("ascii").split(" ", 2)
        if method not in {"GET", "HEAD"} or target != self.server.target or protocol not in {"HTTP/1.0", "HTTP/1.1"}:
            return
        # A signed, empty CRL satisfies Schannel without disabling its revocation checks.
        content = (self.server.directory / "ca.crl").read_bytes()
        if not content or len(content) > LIMIT:
            return
        with self.server.lock:
            self.server.requests += 1
        head = ("HTTP/1.1 200 OK\r\nContent-Type: application/pkix-crl\r\nContent-Length: "
                + str(len(content)) + "\r\nConnection: close\r\n\r\n").encode("ascii")
        self.request.sendall(head + (content if method == "GET" else b""))


class FixtureServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    block_on_close = True
    allow_reuse_address = False

    def __init__(self, context, data_directory):
        self.context = context
        self.data_directory = Path(data_directory)
        self.slots = threading.BoundedSemaphore(8)
        self.lock = threading.Lock()
        self.requests = []
        self.diagnostics = []
        self.artifacts = {"initial.jar": jar("1.0"), "approved.jar": jar("2.0"), "slow.jar": jar("3.0", 128 * 1024)}
        for name, content in self.artifacts.items():
            (self.data_directory / name).write_bytes(content)
        super().__init__(("127.0.0.1", 0), FixtureHandler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def handle_error(self, request, client_address):
        # Preserve bounded handshake evidence; expected cancellations do not become test passes.
        failure = sys.exc_info()[1]
        with self.lock:
            if len(self.diagnostics) < 64:
                self.diagnostics.append(type(failure).__name__ + ": " + str(failure)[:512])

    def version(self, identity):
        mapping = {
            "current1": ("initial.jar", "1.0", "2024-01-01T00:00:00Z"),
            "approved1": ("approved.jar", "2.0", "2025-01-01T00:00:00Z"),
            "update01": ("approved.jar", "2.0", "2025-01-01T00:00:00Z"),
            "broken01": ("approved.jar", "2.0", "2025-01-01T00:00:00Z"),
            "slow0001": ("slow.jar", "3.0", "2025-02-01T00:00:00Z"),
        }
        if identity not in mapping:
            return None
        name, version, published = mapping[identity]
        leaf = "corrupt.jar" if identity == "broken01" else name
        content = self.artifacts[name]
        return {"id": identity, "project_id": "fixture1", "name": "Fixture " + version,
                "version_number": version, "version_type": "release", "date_published": published,
                "loaders": ["fabric"], "game_versions": ["1.20.1"], "dependencies": [],
                "files": [{"filename": name, "url": "https://cdn.modrinth.com/data/fixture1/versions/" + identity + "/" + leaf,
                           "primary": True, "size": len(content), "hashes": hashes(content)}]}

    def route(self, host, target, method, body):
        path = urlsplit(target).path
        if host == "mod.mcimirror.top":
            path = path.replace("/modrinth/v2/", "/v2/", 1).replace("/curseforge/v1/", "/v1/", 1)
        with self.lock:
            self.requests.append({"host": host, "path": path, "method": method})
        if path == "/v2/project/fixture1":
            return 200, "application/json", {"id": "fixture1", "slug": "fixture-mod", "project_type": "mod",
                                             "title": "Controlled Network Mod", "versions": ["1.20.1"], "loaders": ["fabric"]}, False
        if path == "/v2/project/fixture1/version":
            return 200, "application/json", [self.version("current1"), self.version("update01")], False
        if path.startswith("/v2/version/"):
            document = self.version(path.rsplit("/", 1)[-1])
            return (200, "application/json", document, False) if document else (404, "application/json", {}, False)
        if path.startswith("/v2/version_file/"):
            digest = path.rsplit("/", 1)[-1]
            if digest == hashes(self.artifacts["initial.jar"])["sha1"]:
                return 200, "application/json", self.version("current1"), False
            return 404, "application/json", {}, False
        if path == "/v1/fingerprints" and method == "POST":
            document = json.loads(body)
            if not isinstance(document.get("fingerprints"), list):
                return 400, "application/json", {}, False
            return 200, "application/json", {"data": {"exactMatches": []}}, False
        if host == "cdn.modrinth.com" and path.startswith("/data/fixture1/versions/"):
            name = path.rsplit("/", 1)[-1]
            if name == "corrupt.jar":
                content = self.artifacts["approved.jar"]
                return 200, "application/java-archive", b"X" + content[1:], False
            if name in self.artifacts:
                if name == "slow.jar":
                    with self.lock:
                        count = sum(request["path"].endswith("/slow.jar") for request in self.requests)
                    (self.data_directory / ("slow-started-" + str(count))).write_text("TLS request accepted", encoding="ascii")
                return 200, "application/java-archive", self.artifacts[name], name == "slow.jar"
        return 404, "application/json", {}, False


class FixtureHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(15)
        request = bytearray()
        while b"\r\n\r\n" not in request:
            block = self.request.recv(4096)
            if not block or len(request) + len(block) > 16384:
                return
            request.extend(block)
        line = bytes(request).split(b"\r\n", 1)[0].decode("ascii")
        method, target, protocol = line.split(" ", 2)
        if method != "CONNECT" or protocol not in {"HTTP/1.0", "HTTP/1.1"}:
            return
        parsed = urlsplit("//" + target)
        if parsed.port != 443:
            return
        try:
            approved = parsed.hostname in HOSTS or ipaddress.ip_address(parsed.hostname).is_global
        except ValueError:
            approved = False
        if not approved:
            return
        # This proxy never forwards sockets or resolves/contacts the requested server.
        self.request.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        with self.server.context.wrap_socket(self.request, server_side=True) as connection:
            reader = connection.makefile("rb")
            request_line = reader.readline(8193)
            if len(request_line) > 8192:
                return
            method, target, _protocol = request_line.decode("ascii").strip().split(" ", 2)
            headers = {}
            header_size = 0
            while True:
                line = reader.readline(8193)
                header_size += len(line)
                if header_size > 32768 or not line:
                    return
                if line == b"\r\n":
                    break
                name, value = line.decode("ascii").split(":", 1)
                headers[name.lower()] = value.strip()
            host = headers.get("host", "").split(":", 1)[0]
            if host not in HOSTS or host != getattr(connection, "fixture_host", None):
                return
            length = int(headers.get("content-length", "0"))
            if not 0 <= length <= LIMIT or method not in {"GET", "POST", "HEAD"}:
                return
            body = reader.read(length)
            if len(body) != length:
                return
            status, content_type, content, slow = self.server.route(host, target, method, body)
            if not isinstance(content, bytes):
                content = json.dumps(content, separators=(",", ":")).encode("utf-8")
            extra = b""
            if status == 200 and "range" in headers:
                value = headers["range"]
                if not value.startswith("bytes=") or "," in value:
                    return
                first, last = value[6:].split("-", 1)
                start, end = int(first), int(last) if last else len(content) - 1
                if not 0 <= start <= end < len(content):
                    return
                extra = ("Content-Range: bytes " + str(start) + "-" + str(end) + "/" + str(len(content)) + "\r\n").encode("ascii")
                content = content[start:end + 1]
                status = 206
            reason = {200: "OK", 206: "Partial Content", 400: "Bad Request", 404: "Not Found"}[status]
            head = ("HTTP/1.1 " + str(status) + " " + reason + "\r\nContent-Length: " + str(len(content))
                    + "\r\nContent-Type: " + content_type + "\r\nConnection: close\r\n").encode("ascii")
            connection.sendall(head + extra + b"\r\n")
            if method != "HEAD":
                for index in range(0, len(content), 4096):
                    connection.sendall(content[index:index + 4096])
                    if slow:
                        time.sleep(0.1)
            reader.close()


def fetch_fixture(server, ca, host, target, trust=True):
    context = ssl.create_default_context(cafile=str(ca)) if trust else ssl.create_default_context()
    with socket.create_connection(server.server_address, timeout=5) as raw:
        raw.sendall(("CONNECT " + host + ":443 HTTP/1.1\r\nHost: " + host + ":443\r\n\r\n").encode("ascii"))
        response = bytearray()
        while not response.endswith(b"\r\n\r\n"):
            response.extend(raw.recv(1))
        with context.wrap_socket(raw, server_hostname=host) as connection:
            connection.sendall(("GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n").encode("ascii"))
            response = http.client.HTTPResponse(connection)
            response.begin()
            return response.status, response.read()


def self_test(server, ca):
    status, data = fetch_fixture(server, ca, "api.modrinth.com", "/v2/version/approved1")
    document = json.loads(data)
    assert status == 200 and document["project_id"] == "fixture1"
    status, artifact = fetch_fixture(server, ca, "cdn.modrinth.com", "/data/fixture1/versions/approved1/approved.jar")
    assert status == 200 and hashes(artifact) == document["files"][0]["hashes"]
    status, corrupt = fetch_fixture(server, ca, "cdn.modrinth.com", "/data/fixture1/versions/broken01/corrupt.jar")
    assert status == 200 and len(corrupt) == len(artifact) and hashes(corrupt) != hashes(artifact)
    try:
        fetch_fixture(server, ca, "api.modrinth.com", "/v2/project/fixture1", trust=False)
    except ssl.SSLCertVerificationError:
        pass
    else:
        raise AssertionError("Unpinned fixture CA was unexpectedly trusted")
    print("PASS isolated CONNECT/TLS fixture self-test (not a production AppServices test)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--openssl", default=shutil.which("openssl"))
    parser.add_argument("--timeout", type=int, default=140)
    arguments = parser.parse_args()
    if not arguments.openssl:
        parser.error("OpenSSL is required to issue a temporary pinned fixture CA")
    if not arguments.self_test and not arguments.executable:
        parser.error("A linked production resource-action test executable is required")
    executable = Path(arguments.executable).resolve() if arguments.executable else None
    if executable and not executable.is_file():
        parser.error("The production test executable does not exist")
    if not 1 <= arguments.timeout <= 180:
        parser.error("Timeout must be between 1 and 180 seconds")
    with tempfile.TemporaryDirectory(prefix=".resource-network-", dir=Path(__file__).resolve().parent) as temporary:
        directory = Path(temporary)
        with RevocationServer(directory) as revocation:
            context = issue_certificates(directory, arguments.openssl, revocation.url)
            revocation_thread = threading.Thread(target=revocation.serve_forever, daemon=True)
            revocation_thread.start()
            try:
                with FixtureServer(context, directory) as server:
                    thread = threading.Thread(target=server.serve_forever, daemon=True)
                    thread.start()
                    try:
                        if arguments.self_test:
                            self_test(server, directory / "ca.pem")
                            return 0
                        environment = os.environ.copy()
                        environment.update({"NICKEL_RESOURCE_TEST_PROXY": "http://127.0.0.1:" + str(server.server_address[1]),
                                            "NICKEL_RESOURCE_TEST_CA": str(directory / "ca.pem"),
                                            "NICKEL_RESOURCE_TEST_DATA": str(directory), "QT_QPA_PLATFORM": "offscreen"})
                        print("FIXTURE TLS pinned-ca-sha256=" + hashlib.sha256((directory / "ca.pem").read_bytes()).hexdigest()
                              + " crl-sha256=" + hashlib.sha256((directory / "ca.crl").read_bytes()).hexdigest(), flush=True)
                        result = subprocess.run([str(executable), "--network"], env=environment, timeout=arguments.timeout)
                        print("Production resource network fixture exit=" + str(result.returncode), flush=True)
                        return result.returncode
                    finally:
                        server.shutdown()
                        thread.join(timeout=5)
                        with server.lock:
                            for request in server.requests:
                                print("FIXTURE " + request["method"] + " https://" + request["host"] + request["path"], flush=True)
                            for diagnostic in server.diagnostics:
                                print("FIXTURE diagnostic " + diagnostic, flush=True)
            finally:
                revocation.shutdown()
                revocation_thread.join(timeout=5)
                print("FIXTURE loopback signed-CRL requests=" + str(revocation.requests), flush=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, AssertionError) as error:
        print("FAIL resource network fixture: " + str(error), flush=True)
        raise SystemExit(2)
