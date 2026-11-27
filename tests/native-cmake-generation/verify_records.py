#!/usr/bin/env python3
"""Rehash saved real-process evidence; this audit does not replace CMake/Ninja."""

import argparse
import hashlib
import json
from pathlib import Path
import sys
from datetime import datetime, timezone

from run_regression import OWNED, ROOT, digest, owned_size, write_json


RECORDS = OWNED / "records"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def stored_path(relative):
    path = (ROOT / relative).resolve(strict=True)
    require(path.is_relative_to(RECORDS), f"Evidence path escapes the owned records: {relative}")
    return path


def audit_run(result_path):
    data = json.loads(result_path.read_text(encoding="utf-8"))
    commands = data["commands"]
    require(data["workerCount"] == 1, "Worker count must remain one")
    require(data["sourceHashes"] == data["sourceHashesAfter"], "Source inputs changed during an evidence run")
    for command in commands:
        sidecar = result_path.parent / f"{command['label']}.command.json"
        require(json.loads(sidecar.read_text(encoding="utf-8")) == command, f"Command sidecar differs: {sidecar}")
        require(command["timedOut"] is False, f"Timed-out command: {command['label']}")
        require(command["environmentOverrides"]["CMAKE_BUILD_PARALLEL_LEVEL"] == "1", "Parallel override missing")
        require("dupbuild=warn" not in " ".join(command["argv"]), "Duplicate-build warning escape is forbidden")
        for stream in ("stdout", "stderr"):
            path = stored_path(command[stream])
            require(digest(path) == command[f"{stream}Sha256"], f"Raw output hash differs: {path}")
    for case, files in data.get("generated", {}).items():
        for item in files:
            path = result_path.parent / "generated" / case / item["path"]
            require(digest(path) == item["sha256"], f"Saved real generated file hash differs: {path}")
    for block in data.get("consumedBlocks", {}).values():
        origins = [item for item in data["gitPayloads"].values()
                   if item["commit"] == block["originCommit"] and item["path"] == block["originPath"]]
        require(len(origins) == 1, "Expected a unique immutable Git source blob")
        raw = stored_path(origins[0]["record"]).read_bytes()
        require(hashlib.sha256(raw).hexdigest() == block["wholeBlobSha256"], "Original Git blob SHA256 differs")
        fragment = b"".join(raw.splitlines(keepends=True)[block["startLine"] - 1:block["endLine"]])
        require(hashlib.sha256(fragment).hexdigest() == block["blockSha256"], "Extracted block SHA256 differs")
        require(stored_path(block["savedFragment"]).read_bytes() == fragment, "Saved block is not byte-exact original source")
        require(block["modified"] is False, "Owner resource block must not be rewritten")
    if data.get("verification") == "BYTE_EXACT_COMMITTED_PARENT_FAIL_FIXED_CMAKE_NINJA_EXIT0":
        require(data["result"] == "PASS" and all(item["passed"] for item in data["checks"]), "Final committed gates failed")
        codes = {item["label"]: item["returnCode"] for item in commands}
        require(codes["committed-before.configure"] == 0 and codes["committed-before.ninja-parse"] != 0,
                "The committed before case did not reproduce a real Ninja failure")
        for label, code in codes.items():
            if label != "committed-before.ninja-parse":
                require(code == 0, f"Final committed command is not exit0: {label}")
        require(data["resourceNaming"]["rccOrQtLinkExecuted"] is False, "Do not inflate generation evidence to Qt-link evidence")
    return {
        "runId": result_path.parent.name,
        "result": data["result"],
        "resultSha256": digest(result_path),
        "commands": len(commands),
        "checks": len(data["checks"]),
        "failedChecks": sum(not item["passed"] for item in data["checks"]),
        "savedEvidenceVerified": True,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seal", action="store_true", help="Create a new immutable audit-manifest.json; never overwrite")
    args = parser.parse_args()
    try:
        results = sorted(RECORDS.glob("*/result.json"))
        require(bool(results), "No actual run evidence found")
        rows = [audit_run(path) for path in results]
        manifest_path = RECORDS / "audit-manifest.json"
        files = [{"path": path.relative_to(ROOT).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)}
                 for path in sorted(RECORDS.rglob("*")) if path.is_file() and path != manifest_path]
        output = {
            "schemaVersion": 1,
            "kind": "saved-real-process-evidence-integrity-not-new-build",
            "auditArgv": [sys.executable, *sys.argv],
            "cwd": str(ROOT),
            "verifiedUtc": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
            "returnCode": 0,
            "runs": rows,
            "files": files,
            "evidenceBytes": sum(item["bytes"] for item in files),
            "ownedBytesObserved": owned_size(),
        }
        if args.seal:
            require(not manifest_path.exists(), "Audit manifest already exists; refuse to overwrite historical evidence")
            write_json(manifest_path, output)
        elif manifest_path.exists():
            sealed = json.loads(manifest_path.read_text(encoding="utf-8"))
            require(sealed["files"] == files and sealed["runs"] == rows, "Sealed evidence changed or new unsealed evidence appeared")
        print(json.dumps({"savedEvidenceIntegrity": "PASS", "runs": rows,
                          "evidenceBytes": output["evidenceBytes"],
                          "sealed": manifest_path.exists()}, ensure_ascii=True, indent=2))
        return 0
    except Exception as error:
        print(f"Evidence audit failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
