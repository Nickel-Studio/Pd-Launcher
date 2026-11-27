#!/usr/bin/env python3
"""Capture immutable Git build inputs in owned D storage, without relabeling history."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import uuid

from audit_support import AuditSafetyError, OWNED, capacity, owned_output, plain_path, sha256
from dependency_source import RECIPE_INPUTS
from verify_snapshot import HASH, records_hash, relative_key

COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
MAX_BYTES = 64 * 1024 * 1024
PREFIXES = ("native/", "cmake/", "scripts/native/", "tests/native-acceptance/", "tests/native-production-smoke/", "src/assets/")
ROOT_FILES = {"CMakeLists.txt", "CHANGELOG.json", "LICENSE", "LICENSE.md", "NOTICE", "NOTICE.txt"}
REQUIRED = {"CMakeLists.txt", "CHANGELOG.json", "native/metadata.json", "native/app/CMakeLists.txt",
            "native/ui/CMakeLists.txt", "tests/native-acceptance/CMakeLists.txt",
            "tests/native-production-smoke/CMakeLists.txt", "tests/native-production-smoke/production_main.cpp",
            "tests/native-production-smoke/prepare_same_graph.py", *RECIPE_INPUTS}
MARKER = ".git-source-owned.json"
MANIFEST = "git-source-manifest.json"
COMPLETE = "capture-complete.json"


def product_identity(blobs: dict) -> tuple[dict, str]:
    metadata = json.loads(blobs["native/metadata.json"])
    if not isinstance(metadata, dict) or metadata.get("schemaVersion") != 1:
        raise AuditSafetyError("Actual production metadata object required")
    executable = metadata.get("executableName")
    if not isinstance(executable, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", executable):
        raise AuditSafetyError("Plain production executable name required")
    targets = re.findall(r"qt_add_executable\(\s*([A-Za-z0-9_-]+)\s",
                         blobs["native/app/CMakeLists.txt"].decode("utf-8"))
    if len(targets) != 1:
        raise AuditSafetyError("Exactly one actual Qt executable target required")
    return metadata, targets[0]


def git(repository: Path, *arguments: str, data: bytes | None = None) -> bytes:
    environment = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_OPTIONAL_LOCKS="0", GIT_NO_REPLACE_OBJECTS="1")
    command = ["git", "--no-replace-objects"]
    pointer = plain_path(repository / ".git", root=repository, must_exist=True)
    if os.name != "nt" and pointer.is_file():
        text = pointer.read_text(encoding="utf-8").strip()
        if not text.startswith("gitdir: ") or len(text) > 4096:
            raise AuditSafetyError("Bounded Git worktree pointer required")
        location = text[8:]
        match = re.fullmatch(r"([A-Za-z]):[/\\](.+)", location)
        if match:
            location = "/mnt/" + match[1].lower() + "/" + match[2].replace("\\", "/")
        directory = plain_path(Path(location) if Path(location).is_absolute() else repository / location,
                               must_exist=True)
        command.append("--git-dir=" + str(directory))
    result = subprocess.run([*command, *arguments], cwd=repository, input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment, timeout=120)
    if result.returncode != 0:
        raise AuditSafetyError("Immutable Git read failed: " + result.stderr.decode("utf-8", errors="replace"))
    if len(result.stdout) > 2 * MAX_BYTES:
        raise AuditSafetyError("Immutable Git output exceeded the source budget")
    return result.stdout


def commit_inputs(repository: Path, commit: str) -> tuple[str, dict, dict]:
    if not isinstance(commit, str) or not COMMIT.fullmatch(commit):
        raise AuditSafetyError("An explicit full immutable Git commit ID is required")
    repository = plain_path(repository, must_exist=True)
    if git(repository, "cat-file", "-t", commit).strip() != b"commit":
        raise AuditSafetyError("The pinned Git object is not a commit")
    tree = git(repository, "rev-parse", commit + "^{tree}").decode("ascii").strip()
    entries, folded = {}, set()
    for item in git(repository, "ls-tree", "-r", "-l", "-z", commit).split(b"\0"):
        if not item:
            continue
        header, raw_path = item.split(b"\t", 1)
        mode, kind, blob, size = header.decode("ascii").split()
        key = raw_path.decode("utf-8")
        if (key not in ROOT_FILES and not key.startswith(PREFIXES)) or key.startswith("native/tests/minecraft/.build-policy/"):
            continue
        relative_key(key)
        if mode not in {"100644", "100755"} or kind != "blob" or key.casefold() in folded:
            raise AuditSafetyError("Source capture rejects special or case-colliding Git members: " + key)
        folded.add(key.casefold())
        entries[key] = {"bytes": int(size), "gitBlob": blob, "gitMode": mode}
    if not REQUIRED.issubset(entries) or not 1 <= len(entries) <= 20000:
        raise AuditSafetyError("The Git commit lacks bounded complete application build inputs")
    if sum(row["bytes"] for row in entries.values()) > MAX_BYTES:
        raise AuditSafetyError("Git build input capture exceeds 64 MiB")
    request = "".join(row["gitBlob"] + "\n" for row in entries.values()).encode("ascii")
    response = io.BytesIO(git(repository, "cat-file", "--batch", data=request))
    blobs = {}
    for key, row in entries.items():
        header = response.readline().decode("ascii").strip().split()
        if header != [row["gitBlob"], "blob", str(row["bytes"])]:
            raise AuditSafetyError("Git batch object header differs from the pinned tree")
        raw = response.read(row["bytes"])
        if len(raw) != row["bytes"] or response.read(1) != b"\n":
            raise AuditSafetyError("Git batch object is truncated")
        algorithm = hashlib.sha1 if len(row["gitBlob"]) == 40 else hashlib.sha256
        actual = algorithm(b"blob " + str(len(raw)).encode("ascii") + b"\0" + raw).hexdigest()
        if actual != row["gitBlob"]:
            raise AuditSafetyError("Git blob content differs from its immutable object ID")
        row["sha256"] = hashlib.sha256(raw).hexdigest()
        blobs[key] = raw
    if response.read(1):
        raise AuditSafetyError("Unexpected trailing Git batch output")
    return tree, entries, blobs


def physical_guard(root: Path, required: int = 0) -> None:
    root = owned_output(root)
    if os.name == "nt":
        free_c = shutil.disk_usage("C:\\").free
    elif root.is_relative_to(Path("/mnt/d")):
        free_c = shutil.disk_usage("/mnt/c").free
    else:
        raise AuditSafetyError("New product source capture must be on owned D storage")
    if free_c < 1024 * 1024 * 1024:
        raise AuditSafetyError("Physical C reserve blocks source capture before write")
    capacity(root, required, 256 * 1024 * 1024)


def fresh_json(path: Path, payload: dict) -> None:
    raw = (json.dumps(payload, ensure_ascii=True, indent=2) + "\n").encode("ascii")
    with owned_output(path).open("xb") as stream:
        stream.write(raw)


def capture(commit: str, root: Path, guard=None) -> Path:
    root = plain_path(root, root=OWNED / "scratch")
    if os.path.lexists(root):
        raise AuditSafetyError("Existing product source capture is preserved")
    tree, entries, blobs = commit_inputs(OWNED.parents[1], commit)
    metadata, target = product_identity(blobs)
    total = sum(row["bytes"] for row in entries.values())
    physical_guard(root, total + len(entries) * 4096 + 1024 * 1024)
    if guard is not None:
        guard()
    root.mkdir(mode=0o700)
    marker = {"schemaVersion": 1, "kind": "owned-immutable-git-source", "owner": "portability-audit",
              "rootRelativeToOwned": root.relative_to(OWNED).as_posix(), "token": uuid.uuid4().hex,
              "gitCommit": commit, "gitTree": tree, "sourceSetSha256": records_hash(entries)}
    fresh_json(root / MARKER, marker)
    last_guard = __import__("time").monotonic()
    for key, row in entries.items():
        physical_guard(root, row["bytes"])
        now = __import__("time").monotonic()
        if guard is not None and now - last_guard >= 0.5:
            guard()
            last_guard = now
        destination = plain_path(root / relative_key(key), root=root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as stream:
            stream.write(blobs[key])
        if os.name != "nt" and row["gitMode"] == "100755":
            destination.chmod(0o755)
    payload = {"schemaVersion": 1, "kind": "owned-git-production-source-capture", "owner": "portability-audit",
               "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(), "gitCommit": commit, "gitTree": tree,
               "rootRelativeToOwned": marker["rootRelativeToOwned"], "markerSha256": sha256(root / MARKER),
               "selectionVersion": 1, "files": entries, "fileCount": len(entries), "sourceBytes": total,
               "sourceSetSha256": records_hash(entries), "metadata": metadata, "productTarget": target,
               "captureToolSha256": sha256(Path(__file__)), "completeProductVerified": False,
               "scope": "All committed native/cmake/bootstrap/acceptance/assets build inputs; no historical objects or SDK copy"}
    physical_guard(root, 1024 * 1024)
    if guard is not None:
        guard()
    fresh_json(root / MANIFEST, payload)
    fresh_json(root / COMPLETE, {"schemaVersion": 1, "kind": "owned-git-source-capture-complete",
                                "manifestSha256": sha256(root / MANIFEST), "markerSha256": payload["markerSha256"],
                                "gitCommit": commit, "sourceSetSha256": payload["sourceSetSha256"],
                                "fileCount": len(entries), "sourceBytes": total, "captureComplete": True,
                                "completeProductVerified": False})
    return root / MANIFEST


class GitSourceFreeze:
    def __init__(self, manifest: Path, expected_sha256: str, commit: str):
        if not isinstance(expected_sha256, str) or not HASH.fullmatch(expected_sha256):
            raise AuditSafetyError("Explicit Git source manifest SHA-256 is required")
        self.manifest = plain_path(manifest, root=OWNED / "scratch", must_exist=True)
        if self.manifest.name != MANIFEST or self.manifest.stat().st_size > 8 * 1024 * 1024:
            raise AuditSafetyError("Bounded owned Git source manifest required")
        self.raw = self.manifest.read_bytes()
        if hashlib.sha256(self.raw).hexdigest() != expected_sha256:
            raise AuditSafetyError("Git source manifest differs from its explicit pin")
        self.payload = json.loads(self.raw)
        self.root = self.manifest.parent
        self.expected_sha256, self.commit = expected_sha256, commit
        self.marker_raw = plain_path(self.root / MARKER, root=self.root, must_exist=True).read_bytes()
        self.complete_raw = plain_path(self.root / COMPLETE, root=self.root, must_exist=True).read_bytes()
        marker, complete = json.loads(self.marker_raw), json.loads(self.complete_raw)
        tree, entries, blobs = commit_inputs(OWNED.parents[1], commit)
        metadata, target = product_identity(blobs)
        digest = records_hash(entries)
        if (not all(isinstance(doc, dict) for doc in (self.payload, marker, complete))
                or self.payload.get("schemaVersion") != 1 or self.payload.get("kind") != "owned-git-production-source-capture"
                or self.payload.get("owner") != "portability-audit" or self.payload.get("selectionVersion") != 1
                or self.payload.get("gitCommit") != commit or self.payload.get("gitTree") != tree
                or self.payload.get("files") != entries or self.payload.get("sourceSetSha256") != digest
                or self.payload.get("metadata") != metadata or self.payload.get("productTarget") != target
                or self.payload.get("fileCount") != len(entries)
                or self.payload.get("sourceBytes") != sum(row["bytes"] for row in entries.values())
                or self.payload.get("rootRelativeToOwned") != self.root.relative_to(OWNED).as_posix()
                or self.payload.get("markerSha256") != hashlib.sha256(self.marker_raw).hexdigest()
                or marker.get("schemaVersion") != 1 or marker.get("kind") != "owned-immutable-git-source"
                or marker.get("gitCommit") != commit or marker.get("rootRelativeToOwned") != self.payload["rootRelativeToOwned"]
                or marker.get("gitTree") != tree or marker.get("sourceSetSha256") != digest
                or marker.get("owner") != "portability-audit" or not marker.get("token")
                or complete.get("schemaVersion") != 1 or complete.get("kind") != "owned-git-source-capture-complete"
                or complete.get("captureComplete") is not True or complete.get("manifestSha256") != expected_sha256
                or complete.get("gitCommit") != commit or complete.get("markerSha256") != self.payload["markerSha256"]
                or complete.get("sourceSetSha256") != digest or complete.get("fileCount") != len(entries)
                or complete.get("sourceBytes") != sum(row["bytes"] for row in entries.values())):
            raise AuditSafetyError("Git source capture does not bind this owner/root/commit/completion")
        info = self.root.stat()
        self.identity = (info.st_dev, info.st_ino, info.st_uid)
        self.inputs = [{"path": key, **entries[key]} for key in sorted(REQUIRED)]
        self.evidence = {"kind": "immutable-git-product-source-binding", "manifestPath": str(self.manifest),
                         "manifestSha256": expected_sha256, "sourceRoot": str(self.root), "gitCommit": commit,
                         "gitTree": tree, "sourceSetSha256": digest, "fileCount": len(entries),
                         "sourceBytes": sum(row["bytes"] for row in entries.values()), "inputs": self.inputs,
                         "metadata": metadata, "productTarget": target,
                         "sourceVerifiedBefore": False, "sourceVerifiedAfter": False, "completeProductVerified": False}
        self.postcheck()
        self.evidence.update(sourceVerifiedBefore=True, sourceVerifiedAfter=False)

    def check(self):
        root = plain_path(self.root, root=OWNED / "scratch", must_exist=True)
        info = root.stat()
        if ((info.st_dev, info.st_ino, info.st_uid) != self.identity or self.manifest.read_bytes() != self.raw
                or (root / MARKER).read_bytes() != self.marker_raw or (root / COMPLETE).read_bytes() != self.complete_raw):
            raise AuditSafetyError("Pinned Git source identity or control bytes changed")
        for row in self.inputs:
            path = plain_path(root / relative_key(row["path"]), root=root, must_exist=True)
            if not path.is_file() or path.stat().st_size != row["bytes"] or sha256(path) != row["sha256"]:
                raise AuditSafetyError("Pinned Git build/recipe input changed: " + row["path"])

    def postcheck(self):
        self.check()
        actual = set()
        for directory, folders, files in os.walk(self.root, followlinks=False):
            for name in folders:
                plain_path(Path(directory) / name, root=self.root, must_exist=True)
            for name in files:
                path = plain_path(Path(directory) / name, root=self.root, must_exist=True)
                key = path.relative_to(self.root).as_posix()
                if key in {MARKER, MANIFEST, COMPLETE}:
                    continue
                row = self.payload["files"].get(key)
                if row is None or not path.is_file() or path.stat().st_size != row["bytes"] or sha256(path) != row["sha256"]:
                    raise AuditSafetyError("Git source member is changed or unlisted: " + key)
                actual.add(key)
        if actual != set(self.payload["files"]):
            raise AuditSafetyError("Git source member set is incomplete")
        self.evidence["sourceVerifiedAfter"] = True
        return self.evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    try:
        path = capture(args.commit, args.root)
        checked = GitSourceFreeze(path, sha256(path), args.commit)
        print(json.dumps(checked.evidence, ensure_ascii=True))
        return 0
    except (AuditSafetyError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
