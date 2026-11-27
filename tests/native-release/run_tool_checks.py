"""Persist independent release-tool tests, command hashes and actual observer selftest."""
from __future__ import annotations

import argparse
import ast
import os
from pathlib import Path
import subprocess
import sys
import time

from bounded_process import BoundedProcess
from evidence_common import (ROOT, LOCAL, EvidenceError, SingleWorker, exit_code, file_ref, gate, inside,
                             new_output, overall, read_json, utc_now, write_json)
import observer_selftest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--no-live-observer", action="store_true", help="Only synthetic tools; live helper/WMI/GPU remain explicitly pending")
    args = parser.parse_args()
    output = new_output(args.output)
    temp = output / "temp"
    temp.mkdir()
    environment = os.environ.copy()
    environment.update({"TEMP": str(temp), "TMP": str(temp), "PYTHONDONTWRITEBYTECODE": "1", "NO_COLOR": "1"})
    if args.no_live_observer:
        environment["PD_RELEASE_TOOLS_NO_LIVE_OBSERVER"] = "1"
    os.environ.update({"TEMP": str(temp), "TMP": str(temp), "PYTHONDONTWRITEBYTECODE": "1"})
    began, syntax, errors, schemas = time.perf_counter(), [], [], []
    for path in sorted((ROOT / "tests/native-release").glob("*.py")):
        try:
            ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except (OSError, SyntaxError, UnicodeError) as error:
            errors.append(str(error))
        syntax.append(file_ref(path))
    for path in sorted((ROOT / "tests/native-release").glob("*.schema.json")):
        try:
            read_json(path)
        except (OSError, EvidenceError) as error:
            errors.append(str(error))
        schemas.append(file_ref(path))
    command = [sys.executable, "-B", "-m", "unittest", "discover", "-s", "tests/native-release", "-p", "test_*.py", "-v"]
    code = None
    with SingleWorker(inside(ROOT, LOCAL)):
        process = None
        try:
            process = BoundedProcess(command, cwd=ROOT, environment=environment, log=output / "tool-tests.raw.log")
            code = process.wait(90)
        except (OSError, EvidenceError, subprocess.SubprocessError) as error:
            errors.append(str(error))
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(5)
                except (OSError, EvidenceError, subprocess.SubprocessError):
                    pass
        observer_dir = output / "observer"
        observer_dir.mkdir()
        if args.no_live_observer:
            observer = {"kind": "release-tool-observer-selftest", "scope": "owned-helper-only", "status": "pending",
                        "executed": False, "nativeReleaseReady": False,
                        "reason": "No live observer/probe was authorized in this synthetic WIP-contract checkpoint"}
            write_json(observer_dir / "observer-selftest.json", observer)
        else:
            observer = observer_selftest.run(observer_dir)
    gates = {"syntax": gate("failed" if errors else "passed", "Python AST/schema JSON parse and bounded command completion", errors=errors),
             "regressionTests": gate("passed" if code == 0 else "failed", "Synthetic tool discovery, skipped live cases retained in raw log", exitCode=code),
             "observer": gate(observer["status"], "Live checks are independent; unexecuted/WMI/GPU absence stays pending", gates=observer.get("gates"))}
    report = {"schemaVersion": 1, "kind": "native-release-tool-selftest", "scope": "tools-only-not-launcher",
              "status": overall(gates), "gates": gates, "nativeReleaseReady": False, "createdUtc": utc_now(), "argv": command,
              "exitCode": code, "elapsedSeconds": time.perf_counter() - began,
              "testLog": file_ref(output / "tool-tests.raw.log") if (output / "tool-tests.raw.log").is_file() else None,
              "sourceSyntaxAndHashes": syntax, "observerEvidence": file_ref(observer_dir / "observer-selftest.json"),
              "inputManifestSchema": file_ref(ROOT / "tests/native-release/artifact-manifest.schema.json"), "testedSchemas": schemas,
              "limitations": ["Regression manifests/receipts/fixtures are synthetic tool tests, not a release artifact.",
                              "Actual PSAPI/PDH/WMI or skipped helper cases never claim launcher savings or unavailable telemetry as zero."]}
    write_json(output / "tool-checks.json", report)
    print(f"{report['status']}: {output.relative_to(ROOT).as_posix()}/tool-checks.json")
    return exit_code(report["status"])


if __name__ == "__main__":
    raise SystemExit(main())
