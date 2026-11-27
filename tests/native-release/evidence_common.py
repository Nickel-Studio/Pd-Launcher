"""Bounded, byte-preserving helpers for release evidence, not product code."""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
LOCAL = "tests/native-release/_local"
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
COMMIT = re.compile(r"[0-9a-f]{40}(?:[0-9a-f]{24})?\Z")
MAX_JSON_BYTES = 16 * 1024 * 1024
RESERVE_BYTES = 64 * 1024 * 1024


class EvidenceError(ValueError):
    pass


class MissingEvidence(EvidenceError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def relative_name(value: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise EvidenceError("An exact nonempty relative path is required")
    name = value.replace("\\", "/")
    raw = PurePosixPath(name)
    if (raw.is_absolute() or PureWindowsPath(value).drive or
            any(part in {"", ".", ".."} for part in name.split("/")) or ":" in name):
        raise EvidenceError(f"Absolute, alternate-stream or traversing path rejected: {value}")
    return name


def inside(root: Path, value: str, *, must_exist: bool = False) -> Path:
    name = relative_name(value)
    root = root.resolve()
    cursor = root
    for part in name.split("/"):
        cursor /= part
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise EvidenceError(f"Links/reparse redirects are not followed: {value}")
        if cursor.exists() and os.name == "nt":
            attributes = cursor.stat(follow_symlinks=False).st_file_attributes
            if attributes & 0x400:
                raise EvidenceError(f"Reparse material rejected: {value}")
    target = cursor.resolve()
    if not target.is_relative_to(root):
        raise EvidenceError(f"Path escaped its evidence root: {value}")
    if must_exist and not target.exists():
        raise MissingEvidence(f"Missing evidence: {name}")
    return target


def sha_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_sha(value) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=True, allow_nan=False).encode("ascii")).hexdigest()


def json_bytes(value) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def read_json(path: Path):
    if not path.is_file():
        raise MissingEvidence(f"Missing JSON evidence: {path}")
    if path.stat().st_size > MAX_JSON_BYTES:
        raise EvidenceError("Evidence JSON exceeds the 16 MiB input bound")
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=_unique_object,
                          parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError(f"Nonfinite JSON: {value}")))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"Invalid JSON evidence {path}: {error}") from error


def reference(root: Path, row: dict | None, *, nonempty: bool = True) -> Path:
    if row is None or row == {}:
        raise MissingEvidence("No hash-identified material reference was supplied")
    if not isinstance(row, dict) or not SHA256.fullmatch(str(row.get("sha256", ""))):
        raise EvidenceError("Every evidence reference needs an independently recorded SHA-256")
    path = inside(root, row.get("path"), must_exist=True)
    if not path.is_file():
        raise EvidenceError(f"Evidence is not a regular file: {row['path']}")
    before = path.stat()
    if nonempty and before.st_size == 0:
        raise EvidenceError(f"Empty evidence: {row['path']}")
    if "bytes" in row and (type(row["bytes"]) is not int or before.st_size != row["bytes"]):
        raise EvidenceError(f"Evidence length drift: {row['path']}")
    if sha_file(path) != row["sha256"]:
        raise EvidenceError(f"Evidence hash drift: {row['path']}")
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise EvidenceError(f"Evidence changed during hashing: {row['path']}")
    return path


def file_ref(path: Path, root: Path = ROOT) -> dict:
    return {"path": path.relative_to(root.resolve()).as_posix(), "sha256": sha_file(path),
            "bytes": path.stat().st_size}


def check_space(path: Path, size: int, reserve: int = RESERVE_BYTES) -> None:
    ancestor = path
    while not ancestor.exists():
        ancestor = ancestor.parent
    if size < 0 or shutil.disk_usage(ancestor).free < size + reserve:
        raise EvidenceError(f"Insufficient D-local evidence space before write (need {size + reserve} bytes)")


def new_output(value: str, root: Path = ROOT, *, minimum_bytes: int = 1024 * 1024) -> Path:
    name = relative_name(value)
    if not name.startswith(LOCAL + "/"):
        raise EvidenceError(f"Generated output must be below {LOCAL}/")
    path = inside(root, name)
    if path.exists():
        raise EvidenceError(f"Output already exists; evidence is never silently overwritten: {name}")
    check_space(path, minimum_bytes)
    path.mkdir(parents=True, exist_ok=False)
    return path


def write_new(path: Path, raw: bytes) -> None:
    check_space(path, len(raw))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def write_json(path: Path, value) -> None:
    write_new(path, json_bytes(value))


def copy_small_input(path: Path, output: Path, label: str, root: Path = ROOT) -> dict:
    if path.stat().st_size > MAX_JSON_BYTES:
        raise EvidenceError("Raw input copy would exceed 16 MiB; large artifacts are hash-only references")
    target = output / "inputs" / relative_name(label)
    write_new(target, path.read_bytes())
    result = file_ref(target, root)
    result["originalPath"] = path.relative_to(root.resolve()).as_posix()
    return result


def load_python_tool(root: Path, row: dict, name: str):
    path = reference(root, row)
    if path.suffix != ".py":
        raise EvidenceError("Audit entry point must be a hashed Python source file")
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise EvidenceError("Unable to import the existing audit tool")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_git(root: Path, *args: str) -> bytes:
    result = subprocess.run(["git", *args], cwd=root, capture_output=True, check=False)
    if result.returncode:
        raise EvidenceError(result.stderr.decode("utf-8", errors="replace").strip())
    return result.stdout


def actual_commit(root: Path, value: str) -> str:
    if not isinstance(value, str) or not COMMIT.fullmatch(value):
        raise EvidenceError("An exact full commit hash is required, not a branch or guessed HEAD")
    resolved = read_git(root, "rev-parse", "--verify", value + "^{commit}").decode("ascii").strip()
    if resolved != value:
        raise EvidenceError("Commit identity does not resolve to the supplied hash")
    return resolved


def gate(status: str, detail: str, **evidence) -> dict:
    if status not in {"passed", "pending", "failed"}:
        raise EvidenceError("Invalid gate status")
    return {"status": status, "detail": detail, **evidence}


def overall(gates: dict) -> str:
    statuses = {value["status"] for value in gates.values()}
    return "failed" if "failed" in statuses else "pending" if "pending" in statuses or not statuses else "passed"


def exit_code(status: str) -> int:
    return {"passed": 0, "failed": 1, "pending": 2}[status]


class SingleWorker:
    """Never steal an existing lock or clean another run's files."""
    def __init__(self, root: Path):
        self.path = root / "single-worker.lock"
        self.token = uuid.uuid4().hex.encode("ascii")

    def __enter__(self):
        write_new(self.path, self.token)
        return self

    def __exit__(self, *_):
        if self.path.is_file() and self.path.read_bytes() == self.token:
            self.path.unlink()
