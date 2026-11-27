#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Read a genuine Git base-to-candidate delta, never substitute an owner inventory."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
REVISION = re.compile(r"^[0-9a-f]{40}(?:[0-9a-f]{24})?$")
RAW = re.compile(rb"^:([0-7]{6}) ([0-7]{6}) ([0-9a-f]+) ([0-9a-f]+) ([ACDMRTUXB][0-9]*)$")


def git(*args: str) -> bytes:
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, check=False)
    if result.returncode:
        raise ValueError(result.stderr.decode("utf-8", errors="replace").strip())
    return result.stdout


def actual_commit(value: str) -> str:
    if not isinstance(value, str) or not REVISION.fullmatch(value):
        raise ValueError("Supply a full actual commit SHA, not HEAD, a branch, an inventory or a guessed revision")
    resolved = git("rev-parse", "--verify", value + "^{commit}").decode("ascii").strip()
    if resolved != value:
        raise ValueError("Candidate/base must be the exact recorded commit SHA")
    return resolved


def parse_raw(raw: bytes) -> tuple[list[dict], list[str]]:
    if not raw:
        raise ValueError("Actual Git owner delta is empty")
    if not raw.endswith(b"\0"):
        raise ValueError("Truncated NUL-delimited Git raw diff")
    tokens = raw[:-1].split(b"\0")
    changes, paths = [], set()
    cursor = 0
    while cursor < len(tokens):
        header = RAW.fullmatch(tokens[cursor])
        if not header:
            raise ValueError("Invalid Git raw diff record")
        old_mode, new_mode, old_object, new_object, status = [part.decode("ascii") for part in header.groups()]
        count = 2 if status[0] in {"R", "C"} else 1
        cursor += 1
        if cursor + count > len(tokens):
            raise ValueError("Missing path or rename/copy endpoint in Git diff")
        record_paths = [part.decode("utf-8", errors="strict") for part in tokens[cursor:cursor + count]]
        if any(not path for path in record_paths):
            raise ValueError("Empty Git changed path")
        changes.append({"status": status, "oldMode": old_mode, "newMode": new_mode,
                        "oldObject": old_object, "newObject": new_object, "paths": record_paths})
        paths.update(record_paths)
        cursor += count
    return changes, sorted(paths)


def delta(base: str, candidate: str) -> dict:
    base, candidate = actual_commit(base), actual_commit(candidate)
    if base == candidate:
        raise ValueError("Actual base and candidate commits must differ")
    args = ["diff-tree", "--raw", "-z", "--no-commit-id", "--no-abbrev", "-r", "-M", "-C", base, candidate, "--"]
    raw = git(*args)
    changes, paths = parse_raw(raw)
    return {"schemaVersion": 1, "kind": "git-owner-diff", "owner": "brand-release", "base": base, "candidate": candidate,
            "baseTree": git("rev-parse", base + "^{tree}").decode("ascii").strip(),
            "candidateTree": git("rev-parse", candidate + "^{tree}").decode("ascii").strip(),
            "gitArgv": ["git", *args], "rawSha256": hashlib.sha256(raw).hexdigest(),
            "paths": paths, "changes": changes,
            "notes": ["Complete genuine commit-to-commit Git delta; not filtered by an owner allowlist.",
                      "Deletions and both rename/copy endpoints are included; no branch, HEAD or index is changed.",
                      "An integrated multi-owner commit needs a genuine isolated owner candidate, not post-hoc path filtering."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="Full actual base commit SHA")
    parser.add_argument("--candidate", required=True, help="Full actual isolated brand candidate commit SHA")
    parser.add_argument("--output", default=".snow/brand-evidence/git-diff.json")
    args = parser.parse_args()
    try:
        output = (ROOT / args.output).resolve()
        if not output.is_relative_to(ROOT) or output.exists():
            raise ValueError("Use an unused exact output inside this checkout")
        document = delta(args.base, args.candidate)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes((json.dumps(document, indent=2, ensure_ascii=True) + "\n").encode("utf-8"))
        print(json.dumps({"output": output.relative_to(ROOT).as_posix(), "base": document["base"],
                          "candidate": document["candidate"], "changedPaths": len(document["paths"]), "nativeReleaseReady": False}))
        return 0
    except (OSError, UnicodeError, ValueError) as error:
        print(json.dumps({"passed": False, "nativeReleaseReady": False, "errors": [str(error)]}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
