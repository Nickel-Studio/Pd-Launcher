import argparse
from collections import Counter
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlsplit
import zipfile
import zlib


def fingerprint(data):
    data = bytes(value for value in data if value not in (9, 10, 13, 32))
    result = 1 ^ len(data)
    multiplier = 0x5BD1E995
    at = 0
    while at + 4 <= len(data):
        value = int.from_bytes(data[at:at + 4], "little")
        value = (value * multiplier) & 0xFFFFFFFF
        value ^= value >> 24
        value = (value * multiplier) & 0xFFFFFFFF
        result = ((result * multiplier) & 0xFFFFFFFF) ^ value
        at += 4
    tail = data[at:]
    if tail:
        result ^= int.from_bytes(tail, "little")
        result = (result * multiplier) & 0xFFFFFFFF
    result ^= result >> 13
    result = (result * multiplier) & 0xFFFFFFFF
    return result ^ (result >> 15)


def png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\xff")) + chunk(b"IEND", b""))


class State:
    def __init__(self, root):
        self.lock = threading.Lock()
        self.counts = Counter()
        self.closed = Counter()
        self.headers = {}
        self.http = ""
        self.http2 = ""
        self.mod_fail_next = False
        self.snapshot_release = threading.Event()
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("fabric.mod.json", json.dumps({"schemaVersion": 1, "id": "fixture", "version": "2.0", "name": "Socket fixture", "icon": "assets/icon.png"}))
            archive.writestr("assets/icon.png", png())
            archive.writestr("payload.bin", bytes(range(256)) * 512)
        self.jar = buffer.getvalue()
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("fabric.mod.json", json.dumps({"schemaVersion": 1, "id": "fixture", "version": "1.0", "name": "Installed fixture"}))
            archive.writestr("payload.bin", bytes(reversed(range(256))) * 512)
        self.initial = buffer.getvalue()
        self.initial_sha1 = hashlib.sha1(self.initial).hexdigest()
        self.initial_sha512 = hashlib.sha512(self.initial).hexdigest()
        self.initial_sha256 = hashlib.sha256(self.initial).hexdigest()
        self.invalid = b"x" * len(self.jar)
        self.sha1 = hashlib.sha1(self.jar).hexdigest()
        self.sha512 = hashlib.sha512(self.jar).hexdigest()
        self.sha256 = hashlib.sha256(self.jar).hexdigest()
        self.fingerprint = fingerprint(self.jar)
        (root / "fixture.jar").write_bytes(self.jar)
        (root / "initial.jar").write_bytes(self.initial)

    def version(self, secondary=False, local=False):
        matched = {"url": "https://cdn.modrinth.com/data/p1/versions/v1/fixture.jar", "filename": "fixture.jar",
                   "size": len(self.jar), "primary": not secondary, "hashes": {"sha1": self.sha1, "sha512": self.sha512}}
        if local:
            matched["url"] = self.http + "/files/caller-mod-update.jar"
        files = [matched]
        if secondary:
            matched["filename"] = "matched.jar"
            matched["url"] = "https://cdn.modrinth.com/data/p1/versions/v1/matched.jar"
            files.insert(0, {"url": "https://cdn.modrinth.com/data/p1/versions/v1/primary.jar", "filename": "primary.jar", "size": 9,
                             "primary": True, "hashes": {"sha1": "1" * 40, "sha512": "2" * 128}})
        return {"id": "v1", "project_id": "p1", "name": "Fixture", "version_number": "2.0", "loaders": ["fabric"],
                "game_versions": ["1.20.1"], "date_published": "2026-01-01T00:00:00Z", "version_type": "release", "files": files, "dependencies": []}

    def initial_version(self):
        return {"id": "v0", "project_id": "p1", "name": "Installed fixture", "version_number": "1.0", "loaders": ["fabric"],
                "game_versions": ["1.20.1"], "date_published": "2025-01-01T00:00:00Z", "version_type": "release", "dependencies": [],
                "files": [{"url": "https://cdn.modrinth.com/data/p1/versions/v0/initial.jar", "filename": "initial.jar", "primary": True,
                           "size": len(self.initial), "hashes": {"sha1": self.initial_sha1, "sha512": self.initial_sha512}}]}

    def snapshot(self):
        with self.lock:
            return {"counts": dict(self.counts), "closed": dict(self.closed), "headers": dict(self.headers)}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def begin(self):
        path = urlsplit(self.path).path
        with self.server.state.lock:
            self.server.state.counts[path] += 1
            self.server.state.headers[path] = {key.lower(): value for key, value in self.headers.items()}
        return path

    def send_body(self, body, status=200, headers=()):
        if isinstance(body, dict) or isinstance(body, list):
            body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for key, value in headers:
            self.send_header(key, value)
        self.end_headers()
        self.close_connection = True
        self.wfile.write(body)
        self.wfile.flush()

    def peer_closed(self, path):
        with self.server.state.lock:
            self.server.state.closed[path] += 1

    def do_GET(self):
        path = self.begin()
        state = self.server.state
        try:
            if path == "/stats":
                self.send_body(state.snapshot())
            elif path in ("/json", "/echo", "/echo-snapshot"):
                self.send_body({"ok": True})
            elif path == "/redirect-snapshot":
                if not state.snapshot_release.wait(5):
                    self.send_body({"error": "snapshot release timeout"}, 500)
                else:
                    self.send_body(b"", 302, (("Location", "/echo-snapshot"),))
            elif path == "/images/snapshot.png":
                self.send_body(png(), headers=(("Content-Type", "image/png"),))
            elif path == "/redirect":
                self.send_body(b"", 302, (("Location", "/json"),))
            elif path == "/redirect-cross":
                self.send_body(b"", 302, (("Location", state.http2 + "/echo"),))
            elif path == "/redirect-private":
                self.send_body(b"", 302, (("Location", "http://192.168.1.1/secret"),))
            elif path == "/redirect-loop":
                self.send_body(b"", 302, (("Location", "/redirect-loop"),))
            elif path.startswith("/silent-"):
                self.connection.settimeout(8)
                try:
                    while self.connection.recv(4096):
                        pass
                    self.peer_closed(path)
                except (ConnectionResetError, BrokenPipeError):
                    self.peer_closed(path)
                except socket.timeout:
                    pass
                self.close_connection = True
            elif path == "/slow-cache":
                time.sleep(0.25)
                self.send_body({"ok": True}, headers=(("Cache-Control", "max-age=60"),))
            elif path == "/etag":
                headers = (("ETag", '"fixture-v1"'), ("Cache-Control", "max-age=0"))
                self.send_body(b"" if self.headers.get("If-None-Match") else b"etag", 304 if self.headers.get("If-None-Match") else 200, headers)
            elif path == "/big":
                self.send_body(b"x" * 4096)
            elif path == "/chunked-big":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n1000\r\n" + b"x" * 4096 + b"\r\n0\r\n\r\n")
                self.close_connection = True
            elif path == "/truncated":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 200\r\nConnection: close\r\n\r\npartial")
                self.close_connection = True
            elif path == "/invalid-archive":
                self.send_body(state.invalid)
            elif path == "/files/caller-mod-update.jar":
                with state.lock:
                    invalid = state.mod_fail_next
                    state.mod_fail_next = False
                self.send_body(state.invalid if invalid else state.jar, headers=(("Content-Type", "application/java-archive"),))
            elif path.startswith("/files/") and path.endswith(".jar"):
                self.send_body(state.jar, headers=(("Content-Type", "application/java-archive"),))
            elif path in ("/mr/v2/project/p1", "/mods-mr/v2/project/p1"):
                self.send_body({"id": "p1", "slug": "fixture", "project_type": "mod", "title": "Fixture", "description": "Summary",
                                "body": "# Full description\n\n**not a summary**", "loaders": ["fabric"], "game_versions": ["1.20.1"]})
            elif path == "/mods-mr/v2/project/p1/version":
                self.send_body([state.version(local=True), state.initial_version()])
            elif path == "/mods-mr/v2/version/v1":
                self.send_body(state.version(local=True))
            elif path.startswith("/mods-mr/v2/version_file/"):
                sha1 = path.rsplit("/", 1)[-1]
                self.send_body(state.initial_version() if sha1 == state.initial_sha1 else state.version(local=True))
            elif path == "/mr/v2/version/v1":
                self.send_body(state.version())
            elif path.startswith("/mr/v2/version_file/"):
                self.send_body(state.version(True))
            else:
                self.send_body({"error": "not found"}, 404)
        except (BrokenPipeError, ConnectionResetError):
            self.peer_closed(path)
            self.close_connection = True

    def do_POST(self):
        path = self.begin()
        payload = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        state = self.server.state
        if path == "/release-snapshot":
            state.snapshot_release.set()
            self.send_body(b"released")
        elif path == "/fail-next-mod-download":
            with state.lock:
                state.mod_fail_next = True
            self.send_body(b"armed")
        elif path == "/redirect-post-cross":
            self.send_body(b"", 307, (("Location", state.http2 + "/echo"),))
        elif path == "/cf/v1/fingerprints":
            value = json.loads(payload)
            requested = value["fingerprints"][0]
            file = {"id": 101, "modId": 42, "displayName": "Fixture", "fileName": "fixture.jar", "fileLength": len(state.jar),
                    "downloadUrl": "https://edge.forgecdn.net/files/1/1/fixture.jar", "fileFingerprint": requested,
                    "gameVersions": ["1.20.1", "Fabric"], "hashes": [{"algo": 1, "value": state.sha1}], "releaseType": 1, "dependencies": []}
            self.send_body({"data": {"exactMatches": [{"file": file}]}})
        else:
            self.send_body(payload)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    args.workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="community-loopback-", dir=args.workspace) as temporary:
        root = Path(temporary)
        state = State(root)
        servers = [ThreadingHTTPServer(("127.0.0.1", 0), Handler) for _ in range(2)]
        for server in servers:
            server.daemon_threads = True
            server.state = state
        origins = [f"http://127.0.0.1:{server.server_port}" for server in servers]
        state.http, state.http2 = origins
        config = {"http": origins[0], "http2": origins[1], "jar": str(root / "fixture.jar"), "size": len(state.jar),
                  "sha1": state.sha1, "sha512": state.sha512, "sha256": state.sha256, "fingerprint": state.fingerprint,
                  "initial_jar": str(root / "initial.jar"), "initial_sha1": state.initial_sha1,
                  "initial_sha512": state.initial_sha512, "initial_sha256": state.initial_sha256,
                  "invalid_sha1": hashlib.sha1(state.invalid).hexdigest(), "invalid_sha512": hashlib.sha512(state.invalid).hexdigest()}
        file = root / "fixture.json"
        file.write_text(json.dumps(config), encoding="utf-8")
        try:
            for server in servers:
                threading.Thread(target=server.serve_forever, daemon=True).start()
            result = subprocess.run([str(args.executable.resolve()), str(file)], timeout=90)
            if result.returncode:
                raise SystemExit(result.returncode)
            snapshot = state.snapshot()
            for path in ("/silent-direct", "/silent-cache", "/silent-install"):
                if snapshot["closed"].get(path, 0) < 1:
                    raise AssertionError("Real socket was not closed after cancellation: " + path)
            if snapshot["counts"].get("/slow-cache") != 1:
                raise AssertionError("Coalesced cache performed more than one physical request")
            print("PASS independent loopback peers confirm physical cancellation and cache coalescing")
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
