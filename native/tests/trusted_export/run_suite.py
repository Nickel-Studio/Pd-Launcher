"""Run only owned small tools or an explicitly pinned builder-delivered closed EXE."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

import trusted_export_tools as tools

HERE = Path(__file__).resolve().parent
OWNER = HERE.parents[2]


def owned(path: Path, must_exist=False) -> Path:
    candidate = Path(os.path.abspath(path))
    tools.require(candidate == HERE or candidate.is_relative_to(HERE), "unauthorized", "Evidence/inputs must be below trusted_export")
    current = candidate
    while current != current.parent:
        if current.exists():
            info = current.lstat()
            tools.require(not current.is_symlink() and not getattr(info, "st_file_attributes", 0) & 0x400,
                          "unsafe_path", "Owned path contains a redirect")
        current = current.parent
    if must_exist:
        tools.require(candidate.exists(), "missing_service", "Owned input is absent")
    return candidate


def disk_capacity() -> dict:
    if os.name == "nt":
        values = {drive: shutil.disk_usage(drive + ":\\").free for drive in ("C", "D")}
        tools.capacity_gate(values["C"], values["D"])
        return values
    value = shutil.disk_usage(HERE).free
    tools.require(value >= tools.GIB, "capacity", "Owned tool filesystem needs 1GiB free")
    return {"owned": value}


def log_command(command: list[str], directory: Path, environment: dict[str, str], timeout: int) -> dict:
    stdout = directory / "stdout.log"
    stderr = directory / "stderr.log"
    started = dt.datetime.now(dt.UTC).isoformat()
    code = None
    failure = None
    with stdout.open("xb") as out, stderr.open("xb") as err:
        try:
            result = subprocess.run(command, cwd=directory, env=environment, stdout=out, stderr=err,
                                    timeout=timeout, shell=False)
            code = result.returncode
        except subprocess.TimeoutExpired:
            failure = "owned-child-timeout"
        except OSError as error:
            failure = type(error).__name__
    result = {"argv": command, "cwd": str(directory), "startedUtc": started,
              "finishedUtc": dt.datetime.now(dt.UTC).isoformat(), "returnCode": code, "failure": failure}
    for name, path in (("stdout", stdout), ("stderr", stderr)):
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        result[name] = {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}
    return result


def environment(directory: Path) -> dict[str, str]:
    result = dict(os.environ)
    for name in ("APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "TEMP", "TMP"):
        path = directory / "environment" / name.lower()
        path.mkdir(parents=True, exist_ok=False)
        result[name] = str(path)
    result["PYTHONDONTWRITEBYTECODE"] = "1"
    result["NO_COLOR"] = "1"
    result["PYTHON_COLORS"] = "0"
    result["QT_QPA_PLATFORM"] = "offscreen"
    result.pop("QT_PLUGIN_PATH", None)
    result.pop("QML2_IMPORT_PATH", None)
    result.pop("QML_IMPORT_PATH", None)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("tools", "app-negative", "actual-pd-not-run", "audit-package"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--exe-sha256")
    parser.add_argument("--graph", type=Path)
    parser.add_argument("--graph-sha256")
    parser.add_argument("--source-commit")
    parser.add_argument("--source-sha256")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--compiled-arch", default="x86_64")
    parser.add_argument("--notices", type=Path)
    options = parser.parse_args()
    output = owned(options.output)
    tools.require(not output.exists(), "conflict", "Use a fresh immutable run output directory")
    output.mkdir(parents=True, exist_ok=False)
    record = {"schema": "pd.trusted-export.run.v1", "mode": options.mode, "runId": uuid.uuid4().hex,
              "startedUtc": dt.datetime.now(dt.UTC).isoformat(), "status": "NOT_RUN", "scope": options.mode,
              "productionFunctionalPass": False, "actualPdRoundTripPass": False,
              "ownerRoot": str(OWNER), "evidenceRoot": str(output), "cases": []}
    try:
        record["capacityBefore"] = disk_capacity()
        inventory = tools.strict_json((HERE / "required_cases.json").read_bytes())
        env = environment(output)
        if options.mode == "tools":
            record["command"] = log_command([sys.executable, "-B", str(HERE / "test_tools.py")], output, env, 90)
            record["status"] = "PASS" if record["command"]["returnCode"] == 0 else "FAIL"
            record["scope"] = "owned-tool-unit-tests-only"
        elif options.mode == "actual-pd-not-run":
            record.update(tools.aggregate([], inventory["actualPd"] + inventory["receiptNegative"]))
            record["reason"] = "No verified production headless entry and actualPd receipt was handed off; no EXE executed"
        else:
            tools.require(options.exe is not None and isinstance(options.exe_sha256, str)
                          and tools.HEX64.fullmatch(options.exe_sha256) is not None, "missing_service", "An exact delivered EXE and SHA256 are required")
            exe = owned(options.exe, True)
            original = tools.file_snapshot(exe, HERE)
            tools.require(original["sha256"] == options.exe_sha256, "integrity", "Delivered closed EXE hash differs")
            record["deliveredExe"] = original
            if options.mode == "audit-package":
                tools.require(options.package is not None and options.notices is not None, "missing_service", "Actual package and compiled legal inventory are required")
                package = owned(options.package, True)
                notices = tools.strict_json(owned(options.notices, True).read_bytes(), 1024 * 1024)
                record["audit"] = tools.audit_package(package, exe, options.compiled_arch, notices, HERE)
                record["status"] = "PASS"
                record["scope"] = "independent-existing-package-audit-only"
            else:
                tools.require(options.graph is not None and options.graph_sha256 is not None and options.source_commit is not None
                              and options.source_sha256 is not None, "missing_service", "Builder same-graph binding is required")
                tools.require(tools.HEX64.fullmatch(options.graph_sha256) is not None and tools.HEX40.fullmatch(options.source_commit) is not None
                              and tools.HEX64.fullmatch(options.source_sha256) is not None, "invalid_schema", "Full graph/source digests are required")
                graph = owned(options.graph, True)
                graph_bytes = graph.read_bytes()
                tools.require(tools.sha256_bytes(graph_bytes) == options.graph_sha256, "integrity", "Graph receipt differs from builder handoff")
                binding = tools.strict_json(graph_bytes, 4 * tools.MIB)
                tools.require(binding.get("schema") == "pd.trusted-export.graph.v1" and binding.get("appServicesImported") is False
                              and binding.get("sourceCommit") == options.source_commit and binding.get("sourceSha256") == options.source_sha256,
                              "invalid_evidence", "Graph does not bind the real source AppServices")
                tools.require(binding.get("appServicesTarget") == "nickel_app_services" and bool(binding.get("appServicesSources")),
                              "invalid_evidence", "Graph omitted AppServices production sources")
                record["graph"] = {"path": str(graph), "sha256": options.graph_sha256, "binding": binding}
                command = [str(exe), "--output", str(output / "cases.json"), "--fixture-root", str(output / "fixtures"),
                           "--graph-sha256", options.graph_sha256]
                record["command"] = log_command(command, output, env, 180)
                tools.unchanged(original, exe, HERE)
                case_path = output / "cases.json"
                if case_path.exists():
                    cases = tools.strict_json(case_path.read_bytes(), 4 * tools.MIB)
                    tools.require(cases.get("schema") == "pd.trusted-export.cases.v1" and cases.get("sourceCommit") == options.source_commit
                                  and cases.get("sourceSha256") == options.source_sha256 and cases.get("graphSha256") == options.graph_sha256,
                                  "invalid_evidence", "Closed EXE reports different compiled graph/source identity")
                    record.update(tools.aggregate(cases.get("cases", []), inventory["productionNegative"] + inventory["receiptNegative"]))
                    record["caseOutputSha256"] = tools.sha256_bytes(case_path.read_bytes())
                else:
                    record.update(tools.aggregate([], inventory["productionNegative"] + inventory["receiptNegative"]))
                if record["command"]["returnCode"] != 0:
                    record["status"] = "FAIL"
        record["capacityAfter"] = disk_capacity()
    except Exception as error:
        record["status"] = "FAIL"
        record["failure"] = {"code": getattr(error, "code", type(error).__name__), "detail": str(error)}
    record["finishedUtc"] = dt.datetime.now(dt.UTC).isoformat()
    tools.write_new_json(output / "result.json", record)
    print({"status": record["status"], "scope": record["scope"], "result": str(output / "result.json"),
           "productionFunctionalPass": False, "actualPdRoundTripPass": False})
    return 1 if record["status"] == "FAIL" else 77 if record["status"] == "NOT_RUN" else 0


if __name__ == "__main__":
    raise SystemExit(main())
