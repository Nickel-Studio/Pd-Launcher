"""Owner-local, fail-closed evidence tools. Tool success is never product success."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Any

OWNER = Path(__file__).resolve().parents[2]
HASH = re.compile(r"[0-9a-f]{64}\Z")
CASE_ID = re.compile(r"[a-z][a-z0-9-]{0,79}\Z")
MAX_JSON = 8 * 1024 * 1024
MAX_OUTPUT = 4 * 1024 * 1024
REPARSE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class EvidenceError(ValueError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def relative_path(value: Any) -> str:
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise EvidenceError("path is not canonical relative POSIX text")
    parsed = PurePosixPath(value)
    if parsed.is_absolute() or any(part in {"", ".", ".."} for part in value.split("/")):
        raise EvidenceError("path contains an absolute or dot component")
    if str(parsed) != value or any(ord(ch) < 32 for ch in value):
        raise EvidenceError("path is not canonical")
    return value


def _inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def checked_local(value: str | Path, *, writable: bool = False) -> Path:
    raw = Path(value)
    if not raw.is_absolute():
        raw = OWNER / raw
    raw = Path(os.path.abspath(raw))
    if not _inside(raw, OWNER):
        raise EvidenceError("path is outside the owner worktree")
    if writable:
        allowed = (OWNER / "tests/native-production-smoke", OWNER / ".snow/production-smoke")
        if not any(_inside(raw, scope) for scope in allowed):
            raise EvidenceError("write path is outside exclusive owner scopes")
    current = raw
    while current != OWNER:
        if current.exists() or current.is_symlink():
            info = current.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & REPARSE:
                raise EvidenceError("symbolic link or reparse point in path")
        current = current.parent
    if raw.resolve(strict=False) != raw:
        raise EvidenceError("path resolves through a link")
    return raw


def capacity(path: str | Path, required_bytes: int = 0, reserve_bytes: int = 64 * 1024 * 1024) -> dict:
    target = checked_local(path, writable=True)
    parent = target
    while not parent.exists():
        parent = parent.parent
    usage = shutil.disk_usage(parent)
    if required_bytes < 0 or reserve_bytes < 0:
        raise EvidenceError("negative capacity requirement")
    facts = {"observedAt": utc_now(), "path": str(target), "freeBytes": usage.free,
             "requiredBytes": required_bytes, "reserveBytes": reserve_bytes}
    if usage.free < required_bytes + reserve_bytes:
        raise EvidenceError("insufficient owner-local disk capacity: " + json.dumps(facts))
    return facts


def sha256(path: str | Path) -> str:
    source = checked_local(path)
    before = source.stat()
    if not stat.S_ISREG(before.st_mode):
        raise EvidenceError("hash input is not a regular file")
    digest = hashlib.sha256()
    with source.open("rb") as stream:
        while chunk := stream.read(256 * 1024):
            digest.update(chunk)
    after = source.stat()
    identity = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
    if identity(before) != identity(after):
        raise EvidenceError("hash input changed while being read")
    return digest.hexdigest()


def require_hash(value: Any, description: str) -> str:
    if not isinstance(value, str) or not HASH.fullmatch(value):
        raise EvidenceError(description + " is not lowercase SHA256")
    return value


def _unique_object(pairs: list[tuple[str, Any]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError("duplicate JSON object key: " + key)
        result[key] = value
    return result


def read_json(path: str | Path) -> Any:
    source = checked_local(path)
    if source.stat().st_size > MAX_JSON:
        raise EvidenceError("JSON input exceeds evidence quota")
    try:
        return json.loads(source.read_text(encoding="utf-8-sig"), object_pairs_hook=_unique_object)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError("invalid evidence JSON") from error


def write_json_new(path: str | Path, document: Any) -> Path:
    target = checked_local(path, writable=True)
    payload = (json.dumps(document, ensure_ascii=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    capacity(target, len(payload))
    target.parent.mkdir(parents=True, exist_ok=True)
    checked_local(target, writable=True)
    with target.open("xb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    return target


def manifest_records(document: dict) -> list[dict]:
    entries = document.get("files")
    if isinstance(entries, dict):
        rows = []
        for path, info in entries.items():
            if isinstance(info, str):
                rows.append({"path": path, "sha256": info})
            elif isinstance(info, dict) and "path" not in info:
                rows.append({"path": path, **info})
            else:
                raise EvidenceError("ambiguous manifest record")
        return rows
    if isinstance(entries, list) and all(isinstance(row, dict) for row in entries):
        return entries
    raise EvidenceError("manifest must contain a files array or map")


def verify_freeze(root: str | Path, manifest: str | Path, marker: str | Path,
                  expected_sha256: str, required: list[str] | None = None) -> dict:
    base = checked_local(root)
    manifest_path = checked_local(manifest)
    marker_path = checked_local(marker)
    if not _inside(manifest_path, base) or not _inside(marker_path, base):
        raise EvidenceError("manifest/complete marker must belong to the frozen namespace")
    pinned = require_hash(expected_sha256, "manifest pin")
    if sha256(manifest_path) != pinned:
        raise EvidenceError("exact manifest bytes do not match the explicit pin")
    complete = read_json(marker_path)
    if not isinstance(complete, dict):
        raise EvidenceError("publication marker is not an object")
    bounded = complete.get("kind") == "bounded-source-publication-complete"
    if bounded:
        if complete.get("publicationComplete") is not True or complete.get("receiver") != "production-smoke":
            raise EvidenceError("bounded publication incomplete or belongs to another receiver")
        marker_pin = complete.get("receiverManifestSha256")
        finished = complete.get("finishedUtc")
    else:
        if complete.get("state") != "complete":
            raise EvidenceError("publication is not marked complete")
        marker_pin = complete.get("manifestSha256")
        finished = complete.get("completedAt")
    if marker_pin != pinned:
        raise EvidenceError("publication marker does not bind exact manifest")
    if not isinstance(finished, str) or not finished:
        raise EvidenceError("publication completion time is missing")
    try:
        parsed_time = datetime.fromisoformat(finished.replace("Z", "+00:00"))
        if parsed_time.tzinfo is None:
            raise ValueError("missing timezone")
    except ValueError as error:
        raise EvidenceError("invalid publication completion time") from error
    document = read_json(manifest_path)
    if not isinstance(document, dict):
        raise EvidenceError("manifest is not an object")
    rows = manifest_records(document)
    if not rows or len(rows) > 10000:
        raise EvidenceError("empty or over-quota frozen manifest")
    seen: set[str] = set()
    verified = []
    total = 0
    for row in rows:
        rel = relative_path(row.get("path"))
        folded = rel.casefold()
        if folded in seen:
            raise EvidenceError("duplicate/case-colliding frozen input: " + rel)
        seen.add(folded)
        expected = require_hash(row.get("sha256"), "file hash")
        path = checked_local(base / rel)
        if not _inside(path, base):
            raise EvidenceError("frozen input escaped namespace")
        size = path.stat().st_size
        declared_size = row.get("size", row.get("bytes"))
        if declared_size is not None and (type(declared_size) is not int or declared_size != size):
            raise EvidenceError("frozen file size mismatch: " + rel)
        actual = sha256(path)
        if actual != expected:
            raise EvidenceError("frozen file SHA256 mismatch: " + rel)
        total += size
        verified.append({"path": rel, "size": size, "sha256": actual, "owner": row.get("owner")})
    names = {row["path"] for row in verified}
    for rel in required or []:
        if relative_path(rel) not in names:
            raise EvidenceError("required production input not in manifest: " + rel)
    if bounded and (type(complete.get("sourceFiles")) is not int or complete["sourceFiles"] != len(verified)
                    or type(complete.get("sourceBytes")) is not int or complete["sourceBytes"] != total):
        raise EvidenceError("bounded completion file/byte counts do not match actual inputs")
    provenance = document.get("captureProvenance", {})
    if not isinstance(provenance, dict):
        raise EvidenceError("invalid capture provenance")
    if sha256(manifest_path) != pinned:
        raise EvidenceError("manifest changed during verification")
    return {"schema": 1, "kind": "verified-frozen-inputs", "verifiedAt": utc_now(),
            "root": str(base), "manifest": str(manifest_path), "manifestSha256": pinned,
            "completeMarker": str(marker_path), "completeMarkerSha256": sha256(marker_path),
            "completedAt": finished, "fileCount": len(verified), "totalBytes": total,
            "files": verified, "sourceSetScope": document.get("sourceSetScope"),
            "classification": complete.get("classification", provenance.get("classification")),
            "productionBinaryAvailable": complete.get("productionBinaryAvailable", False),
            "matchingDomainLibrariesAvailable": provenance.get("matchingDomainLibrariesAvailable", False),
            "actualCTestInventoryAvailable": provenance.get("actualCTestInventoryAvailable", False),
            "productionPass": False}


def enum_members(text: str, name: str) -> list[str]:
    # Inventory only accepts simple public enum declarations, never evaluates C++.
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    pattern = r"\benum\s+class\s+" + re.escape(name) + r"\s*(?::\s*\w+\s*)?\{([^{}]*)\}\s*;"
    matches = re.findall(pattern, text)
    if len(matches) != 1:
        raise EvidenceError("cannot unambiguously inventory enum " + name)
    entries = [entry.strip() for entry in matches[0].split(",") if entry.strip()]
    result = []
    for entry in entries:
        match = re.fullmatch(r"([A-Za-z_]\w*)(?:\s*=\s*(?:0x[0-9A-Fa-f]+|[0-9]+))?", entry)
        if not match:
            raise EvidenceError("unsupported enum declaration: " + entry)
        result.append(match.group(1))
    if not result or len(set(result)) != len(result):
        raise EvidenceError("empty/duplicate enum inventory")
    return result


def load_matrix(path: str | Path) -> dict:
    matrix = read_json(path)
    if not isinstance(matrix, dict) or matrix.get("schema") != 1:
        raise EvidenceError("unsupported action matrix")
    cases = matrix.get("cases")
    if not isinstance(cases, list) or not cases:
        raise EvidenceError("empty action matrix")
    seen = set()
    for case in cases:
        if not isinstance(case, dict) or not isinstance(case.get("id"), str) or not CASE_ID.fullmatch(case["id"]):
            raise EvidenceError("invalid case id")
        if case["id"] in seen:
            raise EvidenceError("duplicate matrix case")
        seen.add(case["id"])
        for field in ("queries", "actions", "checks", "requirement_ids"):
            values = case.get(field)
            if not isinstance(values, list) or not all(isinstance(value, str) and value for value in values):
                raise EvidenceError("invalid matrix field: " + field)
        if not case["checks"] or not case["requirement_ids"]:
            raise EvidenceError("case has no checks/requirements")
    return matrix


def inventory(header: str | Path, matrix_path: str | Path) -> dict:
    source = checked_local(header)
    matrix = load_matrix(matrix_path)
    text = source.read_text(encoding="utf-8-sig")
    result = {"kind": "action-inventory", "headerSha256": sha256(source),
              "matrixSha256": sha256(matrix_path), "productionPass": False}
    for enum_name, field in (("UiQuery", "queries"), ("UiAction", "actions")):
        actual = enum_members(text, enum_name)
        declared = {value for case in matrix["cases"] for value in case[field]}
        missing = sorted(set(actual) - declared)
        obsolete = sorted(declared - set(actual))
        result[enum_name] = {"members": actual, "count": len(actual), "uncovered": missing, "obsolete": obsolete}
        if missing or obsolete:
            raise EvidenceError("matrix/" + enum_name + " mismatch: " + json.dumps(result[enum_name]))
    return result


def ctest_inventory(document: Any, required_names: list[str]) -> list[str]:
    if not isinstance(document, dict) or document.get("kind") != "ctestInfo":
        raise EvidenceError("not actual ctest --show-only=json-v1 output")
    tests = document.get("tests")
    if not isinstance(tests, list) or not tests:
        raise EvidenceError("empty CTest inventory")
    names = []
    for test in tests:
        if not isinstance(test, dict) or not isinstance(test.get("name"), str):
            raise EvidenceError("invalid CTest test")
        command = test.get("command")
        if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
            raise EvidenceError("CTest test has no bound command")
        names.append(test["name"])
    if len(set(names)) != len(names):
        raise EvidenceError("duplicate CTest tests")
    if not required_names or not set(required_names).issubset(names):
        raise EvidenceError("production tests are absent from CTest inventory")
    if all(any(word in name.casefold() for word in ("tools", "selftest", "generate")) for name in required_names):
        raise EvidenceError("tool-only CTest inventory cannot satisfy production gate")
    return names


def run_command(argv: list[str], cwd: str | Path, output: str | Path,
                *, timeout: float = 120, executable_sha256: str | None = None) -> dict:
    if not argv or not all(isinstance(arg, str) and arg and "\x00" not in arg for arg in argv):
        raise EvidenceError("empty or invalid command argv")
    if timeout <= 0 or timeout > 1800:
        raise EvidenceError("invalid command timeout")
    directory = checked_local(cwd)
    target = checked_local(output, writable=True)
    if target.exists():
        raise EvidenceError("evidence output already exists")
    disk = capacity(target, MAX_OUTPUT * 2)
    target.mkdir(parents=True, exist_ok=False)
    executable = Path(argv[0])
    if not executable.is_absolute():
        local = directory / executable
        executable = local if local.is_file() else Path(shutil.which(argv[0]) or argv[0])
    executable = executable.resolve(strict=True)
    # Installed tools may be outside the owner tree; their bytes are read only.
    exe_digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    if executable_sha256 is not None and exe_digest != require_hash(executable_sha256, "executable pin"):
        raise EvidenceError("command executable SHA256 mismatch")
    started = utc_now()
    monotonic = time.monotonic()
    stdout_path, stderr_path = target / "stdout.txt", target / "stderr.txt"
    timed_out = False
    with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
        process = subprocess.Popen(argv, cwd=directory, stdout=stdout, stderr=stderr, shell=False)
        pid = process.pid
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            # Popen owns this exact child handle. Never kill processes by name.
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            timed_out, code = True, 124
    if stdout_path.stat().st_size > MAX_OUTPUT or stderr_path.stat().st_size > MAX_OUTPUT:
        raise EvidenceError("command output exceeded evidence quota")
    facts = {"schema": 1, "kind": "command-evidence", "argv": argv,
             "cwd": str(directory), "startedAt": started, "finishedAt": utc_now(),
             "elapsedSeconds": round(time.monotonic() - monotonic, 6), "pid": pid,
             "pidBirthBoundBy": "owned-Popen-handle", "executable": str(executable),
             "executableSha256": exe_digest, "exitCode": code, "timedOut": timed_out,
             "capacity": disk, "stdout": str(stdout_path), "stdoutSha256": sha256(stdout_path),
             "stderr": str(stderr_path), "stderrSha256": sha256(stderr_path), "productionPass": False}
    write_json_new(target / "command.json", facts)
    return facts


def production_gate(freeze: dict, command: dict, cases: dict, matrix: dict,
                    ctest: dict, required_tests: list[str]) -> dict:
    if freeze.get("kind") != "verified-frozen-inputs" or not freeze.get("fileCount"):
        raise EvidenceError("verified freeze is missing")
    manifest_pin = require_hash(freeze.get("manifestSha256"), "freeze hash")
    if command.get("kind") != "command-evidence" or command.get("exitCode") != 0 or command.get("timedOut") is not False:
        raise EvidenceError("production binary did not exit successfully")
    binary_pin = require_hash(command.get("executableSha256"), "binary hash")
    if cases.get("kind") != "production-appservices-fixture" or cases.get("schema") != 1:
        raise EvidenceError("case results are not production AppServices fixture evidence")
    if cases.get("manifestSha256") != manifest_pin or cases.get("binarySha256") != binary_pin:
        raise EvidenceError("case results do not bind source and binary")
    if cases.get("realAppServices") is not True or cases.get("realQObject") is not True:
        raise EvidenceError("real AppServices/QObject provenance is missing")
    names = ctest_inventory(ctest, required_tests)
    results = cases.get("cases")
    if not isinstance(results, list):
        raise EvidenceError("case results are missing")
    expected = {case["id"]: case for case in matrix["cases"]}
    seen = set()
    for row in results:
        if not isinstance(row, dict) or row.get("id") not in expected or row["id"] in seen:
            raise EvidenceError("unexpected/duplicate case result")
        seen.add(row["id"])
        if row.get("verdict") != "PASS":
            raise EvidenceError("case did not pass: " + row["id"])
        checks = row.get("checks")
        if not isinstance(checks, dict) or set(checks) != set(expected[row["id"]]["checks"]):
            raise EvidenceError("case checks are incomplete: " + row["id"])
        if not all(value is True for value in checks.values()):
            raise EvidenceError("case assertion is not true: " + row["id"])
    if seen != set(expected):
        raise EvidenceError("matrix cases were not all executed")
    return {"schema": 1, "kind": "production-appservices-verdict", "verdict": "PASS",
            "manifestSha256": manifest_pin, "binarySha256": binary_pin,
            "exitCode": 0, "ctestTests": names, "caseCount": len(seen),
            "scope": "independent-AppServices-fixture-only", "completeProductPass": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--root", required=True)
    verify.add_argument("--manifest", required=True)
    verify.add_argument("--marker", required=True)
    verify.add_argument("--sha256", required=True)
    verify.add_argument("--required", action="append", default=[])
    verify.add_argument("--output", required=True)
    inv = commands.add_parser("inventory")
    inv.add_argument("--header", required=True)
    inv.add_argument("--matrix", default="tests/native-production-smoke/matrix.json")
    inv.add_argument("--output", required=True)
    runner = commands.add_parser("command")
    runner.add_argument("--cwd", default=".")
    runner.add_argument("--output", required=True)
    runner.add_argument("--timeout", type=float, default=120)
    runner.add_argument("--exe-sha256")
    runner.add_argument("argv", nargs=argparse.REMAINDER)
    gate = commands.add_parser("gate")
    for field in ("freeze", "run", "cases", "ctest", "output"):
        gate.add_argument("--" + field, required=True)
    gate.add_argument("--matrix", default="tests/native-production-smoke/matrix.json")
    gate.add_argument("--test", action="append", required=True)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            result = verify_freeze(args.root, args.manifest, args.marker, args.sha256, args.required)
            write_json_new(args.output, result)
        elif args.command == "inventory":
            result = inventory(args.header, args.matrix)
            write_json_new(args.output, result)
        elif args.command == "gate":
            result = production_gate(read_json(args.freeze), read_json(args.run), read_json(args.cases),
                                     load_matrix(args.matrix), read_json(args.ctest), args.test)
            write_json_new(args.output, result)
        else:
            argv = args.argv[1:] if args.argv[:1] == ["--"] else args.argv
            result = run_command(argv, args.cwd, args.output, timeout=args.timeout,
                                 executable_sha256=args.exe_sha256)
        print(json.dumps(result, ensure_ascii=True, indent=2))
        return result.get("exitCode", 0)
    except (EvidenceError, OSError) as error:
        print(json.dumps({"kind": "evidence-error", "verdict": "BLOCKED", "error": str(error),
                          "productionPass": False}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
