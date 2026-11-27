#!/usr/bin/env python3
"""Bounded, owner-only audit output; a failed preflight never truncates evidence."""
from __future__ import annotations

from contextlib import contextmanager
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import uuid

OWNED = Path(__file__).resolve().parent
WRITE_HEADROOM = 64 * 1024 * 1024
BUILD_HEADROOM = 1024 * 1024 * 1024


class AuditSafetyError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def plain_path(path: Path, *, root: Path | None = None, must_exist: bool = False) -> Path:
    path = Path(path)
    if ".." in path.parts:
        raise AuditSafetyError("Refusing traversal before path normalization: " + str(path))
    path = Path(os.path.abspath(path))
    for item in (path, *path.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise AuditSafetyError("Refusing a symlink/junction/reparse path: " + str(item))
    if root is not None and not path.is_relative_to(Path(os.path.abspath(root))):
        raise AuditSafetyError("Path lies outside the exact owner root: " + str(path))
    if must_exist and not path.exists():
        raise AuditSafetyError("Required path is absent: " + str(path))
    return path


def capacity(path: Path, required: int = 0, headroom: int = WRITE_HEADROOM) -> dict:
    if required < 0 or headroom < 0:
        raise ValueError("Capacity quantities cannot be negative")
    anchor = plain_path(path)
    while not anchor.exists():
        anchor = anchor.parent
    usage = shutil.disk_usage(anchor)
    if usage.free < required + headroom:
        raise AuditSafetyError(f"Insufficient capacity before write: {anchor}; "
                               f"free={usage.free}, required={required}, headroom={headroom}")
    return {"anchor": str(anchor), "freeBytes": usage.free, "requiredBytes": required,
            "headroomBytes": headroom}


def owned_output(path: Path) -> Path:
    path = plain_path(path, root=OWNED)
    if os.name == "nt" and path.drive.casefold() != "d:":
        raise AuditSafetyError("New audit output must remain on the owner's D worktree")
    return path


def atomic_bytes(path: Path, data: bytes) -> None:
    path = owned_output(path)
    capacity(path, len(data))
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(".audit-write-" + uuid.uuid4().hex)
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        plain_path(path, root=OWNED)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def atomic_json(path: Path, payload: dict) -> None:
    data = (json.dumps(payload, ensure_ascii=True, indent=2) + "\n").encode("utf-8")
    atomic_bytes(path, data)


def create_scratch(required: int = 64 * 1024 * 1024) -> Path:
    base = owned_output(OWNED / "scratch")
    capacity(base, required, BUILD_HEADROOM)
    base.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="run-", dir=base))
    atomic_json(scratch / ".nickel-portability-owned.json", {
        "owner": str(OWNED), "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "scratchPolicy": "owner-worktree-only; preserve foreign and historical C artifacts"})
    return scratch


@contextmanager
def single_worker():
    lock = owned_output(OWNED / "scratch" / ".audit-worker.lock")
    capacity(lock)
    lock.parent.mkdir(parents=True, exist_ok=True)
    token = json.dumps({"pid": os.getpid(), "token": uuid.uuid4().hex})
    try:
        with lock.open("x", encoding="ascii") as stream:
            stream.write(token)
    except FileExistsError as error:
        raise AuditSafetyError("Another or interrupted audit worker holds the lock: " + str(lock)) from error
    try:
        yield
    finally:
        if plain_path(lock, root=OWNED).read_text(encoding="ascii") == token:
            lock.unlink()
