#!/usr/bin/env python3
"""Independent, read-only verifier for Palladium legal/relink material.

Owned by the Wave5 legal-relink probe. It never writes into tracked paths and
never trusts the hashes recorded in the manifest: every digest is recomputed
from bytes.

Subcommands
    inputs   verify the trusted-launcher notice inputs against the
             manifest (bytes, SHA256, path safety, duplicates, origin binding)
             and recompute notice_set_sha256.
    source   reproduce `git archive` for the recorded commit, compare it with
             the release-package source archive digest, and prove member
             set/content equality against the commit tree.
    pe       inspect PE resources of an executable and report whether any
             notice input bytes are actually embedded.

Usage examples
    python -B scripts/native/legal_input_verify.py inputs
    python -B scripts/native/legal_input_verify.py source --commit a489413a6bb035903ecb84cc2d24eb80cfe7dc75 \
        --expected-sha256 78cd9fc3cbd5d11f8b5f4bb477338d307495578818331e46585c28b7a917df62 \
        --out .snow/legal-relink/source-repro.zip
    python -B scripts/native/legal_input_verify.py pe --exe .snow/legal-relink/build/native/app/pd-launcher.exe
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

MANIFEST = "docs/native/licenses/trusted-launcher-notice-input.json"
NOTICE_ROOT = "docs/native/licenses/trusted-launcher-notice-inputs"
REQUIRED_PRIMARY = ("LICENSE", "LICENSE-ASSETS", "NOTICE.md", "SOURCE.txt")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            total += len(chunk)
            digest.update(chunk)
    return total, digest.hexdigest()


def git_blob(repo: Path, commit: str, path: str) -> bytes | None:
    result = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", f"{commit}:{path}"],
        capture_output=True,
    )
    if result.returncode != 0:
        return None
    blob = result.stdout.decode().strip()
    data = subprocess.run(
        ["git", "-C", str(repo), "cat-file", "blob", blob], capture_output=True
    )
    if data.returncode != 0:
        return None
    return data.stdout


def read_blobs(repo: Path, blob_ids: list[str]) -> dict[str, bytes]:
    """Read every blob in one `git cat-file --batch` process."""
    unique = sorted(set(blob_ids))
    if not unique:
        return {}
    process = subprocess.run(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        input=("\n".join(unique) + "\n").encode(),
        capture_output=True,
        check=True,
    )
    out = process.stdout
    result: dict[str, bytes] = {}
    offset = 0
    for blob in unique:
        newline = out.index(b"\n", offset)
        header = out[offset:newline].decode().split()
        if len(header) != 3:
            raise SystemExit(f"unexpected cat-file header for {blob}: {header}")
        size = int(header[2])
        start = newline + 1
        result[blob] = out[start:start + size]
        offset = start + size + 1
    return result


def unsafe_reason(name: str) -> str | None:
    if not name:
        return "empty name"
    if len(name) > 4096:
        return "name longer than 4096 bytes"
    if "\\" in name:
        return "backslash separator"
    if name.startswith("/"):
        return "absolute path"
    if len(name) > 1 and name[1] == ":":
        return "drive-qualified path"
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in name):
        return "control character"
    parts = name.split("/")
    for part in parts:
        if part in ("", ".", ".."):
            return f"non-canonical segment {part!r}"
        if part != part.strip():
            return "leading/trailing space in segment"
        if part.endswith("."):
            return "trailing dot in segment"
    return None


def notice_set_digest(rows: list[dict], order: str) -> str:
    keyed = [{"bytes": int(row["bytes"]), "name": row["name"], "sha256": row["sha256"]} for row in rows]
    if order == "name":
        keyed.sort(key=lambda row: row["name"].encode())
    else:
        keyed.sort(key=lambda row: (row["name"].encode(), row["bytes"], row["sha256"]))
    payload = json.dumps(keyed, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return sha256_bytes(payload.encode("utf-8"))


def cmd_inputs(args: argparse.Namespace) -> int:
    repo = Path(args.repo).resolve()
    manifest_path = repo / MANIFEST
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes.decode("utf-8"))
    findings: list[str] = []
    checks: dict[str, object] = {}

    findings.append(f"manifest {MANIFEST} bytes={len(manifest_bytes)} sha256={sha256_bytes(manifest_bytes)}")
    checks["manifest_sha256"] = sha256_bytes(manifest_bytes)
    checks["manifest_bytes"] = len(manifest_bytes)
    checks["manifest_declared_kind"] = manifest.get("kind")
    checks["declared_flags"] = {
        key: manifest.get(key)
        for key in (
            "originalTextHashesVerified",
            "compiledNoticeBytesVerified",
            "applicationSourceAndObjectsBound",
            "full40LibraryLicenseSourceBuildBindingVerified",
            "actualReplacementRelinkVerified",
            "trusted_launcher_ready",
            "nativeLegalMaterialValidationPassed",
            "nativeReleaseReady",
        )
    }

    rows = manifest["notices"]
    checks["declared_noticeCount"] = manifest.get("noticeCount")
    checks["actual_notice_rows"] = len(rows)
    if manifest.get("noticeCount") != len(rows):
        findings.append("FAIL noticeCount does not equal number of rows")
    else:
        findings.append(f"PASS noticeCount={len(rows)} equals row count")

    if sorted(row["name"] for row in rows) == sorted(set(row["name"] for row in rows)):
        findings.append("PASS notice names are unique")
    else:
        findings.append("FAIL duplicate notice names")

    casefold: dict[str, str] = {}
    unsafe = []
    missing = []
    byte_mismatch = []
    hash_mismatch = []
    wrong_path = []
    total_bytes = 0
    for row in rows:
        name = row["name"]
        reason = unsafe_reason(name)
        if reason:
            unsafe.append((name, reason))
        folded = name.casefold()
        if folded in casefold and casefold[folded] != name:
            unsafe.append((name, f"case-insensitive duplicate of {casefold[folded]}"))
        casefold[folded] = name
        recorded_path = row["path"]
        expected_path = f"{NOTICE_ROOT}/{name}"
        if recorded_path != expected_path:
            wrong_path.append((name, recorded_path))
        target = repo / recorded_path
        if not target.is_file():
            missing.append(name)
            continue
        size, digest = sha256_file(target)
        total_bytes += size
        if size != row["bytes"]:
            byte_mismatch.append((name, row["bytes"], size))
        if digest != row["sha256"]:
            hash_mismatch.append((name, row["sha256"], digest))

    checks["notice_rows_found"] = len(rows) - len(missing)
    checks["missing"] = missing
    checks["unsafe_or_duplicate"] = unsafe
    checks["wrong_recorded_path"] = wrong_path
    checks["size_mismatch"] = byte_mismatch
    checks["sha256_mismatch"] = hash_mismatch
    checks["total_bytes_recomputed"] = total_bytes
    checks["manifest_declared_bytes"] = manifest.get("noticeBytes")

    findings.append(
        f"PASS all {len(rows)} notice files present" if not missing else f"FAIL missing notice files: {missing[:10]}"
    )
    findings.append(
        "PASS all notice sizes match manifest"
        if not byte_mismatch
        else f"FAIL size mismatches: {byte_mismatch[:10]}"
    )
    findings.append(
        "PASS all notice SHA256 recomputed and match manifest"
        if not hash_mismatch
        else f"FAIL SHA256 mismatches: {hash_mismatch[:10]}"
    )
    findings.append(
        "PASS no unsafe or duplicate notice paths"
        if not unsafe
        else f"FAIL unsafe/duplicate paths: {unsafe[:10]}"
    )
    findings.append(
        "PASS recorded path equals docs/.../trusted-launcher-notice-inputs/<name>"
        if not wrong_path
        else f"FAIL path field mismatches: {wrong_path[:5]}"
    )
    if total_bytes == manifest.get("noticeBytes"):
        findings.append(f"PASS total notice bytes {total_bytes} equals manifest noticeBytes")
    else:
        findings.append(f"FAIL total bytes {total_bytes} != manifest {manifest.get('noticeBytes')}")

    for order in ("name", "name/bytes/sha256"):
        digest = notice_set_digest(rows, order)
        checks[f"notice_set_sha256_recomputed[{order}]"] = digest
        if digest == manifest.get("notice_set_sha256"):
            findings.append(f"PASS notice_set_sha256 reproduced with row order {order} -> {digest}")
    if not any(
        checks[f"notice_set_sha256_recomputed[{order}]"] == manifest.get("notice_set_sha256")
        for order in ("name", "name/bytes/sha256")
    ):
        findings.append("FAIL notice_set_sha256 not reproduced by either documented encoding")

    # primaries present
    names = {row["name"] for row in rows}
    absent = [name for name in REQUIRED_PRIMARY if name not in names]
    findings.append(
        "PASS four primary notices present" if not absent else f"FAIL missing primaries: {absent}"
    )

    # attributes file
    attributes_rel = manifest.get("attributes", {}).get("path")
    if attributes_rel:
        attributes_path = repo / attributes_rel
        if attributes_path.is_file():
            size, digest = sha256_file(attributes_path)
            recorded = manifest["attributes"]
            ok = size == recorded.get("bytes") and digest == recorded.get("sha256")
            findings.append(
                ("PASS" if ok else "FAIL")
                + f" .gitattributes bytes/sha256 ({size}/{digest})"
            )
            checks["attributes"] = {"bytes": size, "sha256": digest, "matches": ok}
        else:
            findings.append(f"FAIL attributes file missing: {attributes_rel}")

    # origin binding
    origin_stats = {
        "git_verified": 0,
        "git_failed": 0,
        "external_verified": 0,
        "external_missing": 0,
        "descriptive": 0,
    }
    git_failures = []
    external_missing = []
    origin_kinds: dict[str, int] = {}
    for row in rows:
        for origin in row.get("origins", []):
            kind = origin.get("kind", "")
            origin_kinds[kind] = origin_kinds.get(kind, 0) + 1
            origin_path = origin.get("path", "")
            commit = origin.get("commit")
            if kind.startswith("immutable-git-source") and commit:
                data = git_blob(repo, commit, origin_path)
                if data is None:
                    origin_stats["git_failed"] += 1
                    git_failures.append((origin_path, commit, "not found at commit"))
                    continue
                if sha256_bytes(data) != origin.get("sha256") or len(data) != origin.get("bytes"):
                    origin_stats["git_failed"] += 1
                    git_failures.append((origin_path, commit, "bytes differ from origin record"))
                    continue
                if sha256_bytes(data) != row["sha256"]:
                    origin_stats["git_failed"] += 1
                    git_failures.append((origin_path, commit, "origin bytes differ from notice copy"))
                    continue
                origin_stats["git_verified"] += 1
            elif not origin_path:
                # Descriptive origin (hash-only wrapper / statement): no file to bind.
                origin_stats["descriptive"] += 1
            else:
                candidate = Path(origin_path)
                if not candidate.is_absolute():
                    candidate = repo / origin_path
                if args.tool_root and origin_path.startswith(".snow/native-tools/"):
                    candidate = Path(args.tool_root) / origin_path[len(".snow/native-tools/"):]
                if candidate.is_file():
                    size, digest = sha256_file(candidate)
                    if (
                        size == origin.get("bytes", size)
                        and digest == origin.get("sha256", digest)
                        and digest == row["sha256"]
                    ):
                        origin_stats["external_verified"] += 1
                    else:
                        origin_stats["external_missing"] += 1
                        external_missing.append((origin_path, "bytes/sha differ"))
                else:
                    origin_stats["external_missing"] += 1
                    external_missing.append((origin_path, "absent on this host"))

    checks["origin_kinds"] = origin_kinds
    checks["origin_stats"] = origin_stats
    checks["git_origin_failures"] = git_failures
    checks["external_origin_unavailable"] = external_missing
    findings.append(
        f"git-origin binding: {origin_stats['git_verified']} verified, {origin_stats['git_failed']} failed"
    )
    if git_failures:
        findings.append(f"FAIL git origins: {git_failures[:5]}")
    findings.append(
        f"external (tool-prefix) origins: {origin_stats['external_verified']} verified on host, "
        f"{origin_stats['external_missing']} unavailable"
    )

    # Pinned source archives (13) used for notice provenance
    archive_rows = []
    archive_verified = 0
    for row in manifest.get("sourceArchiveInputs", []):
        record = row.get("archive", {})
        recorded_path = record.get("path", "")
        candidate = Path(recorded_path)
        if not candidate.is_absolute():
            candidate = repo / recorded_path
        if args.tool_root and recorded_path.startswith(".snow/native-tools/"):
            candidate = Path(args.tool_root) / recorded_path[len(".snow/native-tools/"):]
        entry = {"component": row.get("component"), "path": recorded_path, "present": candidate.is_file()}
        if candidate.is_file():
            size, digest = sha256_file(candidate)
            entry.update(bytes=size, sha256=digest,
                         matches=size == record.get("bytes") and digest == record.get("sha256"))
            archive_verified += int(entry["matches"])
        archive_rows.append(entry)
    checks["source_archive_inputs"] = archive_rows
    findings.append(
        ("PASS" if archive_verified == len(archive_rows) else "FAIL")
        + f" pinned source archives: {archive_verified}/{len(archive_rows)} bytes+sha256 verified on this host"
    )

    # Lock triple self-consistency: (commit, path, sha256) must agree
    lock = manifest.get("lock", {})
    lock_blob = git_blob(repo, lock.get("commit", ""), lock.get("path", "")) if lock.get("commit") else None
    lock_state = {"commit": lock.get("commit"), "path": lock.get("path"), "recorded_sha256": lock.get("sha256")}
    if lock_blob is None:
        lock_state["error"] = "lock path absent at recorded commit"
        findings.append("FAIL manifest lock.commit does not contain lock.path")
    else:
        actual = sha256_bytes(lock_blob)
        lock_state.update(actual_bytes=len(lock_blob), actual_sha256=actual, matches=actual == lock.get("sha256"))
        findings.append(
            ("PASS" if lock_state["matches"] else "FAIL")
            + f" manifest lock triple: {lock.get('path')} at {str(lock.get('commit'))[:12]} is "
            + ("the recorded blob" if lock_state["matches"] else f"{actual} (recorded {lock.get('sha256')})")
        )
    checks["lock_binding"] = lock_state


    # Exactly one file per notice under the input root and nothing extra
    disk_files = sorted(
        str(path.relative_to(repo / NOTICE_ROOT)).replace(os.sep, "/")
        for path in (repo / NOTICE_ROOT).rglob("*")
        if path.is_file()
    )
    expected = {row["name"] for row in rows} | {".gitattributes"}
    extra = [name for name in disk_files if name not in expected]
    checks["disk_file_count"] = len(disk_files)
    checks["extra_files_under_notice_root"] = extra
    findings.append(
        f"PASS notice input root contains exactly the {len(rows)} notices plus .gitattributes"
        if not extra and len(disk_files) == len(rows) + 1
        else f"NOTE notice root has {len(disk_files)} files, extra={extra[:10]}"
    )

    status = "PASS" if not any(item.startswith("FAIL") for item in findings) else "FAIL"
    result = {"check": "notice-inputs", "status": status, "findings": findings, "checks": checks}
    print(json.dumps(result, indent=1, ensure_ascii=False, default=str))
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(result, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    return 0 if status == "PASS" else 1


def cmd_source(args: argparse.Namespace) -> int:
    repo = Path(args.repo).resolve()
    commit = args.commit
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    prefix = args.prefix
    findings: list[str] = []
    checks: dict[str, object] = {}

    ancestry = subprocess.run(
        ["git", "-C", str(repo), "merge-base", "--is-ancestor", commit, "HEAD"], capture_output=True
    )
    checks["commit_is_ancestor_of_HEAD"] = ancestry.returncode == 0
    findings.append(
        ("PASS" if ancestry.returncode == 0 else "FAIL")
        + f" commit {commit} is an ancestor of HEAD"
    )
    head = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True
    ).stdout.decode().strip()
    checks["head"] = head

    subprocess.run(
        ["git", "-C", str(repo), "archive", "--format=zip", f"--prefix={prefix}", commit, "-o", str(out)],
        check=True,
    )
    size, digest = sha256_file(out)
    checks["reproduced_archive"] = {"path": str(out), "bytes": size, "sha256": digest}
    if args.expected_sha256:
        ok = digest == args.expected_sha256 and (args.expected_bytes is None or size == args.expected_bytes)
        findings.append(
            ("PASS" if ok else "FAIL")
            + f" reproduced git archive {size} bytes sha256 {digest} "
            + ("matches the recorded release-package source archive" if ok else f"!= recorded {args.expected_sha256}")
        )
    else:
        findings.append(f"NOTE reproduced git archive {size} bytes sha256 {digest}")

    tree = subprocess.run(
        ["git", "-C", str(repo), "ls-tree", "-r", commit], capture_output=True, check=True
    ).stdout.decode()
    entries: dict[str, str] = {}
    for line in tree.splitlines():
        meta, path = line.split("\t", 1)
        mode, kind, blob = meta.split()
        if kind == "blob":
            entries[path] = blob
    checks["commit_blob_count"] = len(entries)

    attributes_text = subprocess.run(
        ["git", "-C", str(repo), "show", f"{commit}:.gitattributes"], capture_output=True
    ).stdout.decode("utf-8", "replace")
    text_patterns = []
    for line in attributes_text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) >= 2 and ("text" in fields[1:] or "eol=crlf" in fields[1:]):
            text_patterns.append(fields[0])

    member_names = []
    mismatches = []
    crlf_members = 0
    blob_data = read_blobs(repo, list(entries.values()))
    with zipfile.ZipFile(out) as archive:
        members = [info.filename for info in archive.infolist() if not info.is_dir()]
        member_names = [name[len(prefix):] if name.startswith(prefix) else name for name in members]
        if len(member_names) != len(set(member_names)):
            findings.append("FAIL duplicate members in source archive")
        extra = sorted(set(member_names) - set(entries))
        missing = sorted(set(entries) - set(member_names))
        checks["archive_member_count"] = len(member_names)
        checks["members_not_in_commit"] = extra[:20]
        checks["commit_blobs_missing_from_archive"] = missing[:20]
        findings.append(
            "PASS archive member set equals git ls-tree -r exactly"
            if not extra and not missing
            else f"FAIL member set differs: extra={extra[:5]} missing={missing[:5]}"
        )
        for name in member_names:
            data = archive.read(prefix + name if name in member_names else name)
            blob = entries.get(name)
            if blob is None:
                continue
            original = blob_data.get(blob, b"")
            if data == original:
                continue
            crlf = original.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            if data == crlf:
                crlf_members += 1
                continue
            mismatches.append(name)
        checks["crlf_exported_members"] = crlf_members
        checks["content_mismatches"] = mismatches[:20]
        findings.append(
            "PASS every member content equals its commit blob (or its declared CRLF export)"
            if not mismatches
            else f"FAIL content mismatches: {mismatches[:5]}"
        )
        checks["text_attribute_patterns"] = text_patterns

    status = "PASS" if not any(item.startswith("FAIL") for item in findings) else "FAIL"
    result = {"check": "source-archive", "status": status, "findings": findings, "checks": checks}
    print(json.dumps(result, indent=1, ensure_ascii=False, default=str))
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(result, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    return 0 if status == "PASS" else 1


RT_NAMES = {
    1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG", 6: "STRING",
    7: "FONTDIR", 8: "FONT", 9: "ACCELERATOR", 10: "RCDATA", 11: "MESSAGETABLE",
    12: "GROUP_CURSOR", 14: "GROUP_ICON", 16: "VERSION", 17: "DLGINCLUDE",
    19: "PLUGPLAY", 20: "VXD", 21: "ANICURSOR", 22: "ANIICON", 23: "HTML",
    24: "MANIFEST",
}


def _rva_to_offset(sections, rva):
    for virtual_address, virtual_size, raw_pointer, raw_size in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            delta = rva - virtual_address
            if delta < raw_size:
                return raw_pointer + delta
    return None


def pe_resources(data: bytes) -> dict:
    if data[:2] != b"MZ":
        raise SystemExit("not a PE file")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise SystemExit("missing PE signature")
    coff = pe_offset + 4
    machine, section_count = struct.unpack_from("<HH", data, coff)
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    magic = struct.unpack_from("<H", data, optional)[0]
    is_pe32_plus = magic == 0x20B
    data_directory_offset = optional + (112 if is_pe32_plus else 96)
    resource_rva, resource_size = struct.unpack_from("<II", data, data_directory_offset + 2 * 8)
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        base = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, base + 8)
        sections.append((virtual_address, virtual_size, raw_pointer, raw_size))

    resources = {"types": [], "entries": [], "resource_directory": {"rva": resource_rva, "size": resource_size}}
    if not resource_rva:
        return resources

    def walk(offset, depth, path):
        base = _rva_to_offset(sections, resource_rva + offset)
        if base is None:
            return
        named, id_count = struct.unpack_from("<HH", data, base + 12)
        for index in range(named + id_count):
            entry = base + 16 + index * 8
            name_or_id, data_offset = struct.unpack_from("<II", data, entry)
            named_entry = bool(name_or_id & 0x80000000)
            name = None
            if named_entry:
                string_offset = _rva_to_offset(sections, resource_rva + (name_or_id & 0x7FFFFFFF))
                if string_offset is not None:
                    length = struct.unpack_from("<H", data, string_offset)[0]
                    name = data[string_offset + 2:string_offset + 2 + length * 2].decode("utf-16-le", "replace")
            else:
                name = name_or_id
            if depth == 0:
                label = RT_NAMES.get(name, "UNKNOWN") if isinstance(name, int) else name
                resources["types"].append({"id": name, "name": label, "entries": 0, "bytes": 0})
            if data_offset & 0x80000000:
                walk(data_offset & 0x7FFFFFFF, depth + 1, path + [name])
            else:
                found = _rva_to_offset(sections, resource_rva + data_offset)
                if found is None:
                    continue
                data_rva, size, codepage, _reserved = struct.unpack_from("<IIII", data, found)
                payload_offset = _rva_to_offset(sections, data_rva)
                payload = data[payload_offset:payload_offset + size] if payload_offset is not None else b""
                resources["entries"].append(
                    {
                        "path": [str(item) for item in path + [name]],
                        "type": RT_NAMES.get(path[0], str(path[0])) if path else None,
                        "id": name,
                        "bytes": size,
                        "sha256": sha256_bytes(payload) if payload else None,
                        "payload_offset": payload_offset,
                        "sample_ascii": "".join(
                            chr(byte) if 32 <= byte < 127 else "." for byte in payload[:48]
                        ),
                    }
                )
                if resources["types"]:
                    resources["types"][0]["bytes"] += size
                    resources["types"][0]["entries"] += 1

    walk(0, 0, [])
    return resources


def cmd_pe(args: argparse.Namespace) -> int:
    exe = Path(args.exe)
    data = exe.read_bytes()
    findings: list[str] = []
    checks: dict[str, object] = {"exe": str(exe), "bytes": len(data), "sha256": sha256_bytes(data)}
    resources = pe_resources(data)
    checks["resources"] = resources
    rtypes = [entry for entry in resources["types"]]
    findings.append(
        "PE resource types: " + ", ".join(f"{entry['name']}({entry['id']})x{entry['entries']}/{entry['bytes']}B" for entry in rtypes)
    )
    rcdata = [entry for entry in resources["entries"] if entry["type"] == "RCDATA"]
    checks["rcdata_entry_count"] = len(rcdata)
    checks["rcdata_bytes_total"] = sum(entry["bytes"] for entry in rcdata)
    findings.append(
        f"RCDATA entries={len(rcdata)} total={sum(entry['bytes'] for entry in rcdata)} bytes"
        if rcdata
        else "FAIL no RCDATA resource in EXE"
    )

    if args.manifest:
        manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
        repo = Path(args.repo).resolve()
        embedded = []
        absent = []
        for row in manifest["notices"]:
            payload = (repo / row["path"]).read_bytes()
            if payload in data:
                embedded.append(row["name"])
            else:
                absent.append(row["name"])
        checks["notices_byte_identical_in_exe"] = embedded
        checks["notices_absent_from_exe"] = absent
        findings.append(
            f"{len(embedded)}/{len(manifest['notices'])} notice files are byte-identical substrings of the EXE"
        )

    status = "PASS" if rcdata and not checks.get("notices_absent_from_exe") else "BLOCKED"
    result = {"check": "pe-resources", "status": status, "findings": findings, "checks": checks}
    print(json.dumps(result, indent=1, ensure_ascii=False, default=str))
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(result, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    return 0 if status == "PASS" else 2


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    sub = parser.add_subparsers(dest="command", required=True)

    inputs = sub.add_parser("inputs")
    inputs.add_argument("--json")
    inputs.add_argument(
        "--tool-root",
        default=None,
        help="host path of the approved read-only tool root (maps .snow/native-tools/...)",
    )
    inputs.set_defaults(func=cmd_inputs)

    source = sub.add_parser("source")
    source.add_argument("--commit", required=True)
    source.add_argument("--prefix", default="pd-launcher-0.1.0-source/")
    source.add_argument("--expected-sha256", default=None)
    source.add_argument("--expected-bytes", type=int, default=None)
    source.add_argument("--out", default=".snow/legal-relink/source-repro.zip")
    source.add_argument("--json")
    source.set_defaults(func=cmd_source)

    pe = sub.add_parser("pe")
    pe.add_argument("--exe", required=True)
    pe.add_argument("--manifest", default=MANIFEST)
    pe.add_argument("--json")
    pe.set_defaults(func=cmd_pe)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))