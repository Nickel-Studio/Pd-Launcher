#!/usr/bin/env python3
"""Real Linux child supervision with D-owned logs/temp and exact descendant cancellation."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from audit_support import AuditSafetyError, atomic_json, capacity, owned_output, plain_path, sha256


def proc_identity(pid: int) -> dict:
    stat = (Path("/proc") / str(pid) / "stat").read_text()
    fields = stat.rpartition(") ")[2].split()
    return {"pid": pid, "parent": int(fields[1]), "startTicks": fields[19], "state": fields[0]}


def cancel_descendants(process: subprocess.Popen) -> list[dict]:
    if process.poll() is not None:
        return []
    root = proc_identity(process.pid)
    if root["parent"] != os.getpid():
        raise AuditSafetyError("Owned child parent identity changed; refusing cancellation")
    stopped = {}

    def stop(record: dict) -> None:
        pid = record["pid"]
        descriptor = os.pidfd_open(pid)
        try:
            if proc_identity(pid)["startTicks"] != record["startTicks"]:
                raise AuditSafetyError("PID was reused; refusing cancellation")
            signal.pidfd_send_signal(descriptor, signal.SIGSTOP)
            stopped[pid] = (descriptor, record)
        except BaseException:
            os.close(descriptor)
            raise

    try:
        stop(root)
        for _ in range(8):
            records = {}
            for path in Path("/proc").iterdir():
                if not path.name.isdigit():
                    continue
                try:
                    record = proc_identity(int(path.name))
                    records[record["pid"]] = record
                except (OSError, ValueError):
                    continue
            pending = {process.pid}
            changed = True
            while changed:
                changed = False
                for pid, record in records.items():
                    if record["parent"] in pending and pid not in pending:
                        pending.add(pid)
                        changed = True
            unseen = pending - stopped.keys()
            if not unseen:
                break
            for pid in sorted(unseen):
                try:
                    stop(records[pid])
                except (ProcessLookupError, FileNotFoundError):
                    continue
        # pidfds refer to exact processes, including ninja subcommands in other groups.
        for pid, (descriptor, _) in reversed(list(stopped.items())):
            try:
                signal.pidfd_send_signal(descriptor, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait(timeout=15)
        return [record for _, record in stopped.values()]
    finally:
        for descriptor, _ in stopped.values():
            os.close(descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cwd", required=True, type=Path)
    parser.add_argument("--temp", required=True, type=Path)
    parser.add_argument("--status", required=True, type=Path)
    parser.add_argument("--control", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--token", required=True)
    parser.add_argument("--seconds", required=True, type=int)
    parser.add_argument("--headroom", type=int, default=1024 * 1024 * 1024)
    parser.add_argument("--ram-fixtures", action="store_true")
    parser.add_argument("argv", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    argv = args.argv[1:] if args.argv[:1] == ["--"] else args.argv
    if sys.platform != "linux" or not hasattr(os, "pidfd_open") or args.seconds < 1 or not argv:
        parser.error("A real Linux pidfd host, positive timeout and concrete argv are required")
    for path in (args.status, args.control, args.stdout, args.stderr, args.temp):
        owned_output(path)
    cwd = plain_path(args.cwd, must_exist=True)
    capacity(args.temp, 1024 * 1024, args.headroom)
    args.temp.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(TMPDIR=str(args.temp), TMP=str(args.temp), TEMP=str(args.temp),
               PYTHONDONTWRITEBYTECODE="1")
    state = {"schema": 1, "state": "STARTING", "token": args.token,
             "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(), "argv": argv,
             "cwd": str(cwd), "temp": str(args.temp), "supervisorPid": os.getpid(),
             "exitCode": None, "passed": False, "completeProductVerified": False}
    atomic_json(args.status, state)
    cancelled = []
    reason = None
    started = time.monotonic()
    process = None
    lease = None
    try:
        if args.ram_fixtures:
            from posix_fixtures import RamFixture
            lease = RamFixture()
            fixture = lease.path / "run"
            os.mkdir("run", mode=0o700, dir_fd=lease.fd)
            argv = [*argv, "--fixture-root", str(fixture)]
            env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
                   "TMPDIR": str(args.temp), "TMP": str(args.temp), "TEMP": str(args.temp),
                   "PYTHONDONTWRITEBYTECODE": "1", "HOME": str(fixture / "home"),
                   "XDG_CONFIG_HOME": str(fixture / "xdg-config"), "XDG_DATA_HOME": str(fixture / "xdg-data"),
                   "NICKEL_RAM_FIXTURE_ROOT": str(fixture)}
            state.update(argv=argv, fixtureInitial=lease.capture(), fixturePolicy="Per-run marked tmpfs <=1 MiB; no-follow captured identities; finally cleanup; all logs/temp/build remain D")
            atomic_json(args.status, state)
        with args.stdout.open("xb") as stdout, args.stderr.open("xb") as stderr:
            process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=stdout, stderr=stderr,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            state.update(state="RUNNING", child=proc_identity(process.pid))
            atomic_json(args.status, state)
            while process.poll() is None:
                if time.monotonic() - started >= args.seconds:
                    reason = "TIMED_OUT"
                    break
                if args.control.exists():
                    control = json.loads(args.control.read_text())
                    if control.get("token") != args.token:
                        raise AuditSafetyError("Control token does not identify this owned command")
                    if control.get("action") == "cancel":
                        reason = control.get("reason", "CANCELLED")
                        break
                try:
                    capacity(args.temp, 1024 * 1024, args.headroom)
                    if lease is not None:
                        try:
                            lease.capture()
                        except FileNotFoundError:
                            lease.verify()
                except AuditSafetyError as error:
                    reason = "FIXTURE_OR_CAPACITY_CANCELLED"
                    state["guardError"] = str(error)
                    break
                time.sleep(0.02 if lease is not None else 0.25)
            if reason:
                cancelled = cancel_descendants(process)
            state.update(state=reason or "FINISHED", exitCode=process.wait(timeout=15))
    except BaseException as error:
        state.update(state="SUPERVISION_FAILED", error=str(error))
        if process is not None:
            try:
                cancelled = cancel_descendants(process)
                state["exitCode"] = process.wait(timeout=15)
            except BaseException as cancel_error:
                state["cancellationError"] = str(cancel_error)
    finally:
        if lease is not None:
            if process is not None and process.poll() is None:
                lease.close()
                state.update(state="CLEANUP_BLOCKED_LIVE_CHILD", fixtureCleanupError="Owned command is still live; marked RAM root preserved")
            else:
                try:
                    state["fixtureFinal"] = lease.capture(final=True)
                except BaseException as error:
                    state.update(state="FIXTURE_GUARD_FAILED", fixtureCaptureError=str(error))
                try:
                    state["fixtureCleanup"] = lease.cleanup()
                except BaseException as error:
                    lease.close()
                    state.update(state="FIXTURE_CLEANUP_FAILED", fixtureCleanupError=str(error))
    state.update(finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat(),
                 elapsedSeconds=round(time.monotonic() - started, 3), cancelledExactProcesses=cancelled,
                 stdoutSha256=sha256(args.stdout) if args.stdout.is_file() else None,
                 stderrSha256=sha256(args.stderr) if args.stderr.is_file() else None)
    state["passed"] = state["state"] == "FINISHED" and state["exitCode"] == 0
    atomic_json(args.status, state)
    print(json.dumps({"state": state["state"], "exitCode": state["exitCode"],
                      "fixtureRemoved": state.get("fixtureCleanup", {}).get("removedExactRoot"),
                      "completeProductVerified": False}))
    return 0 if state["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
