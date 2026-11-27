"""Test-owned bounded child output. Overflow fails instead of consuming the last disk bytes."""
from __future__ import annotations

from pathlib import Path
import subprocess
import threading
import time

from evidence_common import EvidenceError, check_space


class BoundedProcess:
    def __init__(self, argv: list[str], *, cwd: Path, environment: dict, log: Path, limit_bytes: int = 2 * 1024 * 1024):
        if type(limit_bytes) is not int or limit_bytes < 1:
            raise EvidenceError("A positive owned child-output bound is required")
        check_space(log, limit_bytes)
        self.log = log
        self.stream = log.open("xb")
        self.limit = limit_bytes
        self.bytes = self.bytes_seen = 0
        self.error = None
        try:
            # Exclude evidence-log preparation from the product process-creation clock.
            self.creation_begin_ns = time.perf_counter_ns()
            self.process = subprocess.Popen(argv, cwd=cwd, env=environment, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            self.creation_end_ns = time.perf_counter_ns()
        except Exception:
            self.stream.close()
            raise
        self.thread = threading.Thread(target=self._drain, name="owned-output-drain", daemon=True)
        self.thread.start()

    def _drain(self):
        try:
            while True:
                chunk = self.process.stdout.read(16384)
                if not chunk:
                    break
                self.bytes_seen += len(chunk)
                if self.error:
                    continue
                if self.bytes + len(chunk) > self.limit:
                    self.error = "Owned subprocess output exceeded bounded log budget; raw log is incomplete"
                    continue
                check_space(self.log, len(chunk))
                self.stream.write(chunk)
                self.bytes += len(chunk)
        except (OSError, EvidenceError) as error:
            self.error = str(error)
        finally:
            try:
                self.stream.flush()
            except OSError as error:
                self.error = str(error)
            finally:
                self.stream.close()
                self.process.stdout.close()

    def poll(self):
        return self.process.poll()

    def wait(self, timeout: float):
        result = self.process.wait(timeout=timeout)
        self.thread.join(timeout=5)
        if self.thread.is_alive():
            raise EvidenceError("Owned output pipe not drained after child exit; inherited handle is still open")
        if self.error:
            raise EvidenceError(self.error)
        return result

    def terminate(self):
        # This Popen owns exactly one helper; never match processes by name.
        if self.process.poll() is None:
            self.process.terminate()

    @property
    def pid(self):
        return self.process.pid

    @property
    def returncode(self):
        return self.process.returncode
