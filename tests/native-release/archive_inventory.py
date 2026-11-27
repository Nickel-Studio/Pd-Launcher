"""Hash actual program archive members without extraction or executable inspection."""
from __future__ import annotations

import hashlib
from pathlib import Path
import stat
import tarfile
import zipfile

from evidence_common import EvidenceError, relative_name, sha_file

MAX_MEMBERS = 20000
MAX_TOTAL_BYTES = 512 * 1024 * 1024


def stream_digest(stream, expected_bytes: int) -> str:
    result, read = hashlib.sha256(), 0
    while True:
        chunk = stream.read(256 * 1024)
        if not chunk:
            break
        read += len(chunk)
        if read > expected_bytes or read > MAX_TOTAL_BYTES:
            raise EvidenceError("Program archive member exceeded its declared/bounded size")
        result.update(chunk)
    if read != expected_bytes:
        raise EvidenceError("Program archive member length drift")
    return result.hexdigest()


def inventory(path: Path, format_name: str) -> list[dict]:
    rows, names, total = [], set(), 0

    def add(name: str, size: int, stream):
        nonlocal total
        name = relative_name(name)
        if name.casefold() in names or len(names) >= MAX_MEMBERS:
            raise EvidenceError("Duplicate/case-colliding or over-quota program archive")
        names.add(name.casefold())
        total += size
        if size < 0 or total > MAX_TOTAL_BYTES:
            raise EvidenceError("Program archive exceeds bounded uncompressed delivery budget")
        rows.append({"distributionPath": name, "bytes": size, "sha256": stream_digest(stream, size)})

    if format_name == "raw":
        return [{"distributionPath": path.name, "sha256": sha_file(path), "bytes": path.stat().st_size}]
    if format_name == "zip":
        with zipfile.ZipFile(path) as archive:
            if len(archive.infolist()) > MAX_MEMBERS:
                raise EvidenceError("Program ZIP has too many members")
            for item in archive.infolist():
                if item.is_dir():
                    relative_name(item.filename.rstrip("/"))
                    continue
                mode = item.external_attr >> 16
                if mode and stat.S_IFMT(mode) not in (0, stat.S_IFREG) or item.flag_bits & 1:
                    raise EvidenceError("Linked/nonregular/encrypted program ZIP member")
                with archive.open(item) as stream:
                    add(item.filename, item.file_size, stream)
    elif format_name in {"tar", "tar.gz", "tar.xz"}:
        with tarfile.open(path, "r|*") as archive:
            member_count = 0
            for item in archive:
                member_count += 1
                if member_count > MAX_MEMBERS:
                    raise EvidenceError("Program TAR has too many members")
                if item.isdir():
                    relative_name(item.name.rstrip("/"))
                    continue
                if not item.isfile() or item.issparse():
                    raise EvidenceError("Linked/nonregular/sparse program TAR member")
                with archive.extractfile(item) as stream:
                    add(item.name, item.size, stream)
    else:
        raise EvidenceError("Package format has no independently verified archive inventory adapter")
    return sorted(rows, key=lambda row: row["distributionPath"])


def verify_program_package(path: Path, method: dict, runtime: list[dict]) -> list[dict]:
    actual = inventory(path, method["format"])
    expected = sorted(({key: row[key] for key in ("distributionPath", "sha256", "bytes")} for row in runtime),
                      key=lambda row: row["distributionPath"])
    if method["format"] == "raw" and len(expected) == 1:
        actual[0]["distributionPath"] = expected[0]["distributionPath"]
    if actual != expected:
        raise EvidenceError("Actual program download contents differ from the complete runtime Artifact manifest")
    return actual
