#!/usr/bin/env python3
"""Read-only verification of a real owner mirror, not product acceptance."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys

from audit_support import AuditSafetyError, atomic_json, plain_path, sha256

HASH = re.compile(r"[0-9a-f]{64}\Z")


def records_hash(records: dict) -> str:
    raw = json.dumps(records, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    return hashlib.sha256(raw).hexdigest()


def relative_key(key: str) -> PurePosixPath:
    if not isinstance(key, str) or not key or "\\" in key or ":" in key:
        raise AuditSafetyError("Snapshot key is not a plain relative POSIX path")
    path = PurePosixPath(key)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in key.split("/")):
        raise AuditSafetyError("Snapshot key contains traversal or an empty component: " + key)
    return path


def verify(manifest: Path, expected_sha256: str | None = None) -> dict:
    manifest = plain_path(manifest, must_exist=True)
    if not manifest.is_file() or manifest.stat().st_size > 8 * 1024 * 1024:
        raise AuditSafetyError("Snapshot manifest must be a bounded regular file")
    raw = manifest.read_bytes()
    manifest_hash = hashlib.sha256(raw).hexdigest()
    if expected_sha256 is not None and expected_sha256 != manifest_hash:
        raise AuditSafetyError("Snapshot manifest is not the explicitly frozen manifest")
    payload = json.loads(raw)
    if not isinstance(payload, dict) or payload.get("schemaVersion") != 1 or payload.get("kind") != "ignored-owner-dependency-snapshot":
        raise AuditSafetyError("Unrecognized real owner snapshot schema")
    records = payload.get("files")
    if not isinstance(records, dict) or not 1 <= len(records) <= 50000:
        raise AuditSafetyError("Snapshot has no bounded real file set")
    source_set = payload.get("sourceSetSha256")
    partial = source_set is None
    errors = []
    if partial:
        scopes, owners = payload.get("refreshedScopes"), payload.get("refreshedOwners")
        frozen = payload.get("frozenSource")
        if expected_sha256 is None or not isinstance(scopes, list) or not 1 <= len(scopes) <= 1000:
            raise AuditSafetyError("A precision refresh requires an explicitly frozen manifest and bounded scopes")
        for scope in scopes:
            relative_key(scope)
        if not isinstance(owners, list) or not owners or any(not isinstance(owner, str) or not owner for owner in owners):
            raise AuditSafetyError("Precision refresh lacks its selected owners")
        if not isinstance(frozen, dict) or not isinstance(frozen.get("manifest"), str) or not frozen["manifest"]:
            raise AuditSafetyError("Precision refresh lacks its frozen source reference")
        for value in (payload.get("refreshSetSha256"), payload.get("previousManifestSha256"), frozen.get("manifestSha256")):
            if not isinstance(value, str) or not HASH.fullmatch(value):
                raise AuditSafetyError("Precision refresh lacks a valid source or previous-manifest identity")
        selected = {key: record for key, record in records.items()
                    if any(key == scope or key.startswith(scope + "/") for scope in scopes)}
        if not selected or any(not isinstance(record, dict) or record.get("owner") not in owners for record in selected.values()):
            raise AuditSafetyError("Precision refresh selected records do not match their declared owners")
        if records_hash(selected) != payload["refreshSetSha256"]:
            errors.append({"kind": "refresh-set-hash-mismatch"})
    elif not isinstance(source_set, str) or not HASH.fullmatch(source_set):
        raise AuditSafetyError("Snapshot lacks its complete-owner source-set identity")
    if records_hash(records) != payload.get("snapshotSha256"):
        errors.append({"kind": "record-set-hash-mismatch"})
    mirror = manifest.parent
    checked = []
    for key, record in sorted(records.items()):
        path = mirror / relative_key(key)
        if (not isinstance(record, dict) or not isinstance(record.get("owner"), str)
                or not record["owner"] or not isinstance(record.get("sha256"), str)
                or not HASH.fullmatch(record["sha256"])):
            raise AuditSafetyError("Malformed owner/hash record: " + key)
        try:
            path = plain_path(path, root=mirror, must_exist=True)
            if not path.is_file():
                raise AuditSafetyError("Snapshot member is not a regular file")
            actual = sha256(path)
            if actual != record["sha256"]:
                errors.append({"kind": "file-hash-mismatch", "path": key,
                               "expected": record["sha256"], "actual": actual})
            checked.append({"path": key, "sha256": actual, "owner": record["owner"]})
        except (OSError, AuditSafetyError) as error:
            errors.append({"kind": "missing-or-unsafe-member", "path": key, "error": str(error)})
    native = mirror / "native"
    if native.is_dir():
        for path in native.rglob("*"):
            if path.is_file() and path.relative_to(mirror).as_posix() not in records:
                errors.append({"kind": "unlisted-native-file", "path": path.relative_to(mirror).as_posix()})
    manifest_unchanged = manifest.read_bytes() == raw
    if not manifest_unchanged:
        errors.append({"kind": "manifest-changed-during-verification"})
    return {"schema": 1, "kind": "read-only-owner-snapshot-verification",
            "manifestPath": str(manifest), "manifestSha256": manifest_hash,
            "snapshotSha256": payload.get("snapshotSha256"), "sourceSetSha256": source_set,
            "precisionRefresh": partial, "refreshSetSha256": payload.get("refreshSetSha256"),
            "previousManifestSha256": payload.get("previousManifestSha256"), "frozenSource": payload.get("frozenSource"),
            "snapshotCreatedUnix": payload.get("createdUnix"), "fileCount": len(records),
            "checkedFiles": checked, "manifestUnchanged": manifest_unchanged,
            "errors": errors, "verified": not errors, "completeProductVerified": False,
            "scope": "Exact mirror file hashes only; no compilation/runtime/release claim. "
                     "A precision refresh retains sourceSetSha256=null and verifies only its declared refresh set; "
                     "a complete sourceSetSha256 remains owner-supplied because the mirror excludes this member's files."}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--expected-sha256")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = verify(args.manifest, args.expected_sha256)
        if args.output.exists():
            raise AuditSafetyError("Refusing to replace an existing verification report")
        atomic_json(args.output, result)
        print(json.dumps({"manifestSha256": result["manifestSha256"], "files": result["fileCount"],
                          "verified": result["verified"], "errors": result["errors"]}, indent=2))
        return 0 if result["verified"] else 1
    except (OSError, ValueError, AuditSafetyError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
