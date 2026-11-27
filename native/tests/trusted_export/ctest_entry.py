"""Builder root CTest adapter. Only the pinned small test EXE is copied/executed."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

import run_suite
import trusted_export_tools as tools


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("tools", "app-negative", "actual-pd-not-run"))
    parser.add_argument("--owner", type=Path, required=True)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--graph", type=Path)
    parser.add_argument("--source-commit")
    parser.add_argument("--source-sha256")
    args = parser.parse_args()
    owner = args.owner.resolve(strict=True)
    tools.require(owner == Path(__file__).resolve().parent, "unauthorized", "CTest owner path is not this harness")
    run_suite.disk_capacity()
    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    delivery = owner / "runs" / ("ctest-input-" + stamp)
    delivery.mkdir(parents=True, exist_ok=False)
    output = owner / "runs" / (args.mode + "-" + stamp)
    command = [sys.executable, "-B", str(owner / "run_suite.py"), args.mode, "--output", str(output)]
    if args.mode == "app-negative":
        tools.require(args.exe is not None and args.graph is not None, "missing_service", "Current root test/graph paths are required")
        source = args.exe.resolve(strict=True)
        source_snapshot = tools.file_snapshot(source)
        exe = delivery / "pd_trusted_export_security_tests.exe"
        with source.open("rb") as reader, exe.open("xb") as writer:
            shutil.copyfileobj(reader, writer, 64 * 1024)
        tools.unchanged(source_snapshot, source)
        copied = tools.file_snapshot(exe, owner)
        tools.require(copied["sha256"] == source_snapshot["sha256"], "integrity", "CTest closed EXE copy differs")
        graph_bytes = args.graph.resolve(strict=True).read_bytes()
        graph = delivery / "graph.json"
        with graph.open("xb") as writer:
            writer.write(graph_bytes)
        command.extend(["--exe", str(exe), "--exe-sha256", copied["sha256"], "--graph", str(graph),
                        "--graph-sha256", hashlib.sha256(graph_bytes).hexdigest(),
                        "--source-commit", args.source_commit or "", "--source-sha256", args.source_sha256 or ""])
    tools.write_new_json(delivery / "ctest-handoff.json", {"schema": "pd.trusted-export.ctest-handoff.v1",
                         "mode": args.mode, "command": command, "actualPdExecuted": False})
    return subprocess.run(command, cwd=owner, shell=False).returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except tools.Rejected as error:
        print("CTest adapter failed closed: " + error.code + ": " + str(error), file=sys.stderr)
        raise SystemExit(1)
