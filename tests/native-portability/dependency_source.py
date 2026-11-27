#!/usr/bin/env python3
"""Pin completed immutable dependency recipes, never a complete-product verdict."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from audit_support import AuditSafetyError, OWNED, plain_path, sha256
from verify_snapshot import HASH, relative_key, verify

MIRROR = OWNED.parents[1] / ".snow/native-shared"
RECIPE_INPUTS = (
    "scripts/native/deps-lock.json",
    "scripts/native/bootstrap-deps.py",
    "scripts/native/deps/bzip2/CMakeLists.txt",
    "scripts/native/deps/gumbo/CMakeLists.txt",
    "scripts/native/deps/gumbo/gumbo-config.cmake",
    "scripts/native/deps/yaml/CMakeLists.txt",
    "scripts/native/deps/yaml/yamlConfig.cmake",
    "cmake/NickelDependencies.cmake",
)


class SourceFreeze:
    def __init__(self, manifest: Path, expected_sha256: str):
        if not isinstance(expected_sha256, str) or not HASH.fullmatch(expected_sha256):
            raise AuditSafetyError("An explicit complete-source manifest SHA-256 is required")
        self.manifest = plain_path(manifest, root=MIRROR, must_exist=True)
        self.root = self.manifest.parent
        self.expected_sha256 = expected_sha256
        checked = verify(self.manifest, expected_sha256)
        if not checked["verified"] or checked["precisionRefresh"]:
            raise AuditSafetyError("Dependency recipes require a verified complete source publication")
        self.inputs = []
        documents = {}
        for name in (self.manifest.name, "publication-complete.json", "reader-lock.json", "changed-paths.json"):
            path = plain_path(self.root / name, root=self.root, must_exist=True)
            if not path.is_file() or path.stat().st_size > 8 * 1024 * 1024:
                raise AuditSafetyError("Publication metadata must be a bounded regular file")
            raw = path.read_bytes()
            documents[name] = json.loads(raw)
            if not isinstance(documents[name], dict):
                raise AuditSafetyError("Publication metadata must be an object")
            self.inputs.append({"path": name, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
        self.payload = documents[self.manifest.name]
        receipt, lease = documents["publication-complete.json"], documents["reader-lock.json"]
        frozen = self.payload.get("frozenSource")
        receiver = OWNED.parents[1].name
        if (self.payload.get("member") != receiver or not isinstance(frozen, dict)
                or not isinstance(frozen.get("manifestSha256"), str) or not HASH.fullmatch(frozen["manifestSha256"])
                or receipt.get("schemaVersion") != 1 or receipt.get("kind") != "bounded-source-publication-complete"
                or receipt.get("publicationComplete") is not True or receipt.get("receiver") != receiver
                or receipt.get("receiverManifestSha256") != expected_sha256
                or receipt.get("sourceSetSha256") != checked["sourceSetSha256"]
                or receipt.get("sourceCaptureManifestSha256") != frozen["manifestSha256"]
                or lease.get("schemaVersion") != 1 or lease.get("receiver") != receiver
                or lease.get("upstreamManifestSha256") != frozen["manifestSha256"]
                or not isinstance(lease.get("lease"), str) or not lease["lease"]):
            raise AuditSafetyError("Publication receipt/reader lease does not identify the complete freeze")
        source_bytes = 0
        for name, record in self.payload["files"].items():
            path = plain_path(self.root / relative_key(name), root=self.root, must_exist=True)
            if type(record.get("bytes")) is not int or record["bytes"] < 0 or path.stat().st_size != record["bytes"]:
                raise AuditSafetyError("Frozen source member size differs: " + name)
            source_bytes += record["bytes"]
        if receipt.get("sourceFiles") != checked["fileCount"] or receipt.get("sourceBytes") != source_bytes:
            raise AuditSafetyError("Source publication receipt count/size differs")
        oracle = self.payload.get("oracleFiles")
        if not isinstance(oracle, dict) or len(oracle) > 1000:
            raise AuditSafetyError("Publication oracle set is not bounded")
        oracle_bytes = 0
        for name, record in oracle.items():
            path = plain_path(self.root / relative_key(name), root=self.root, must_exist=True)
            if (not isinstance(record, dict) or type(record.get("bytes")) is not int or record["bytes"] < 0
                    or not path.is_file() or path.stat().st_size != record["bytes"] or sha256(path) != record.get("sha256")):
                raise AuditSafetyError("Frozen oracle differs: " + name)
            oracle_bytes += record["bytes"]
        if receipt.get("oracleFiles") != len(oracle) or receipt.get("oracleBytes") != oracle_bytes:
            raise AuditSafetyError("Source publication oracle count/size differs")
        changes = next(row for row in self.inputs if row["path"] == "changed-paths.json")
        if receipt.get("changedPathsSha256") != changes["sha256"]:
            raise AuditSafetyError("Publication changed-path identity differs")
        for name in RECIPE_INPUTS:
            record = self.payload["files"].get(name)
            if not isinstance(record, dict):
                raise AuditSafetyError("Required dependency recipe is outside the pinned set: " + name)
            self.inputs.append({"path": name, "bytes": record["bytes"], "sha256": record["sha256"]})
        info = self.root.stat()
        self.identity = (info.st_dev, info.st_ino, info.st_uid)
        self.evidence = {"sourceRoot": str(self.root), "rootIdentity": list(self.identity),
                         "manifestPath": str(self.manifest), "manifestSha256": expected_sha256,
                         "sourceSetSha256": checked["sourceSetSha256"], "fileCount": checked["fileCount"],
                         "sourceBytes": source_bytes, "oracleFiles": len(oracle), "oracleBytes": oracle_bytes,
                         "readerLease": lease["lease"], "inputs": self.inputs,
                         "sourceVerifiedBefore": True, "sourceVerifiedAfter": False,
                         "completeProductVerified": False}
        self.check()

    def check(self):
        root = plain_path(self.root, root=MIRROR, must_exist=True)
        info = root.stat()
        if (info.st_dev, info.st_ino, info.st_uid) != self.identity:
            raise AuditSafetyError("Pinned dependency root identity changed")
        for row in self.inputs:
            path = plain_path(root / relative_key(row["path"]), root=root, must_exist=True)
            if not path.is_file() or path.stat().st_size != row["bytes"] or sha256(path) != row["sha256"]:
                raise AuditSafetyError("Pinned dependency manifest/receipt/recipe changed: " + row["path"])

    def postcheck(self):
        self.check()
        checked = verify(self.manifest, self.expected_sha256)
        if not checked["verified"]:
            raise AuditSafetyError("Source changed during the dependency/product build")
        self.evidence["sourceVerifiedAfter"] = True
        return checked
