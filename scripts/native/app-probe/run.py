#!/usr/bin/env python3
"""Run real native application adapters against private files and controlled HTTP only."""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/probe")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path in {"/escape", "/cross-origin"}:
            target = "http://localhost:1/probe" if self.path == "/escape" else self.server.alternate + "/probe"
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/slow":
            time.sleep(0.6)
        body = b"x" * 4096 if self.path == "/oversize" else b"native-shared-http"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


def main() -> int:
    executable = Path(sys.argv[1]).resolve(strict=True)
    servers = [ThreadingHTTPServer(("127.0.0.1", 0), Handler) for _ in range(2)]
    origins = [f"http://127.0.0.1:{server.server_port}" for server in servers]
    for index, server in enumerate(servers):
        server.requests = []
        server.alternate = origins[1 - index]
        threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="nickel-app-adapter-") as directory:
            result = subprocess.run([str(executable), directory, *origins], capture_output=True, text=True, timeout=45)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if servers[1].requests:
            raise AssertionError("The credential-bearing redirect reached a second origin")
        return result.returncode
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
