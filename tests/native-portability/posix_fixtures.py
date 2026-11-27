#!/usr/bin/env python3
"""Per-run synthetic tmpfs fixtures with identity-checked, descriptor-relative cleanup."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import uuid

from audit_support import AuditSafetyError, OWNED, atomic_json, capacity, owned_output, plain_path

LIMIT = 1024 * 1024
ENTRY_LIMIT = 128
DEPTH_LIMIT = 24
MARKER = ".nickel-portability-owned.json"
DIRECTORY_FLAGS = getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0) | os.O_RDONLY


def identity(info) -> dict:
    return {"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid,
            "mode": stat.S_IFMT(info.st_mode)}


def same(info, expected: dict) -> bool:
    return identity(info) == {key: expected[key] for key in ("device", "inode", "uid", "mode")}


class RamFixture:
    def __init__(self):
        if sys.platform != "linux" or not hasattr(os, "O_NOFOLLOW"):
            raise AuditSafetyError("RAM fixtures require a real Linux no-follow host")
        self.owner = plain_path(OWNED, must_exist=True)
        capacity(self.owner, 256 * 1024)
        self.base = plain_path(Path("/dev/shm"), must_exist=True)
        observed = subprocess.run(["stat", "-f", "-c", "%T", str(self.base)], check=True,
                                  capture_output=True, text=True, timeout=10).stdout.strip()
        if observed != "tmpfs":
            raise AuditSafetyError("The exact /dev/shm fixture parent is not tmpfs")
        available = os.statvfs(self.base)
        if available.f_bavail * available.f_frsize < 2 * LIMIT:
            raise AuditSafetyError("Insufficient tmpfs capacity for a bounded fixture")
        self.parent_fd = os.open(self.base, DIRECTORY_FLAGS)
        self.parent_identity = identity(os.fstat(self.parent_fd))
        self.name = "nickel-portability-" + uuid.uuid4().hex
        self.path = self.base / self.name
        self.fd = None
        self.peak_bytes = 0
        self.closed = False
        try:
            os.mkdir(self.name, mode=0o700, dir_fd=self.parent_fd)
            self.fd = os.open(self.name, DIRECTORY_FLAGS, dir_fd=self.parent_fd)
            self.root_identity = identity(os.fstat(self.fd))
            if self.root_identity["uid"] != os.geteuid():
                raise AuditSafetyError("Fixture root is not owned by this exact effective UID")
            self.marker = {"owner": str(self.owner), "root": str(self.path),
                           "rootIdentity": self.root_identity, "token": uuid.uuid4().hex,
                           "limitBytes": LIMIT, "syntheticOnly": True}
            self.marker_raw = (json.dumps(self.marker, sort_keys=True) + "\n").encode("ascii")
            marker_fd = os.open(MARKER, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                                0o600, dir_fd=self.fd)
            try:
                written = os.write(marker_fd, self.marker_raw)
                if written != len(self.marker_raw):
                    raise AuditSafetyError("Incomplete fixture owner marker")
                os.fsync(marker_fd)
                self.marker_identity = identity(os.fstat(marker_fd))
            finally:
                os.close(marker_fd)
            self.verify()
        except BaseException:
            if self.fd is not None:
                os.close(self.fd)
            os.close(self.parent_fd)
            raise

    def verify(self) -> None:
        if self.closed or not same(os.fstat(self.parent_fd), self.parent_identity):
            raise AuditSafetyError("Fixture parent descriptor identity changed")
        if not same(os.stat(self.base, follow_symlinks=False), self.parent_identity):
            raise AuditSafetyError("Exact tmpfs parent path identity changed")
        if not same(os.fstat(self.fd), self.root_identity) or not same(
                os.stat(self.name, dir_fd=self.parent_fd, follow_symlinks=False), self.root_identity):
            raise AuditSafetyError("Exact fixture root identity changed; refusing cleanup")
        info = os.stat(MARKER, dir_fd=self.fd, follow_symlinks=False)
        if not same(info, self.marker_identity) or info.st_nlink != 1:
            raise AuditSafetyError("Fixture owner marker was replaced or linked")
        marker_fd = os.open(MARKER, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=self.fd)
        try:
            if not same(os.fstat(marker_fd), self.marker_identity) or os.read(marker_fd, 4097) != self.marker_raw:
                raise AuditSafetyError("Fixture owner marker no longer matches its captured identity/content")
        finally:
            os.close(marker_fd)

    def capture(self, *, final: bool = False, enforce_limit: bool = True) -> dict:
        self.verify()
        records = []
        charged = 4096

        def walk(fd: int, prefix: str, depth: int) -> None:
            nonlocal charged
            if depth > DEPTH_LIMIT:
                raise AuditSafetyError("Fixture traversal exceeds its bounded depth")
            for name in sorted(os.listdir(fd)):
                if name in {".", ".."} or "/" in name or not name:
                    raise AuditSafetyError("Unexpected fixture directory entry")
                if len(records) >= ENTRY_LIMIT:
                    raise AuditSafetyError("Fixture traversal exceeds its bounded entry count")
                info = os.stat(name, dir_fd=fd, follow_symlinks=False)
                record = {"path": prefix + name, **identity(info), "bytes": info.st_size,
                          "allocatedBytes": info.st_blocks * 512, "links": info.st_nlink}
                if info.st_dev != self.root_identity["device"] or info.st_uid != os.geteuid():
                    raise AuditSafetyError("Fixture member belongs to a different device or UID")
                records.append(record)
                if stat.S_ISDIR(info.st_mode):
                    charged += max(4096, info.st_blocks * 512)
                    child = os.open(name, DIRECTORY_FLAGS, dir_fd=fd)
                    try:
                        if not same(os.fstat(child), record):
                            raise AuditSafetyError("Fixture directory changed during no-follow traversal")
                        walk(child, prefix + name + "/", depth + 1)
                    finally:
                        os.close(child)
                elif stat.S_ISREG(info.st_mode):
                    if final and info.st_nlink != 1:
                        raise AuditSafetyError("Final fixture file has an unexplained hard link; preserving it")
                    charged += max(info.st_size, info.st_blocks * 512)
                    if final:
                        if info.st_size > LIMIT:
                            record["sha256"] = None
                        else:
                            file_fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=fd)
                            try:
                                if not same(os.fstat(file_fd), record):
                                    raise AuditSafetyError("Fixture file changed before capture")
                                content = bytearray()
                                while len(content) <= LIMIT:
                                    block = os.read(file_fd, min(65536, LIMIT + 1 - len(content)))
                                    if not block:
                                        break
                                    content.extend(block)
                                after = os.fstat(file_fd)
                                if len(content) != info.st_size or not same(after, record) or after.st_size != info.st_size:
                                    raise AuditSafetyError("Fixture file changed while capturing bounded content")
                                record["sha256"] = hashlib.sha256(content).hexdigest()
                            finally:
                                os.close(file_fd)
                elif stat.S_ISLNK(info.st_mode):
                    charged += max(info.st_size, info.st_blocks * 512)
                    record["linkTarget"] = os.readlink(name, dir_fd=fd)
                else:
                    raise AuditSafetyError("Fixture contains a special file; no broad deletion is permitted")
                if enforce_limit and charged > LIMIT:
                    raise AuditSafetyError("Fixture allocation/payload exceeds its 1 MiB budget")
        walk(self.fd, "", 0)
        self.verify()
        self.peak_bytes = max(self.peak_bytes, charged)
        return {"root": str(self.path), "rootIdentity": self.root_identity,
                "owner": str(self.owner), "markerIdentity": self.marker_identity,
                "markerSha256": hashlib.sha256(self.marker_raw).hexdigest(), "limitBytes": LIMIT,
                "chargedBytes": charged, "peakObservedBytes": self.peak_bytes,
                "withinBudget": charged <= LIMIT, "entryCount": len(records), "members": records}

    def cleanup(self) -> dict:
        captured = self.capture(final=True, enforce_limit=False)
        records = {item["path"]: item for item in captured["members"]}

        def clear(fd: int, prefix: str) -> None:
            self.verify()
            names = sorted(os.listdir(fd))
            for name in names:
                relative = prefix + name
                if relative == MARKER:
                    continue
                record = records.get(relative)
                if record is None or not same(os.stat(name, dir_fd=fd, follow_symlinks=False), record):
                    raise AuditSafetyError("Uncaptured or replaced fixture member; refusing deletion")
                if record["mode"] == stat.S_IFDIR:
                    child = os.open(name, DIRECTORY_FLAGS, dir_fd=fd)
                    try:
                        if not same(os.fstat(child), record):
                            raise AuditSafetyError("Captured cleanup directory identity changed")
                        clear(child, relative + "/")
                    finally:
                        os.close(child)
                    if not same(os.stat(name, dir_fd=fd, follow_symlinks=False), record):
                        raise AuditSafetyError("Fixture directory was replaced before rmdir")
                    os.rmdir(name, dir_fd=fd)
                else:
                    info = os.stat(name, dir_fd=fd, follow_symlinks=False)
                    if not same(info, record) or (record["mode"] == stat.S_IFREG and info.st_nlink != 1):
                        raise AuditSafetyError("Fixture leaf identity changed before unlink")
                    os.unlink(name, dir_fd=fd)
        try:
            clear(self.fd, "")
            self.verify()
            if os.listdir(self.fd) != [MARKER]:
                raise AuditSafetyError("Fixture root contains uncaptured additions; preserving the marker")
            os.unlink(MARKER, dir_fd=self.fd)
            if not same(os.stat(self.name, dir_fd=self.parent_fd, follow_symlinks=False), self.root_identity):
                raise AuditSafetyError("Fixture root changed immediately before rmdir")
            os.rmdir(self.name, dir_fd=self.parent_fd)
            absent = self.name not in os.listdir(self.parent_fd)
            return {"removedExactRoot": absent, "captured": captured,
                    "method": "Captured device/inode/UID, owner marker and dir_fd no-follow unlink/rmdir; symlink targets never traversed"}
        finally:
            self.close()

    def close(self) -> None:
        if not self.closed:
            os.close(self.fd)
            os.close(self.parent_fd)
            self.closed = True


def self_test(output: Path) -> int:
    output = owned_output(output)
    if output.exists():
        raise AuditSafetyError("Refusing existing RAM-fixture evidence")
    capacity(output, 256 * 1024)
    lease = RamFixture()
    result = {"kind": "real-linux-ram-fixture-helper-check", "host": sys.platform,
              "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(), "passed": False,
              "completeProductVerified": False, "scope": "Actual tmpfs helper only; not C++ product acceptance"}
    try:
        os.mkdir("fixture", dir_fd=lease.fd)
        directory = os.open("fixture", DIRECTORY_FLAGS, dir_fd=lease.fd)
        try:
            file_fd = os.open("synthetic", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                              0o600, dir_fd=directory)
            try:
                os.write(file_fd, b"owned synthetic fixture\n")
            finally:
                os.close(file_fd)
            os.symlink("../fixture/synthetic", "link", dir_fd=directory)
        finally:
            os.close(directory)
        result["capture"] = lease.capture(final=True)
        assert result["capture"]["withinBudget"]
        assert any(item.get("linkTarget") == "../fixture/synthetic" for item in result["capture"]["members"])
        original = lease.marker_raw
        marker_fd = os.open(MARKER, os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW, dir_fd=lease.fd)
        try:
            os.write(marker_fd, b"{\"owner\":\"foreign\"}\n")
        finally:
            os.close(marker_fd)
        try:
            lease.verify()
        except AuditSafetyError:
            result["modifiedMarkerRejected"] = True
        else:
            raise AssertionError("Altered owner marker was accepted")
        marker_fd = os.open(MARKER, os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW, dir_fd=lease.fd)
        try:
            os.write(marker_fd, original)
        finally:
            os.close(marker_fd)
        lease.verify()
        result["helperCheckPassed"] = True
    except BaseException as error:
        result.update(helperCheckPassed=False, error=str(error))
    finally:
        try:
            result["cleanup"] = lease.cleanup()
        except BaseException as error:
            lease.close()
            result.update(helperCheckPassed=False, cleanupError=str(error))
        atomic_json(output, result)
    print(json.dumps({"helperCheckPassed": result["helperCheckPassed"],
                      "removedExactRoot": result.get("cleanup", {}).get("removedExactRoot", False),
                      "completeProductVerified": False}))
    return 0 if result["helperCheckPassed"] and result.get("cleanup", {}).get("removedExactRoot") else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test-output", type=Path, required=True)
    args = parser.parse_args()
    try:
        raise SystemExit(self_test(args.self_test_output))
    except (OSError, ValueError, AuditSafetyError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(2)
