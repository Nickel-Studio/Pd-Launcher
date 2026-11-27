"""Bounded test-owned WMI start/stop trace supplementing birth-identified snapshots."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import time

from bounded_process import BoundedProcess
from evidence_common import EvidenceError, ROOT, file_ref, read_json, write_new


class EventTrace:
    def __init__(self, directory: Path, duration: int):
        self.directory = directory
        self.duration = duration
        self.process = None
        self.events = directory / "process-events.jsonl"
        self.ready = directory / "trace-ready.json"
        self.stop = directory / "trace-stop"
        self.script = ROOT / "tests/native-release/process_trace.ps1"
        self.reason = None
        self.subscribed = False

    def start(self):
        if os.name != "nt":
            self.reason = "WMI process event trace requires an actual Windows host"
            return
        argv = ["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                "-File", str(self.script), "-Events", str(self.events), "-Ready", str(self.ready),
                "-Stop", str(self.stop), "-DurationSeconds", str(self.duration)]
        environment = os.environ.copy()
        environment.update({"TEMP": str(self.directory), "TMP": str(self.directory), "PYTHONDONTWRITEBYTECODE": "1"})
        try:
            self.process = BoundedProcess(argv, cwd=ROOT, environment=environment, log=self.directory / "trace-command.log")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if self.ready.exists():
                    receipt = read_json(self.ready)
                    if receipt.get("status") == "subscribed" and receipt.get("collectorPid") == self.process.pid:
                        self.subscribed = True
                        return
                    break
                if self.process.poll() is not None or self.process.error:
                    break
                time.sleep(0.05)
            self.reason = "WMI start/stop trace did not subscribe before launch; complete lifetime observation is pending"
        except (OSError, EvidenceError) as error:
            self.reason = str(error)

    def finish(self) -> dict:
        if self.process:
            try:
                write_new(self.stop, b"owned trace stop\n")
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(5)
                except (OSError, EvidenceError, subprocess.SubprocessError):
                    pass
                self.reason = "Owned WMI trace helper timed out during final drain"
            except (OSError, EvidenceError, subprocess.SubprocessError) as error:
                self.reason = str(error)
                if self.process.poll() is None:
                    self.process.terminate()
                    try:
                        self.process.wait(5)
                    except (OSError, EvidenceError, subprocess.SubprocessError):
                        pass
        try:
            events = self.read_events()
        except (OSError, EvidenceError) as error:
            self.reason, events = str(error), []
        errors = [row for row in events if row.get("kind") == "collector-error"]
        if errors:
            self.reason = "; ".join(str(row.get("error", "WMI collector error")) for row in errors)
        complete = bool(events and events[-1].get("kind") == "collector-complete")
        if not complete and not self.reason:
            self.reason = "Process trace did not produce a completion marker"
        return {"status": "observed" if self.subscribed and complete and not self.reason else "unavailable", "reason": self.reason,
                "events": file_ref(self.events) if self.events.is_file() else None,
                "commandLog": file_ref(self.directory / "trace-command.log") if (self.directory / "trace-command.log").is_file() else None,
                "collector": file_ref(self.script), "eventCount": len(events),
                "limitations": ["WMI delivery has no zero-loss contract; all live descendants are additionally enumerated.",
                                "Short-lived traced descendants without CPU memory/module observation remain explicit pending."]}

    def read_events(self) -> list[dict]:
        if not self.events.is_file():
            return []
        if self.events.stat().st_size > 16 * 1024 * 1024:
            raise EvidenceError("Process event trace exceeded 16 MiB")
        rows = []
        with self.events.open(encoding="utf-8-sig") as stream:
            for line in stream:
                try:
                    row = json.loads(line)
                except (ValueError, UnicodeError) as error:
                    raise EvidenceError("Incomplete/malformed process event trace is not a complete lifetime") from error
                if not isinstance(row, dict):
                    raise EvidenceError("Process event trace must contain JSON objects")
                rows.append(row)
        return rows


def trace_descendants(events: list[dict], root_pid: int, root_birth: int, observed: set[tuple[int, int]], *,
                      trace_observed: bool = False) -> dict:
    if type(root_pid) is not int or type(root_birth) is not int or root_pid < 1 or root_birth < 1:
        raise EvidenceError("Lifetime correlation requires an actual root PID and kernel creation identity")
    active = {root_pid: {"birth": root_birth, "root": True}}
    descendants, last_stop, ambiguous, ordered = [], {}, [], []
    for row in events:
        if not isinstance(row, dict) or row.get("kind") not in {"start", "stop"}:
            continue
        if any(type(row.get(key)) is not int for key in ("pid", "eventTime100ns")):
            raise EvidenceError("Malformed process lifetime event identity")
        if row["eventTime100ns"] >= root_birth:
            ordered.append(row)
    root_started = root_stopped = False
    for row in sorted(ordered, key=lambda value: value["eventTime100ns"]):
        pid, stamp = row["pid"], row["eventTime100ns"]
        if row["kind"] == "stop":
            record = active.pop(pid, None)
            last_stop[pid] = stamp
            if record and record.get("root"):
                root_stopped = True
            elif record:
                record["traceStopTime100ns"] = stamp
            continue
        if pid == root_pid:
            if root_started or root_stopped:
                ambiguous.append({"pid": pid, "reason": "Root PID has multiple traced incarnations"})
            else:
                root_started = True
            continue
        parent = row.get("parentPid")
        if type(parent) is not int or parent not in active or pid == parent:
            continue
        if stamp < active[parent]["birth"]:
            continue
        if pid in active:
            ambiguous.append({"pid": pid, "reason": "Repeated start without an intervening stop; PID incarnation is ambiguous"})
        record = {**row, "birth": stamp, "previousStopTime100ns": last_stop.get(pid), "traceStopTime100ns": None}
        descendants.append(record)
        active[pid] = record
    missed, matched = [], {(root_pid, root_birth)}
    for record in descendants:
        # WMI TIME_CREATED is an event time, NOT the kernel process creation time.
        births = [birth for pid, birth in observed if pid == record["pid"] and root_birth <= birth <= record["eventTime100ns"] and
                  (record["previousStopTime100ns"] is None or birth > record["previousStopTime100ns"])]
        record["observedCreationTime100ns"] = births[0] if len(births) == 1 else None
        if len(births) != 1:
            missed.append(record)
        else:
            matched.add((record["pid"], births[0]))
    untraced = [{"pid": pid, "creationTime100ns": birth} for pid, birth in sorted(observed - matched)]
    missing_stops = [{"pid": row["pid"], "eventTime100ns": row["eventTime100ns"]} for row in descendants if row["traceStopTime100ns"] is None]
    complete = bool(trace_observed and (root_pid, root_birth) in observed and root_started and root_stopped and
                    not missed and not untraced and not missing_stops and not ambiguous)
    return {"traceObserved": trace_observed, "allTracedDescendantsCount": len(descendants), "descendants": descendants,
            "unobservedShortLivedDescendants": missed, "untracedObservedDescendants": untraced,
            "missingStopEvents": missing_stops, "ambiguousIncarnations": ambiguous,
            "rootStartObserved": root_started, "rootStopObserved": root_stopped, "completeObservedLifetime": complete,
            "identityLimit": "Only traced lifetimes with one matching kernel birth and a stop are complete; unavailable trace or PID reuse never becomes zero loss."}
