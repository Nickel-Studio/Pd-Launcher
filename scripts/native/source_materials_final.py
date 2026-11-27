#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Bounded, read-only source/notice inventory and final material closure checks.

This tool never builds, downloads, packages, relinks, stages files, or grants a
release. A classified Git tree is a source candidate, not compiler consumption.
"""
from __future__ import annotations

import argparse

from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
MIB = 1024 * 1024
BATCH_LIMIT = 256 * MIB
OUTPUT_LIMIT = 16 * MIB
SHA = re.compile(r"^[0-9a-f]{64}$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
BUILD_OWNER = "native-final-build/49dfbdc0"
SOURCE_OWNER = "native-source-materials-final/e2562d48"
FACTORY_OWNER = "native-factory-finish/dda58fa0"
RELEASE_OWNER = "release-final-evidence/82c9aaae"
FALSE_READINESS = {"dependencySnapshotValidationPassed": False,
                   "nativeLegalMaterialValidationPassed": False,
                   "nativeReleaseReady": False}
GRAPH_FILES = ("CMakeLists.txt", "CMakePresets.json", "native/metadata.json",
               "cmake/NickelDependencies.cmake", "cmake/NickelDomains.cmake",
               "cmake/NickelQtDependencies.cmake", "cmake/NickelStaticDependencies.cmake",
               "native/app/CMakeLists.txt", "scripts/native/deps-lock.json")
GENERATED_PARTS = {".snow", "artifacts", "build", "CMakeFiles", "node_modules", "target",
                   "__pycache__", "history", "scratch", "testsScratch6"}
OUTPUT_SUFFIXES = {".exe", ".obj", ".lib", ".dll", ".pdb", ".map", ".log", ".pyc",
                   ".ninja", ".a", ".so", ".dylib"}


class EvidenceError(ValueError):
    def __init__(self, message: str, *, code: str = "invalid-evidence", owner: str = BUILD_OWNER):
        super().__init__(message)
        self.code, self.owner = code, owner


def canonical_bytes(value) -> bytes:
    return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":"),
                      allow_nan=False).encode("ascii")


def digest(value) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def relative_name(value: str) -> str:
    if not isinstance(value, str) or not value or value != value.strip() or "\\" in value:
        raise EvidenceError("Paths must be exact, normalized checkout-relative POSIX strings")
    parts = PurePosixPath(value)
    if (parts.is_absolute() or str(parts) != value or any(p in {".", ".."} for p in parts.parts)
            or any(c in value for c in "\x00\r\n:") or not parts.parts):
        raise EvidenceError(f"Unsafe material/member path: {value!r}")
    return value


def inside(root: Path, name: str) -> Path:
    name = relative_name(name)
    cursor = root
    for part in PurePosixPath(name).parts:
        cursor = cursor / part
        try:
            info = cursor.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise EvidenceError(f"Symlink/reparse-point material is not followed: {name}")
    if not cursor.resolve().is_relative_to(root.resolve()):
        raise EvidenceError("Material escaped the owned root")
    return cursor


def capacity(root: Path) -> dict:
    if os.name == "nt":
        free = {drive: shutil.disk_usage(drive + ":/").free for drive in ("C", "D")}
        required = {"C": 1024 * MIB, "D": 256 * MIB}
    else:
        free, required = {"worktree": shutil.disk_usage(root).free}, {"worktree": 256 * MIB}
    return {"measuredAt": datetime.now(timezone.utc).isoformat(), "freeBytes": free,
            "requiredReserveBytes": required,
            "reserveSatisfied": all(free[key] >= value for key, value in required.items())}


def write_new(root: Path, name: str, value) -> dict:
    path = inside(root, name)
    data = json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2, allow_nan=False).encode("ascii") + b"\n"
    measured = capacity(root)
    if not measured["reserveSatisfied"] or len(data) > OUTPUT_LIMIT:
        raise EvidenceError("Capacity/output budget gate rejected the small new receipt", code="capacity-gate")
    if path.exists():
        raise EvidenceError("Receipt already exists; preserve it and choose a fresh exact path")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)
    return {"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class Reader:
    """Hash exact files with bounded buffers; never enumerate a generated directory."""
    def __init__(self, root: Path, limit: int = BATCH_LIMIT):
        self.root, self.limit, self.read_bytes = root, limit, 0
        self.cache: dict[str, dict] = {}

    def hash_stream(self, stream, length: int) -> str:
        if type(length) is not int or length < 0 or self.read_bytes + length > self.limit:
            raise EvidenceError("Read batch exceeds 256 MiB; builder must supply a smaller closed batch",
                                code="read-budget-exceeded")
        result, received = hashlib.sha256(), 0
        while received < length:
            chunk = stream.read(min(256 * 1024, length - received))
            if not chunk:
                raise EvidenceError("Truncated actual file/blob")
            result.update(chunk)
            received += len(chunk)
        self.read_bytes += received
        return result.hexdigest()

    def file(self, row: dict, *, nonempty: bool = True) -> Path:
        if (not isinstance(row, dict) or not SHA.fullmatch(str(row.get("sha256", "")))
                or type(row.get("bytes")) is not int or row["bytes"] < int(nonempty)):
            raise EvidenceError("Every actual material needs exact path, full SHA-256 and integer bytes")
        path = inside(self.root, row.get("path"))
        if not path.is_file():
            raise EvidenceError(f"Not supplied in this authorized root: {row['path']}", code="missing-material")
        before = path.stat()
        if before.st_size != row["bytes"]:
            raise EvidenceError(f"Material byte-length drift: {row['path']}")
        cached = self.cache.get(row["path"])
        identity = (before.st_size, before.st_mtime_ns, before.st_ino)
        if cached and cached["identity"] == identity:
            actual = cached["sha256"]
        else:
            with path.open("rb") as stream:
                actual = self.hash_stream(stream, before.st_size)
            after = path.stat()
            if identity != (after.st_size, after.st_mtime_ns, after.st_ino):
                raise EvidenceError(f"Material changed during hashing: {row['path']}")
            self.cache[row["path"]] = {"identity": identity, "sha256": actual}
        if actual != row["sha256"]:
            raise EvidenceError(f"Material SHA-256 drift: {row['path']}")
        return path

    def json(self, row: dict) -> dict:
        path = self.file(row)
        if row["bytes"] > OUTPUT_LIMIT:
            raise EvidenceError("JSON metadata exceeds the small-tool quota")
        value = json.loads(path.read_bytes())
        if not isinstance(value, dict):
            raise EvidenceError("Evidence document must be a JSON object")
        return value


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=root, stderr=subprocess.PIPE)


def git_tree(root: Path, commit: str) -> tuple[str, list[dict]]:
    if not COMMIT.fullmatch(commit):
        raise EvidenceError("Use a full immutable lowercase commit", owner=SOURCE_OWNER)
    if git(root, "rev-parse", commit + "^{commit}").decode().strip() != commit:
        raise EvidenceError("Commit did not resolve exactly")
    tree = git(root, "rev-parse", commit + "^{tree}").decode().strip()
    rows = []
    for entry in git(root, "ls-tree", "-r", "-z", "--long", commit).split(b"\0"):
        if entry:
            header, raw_name = entry.split(b"\t", 1)
            mode, kind, obj, size = header.decode("ascii").split()
            name = relative_name(raw_name.decode("utf-8"))
            rows.append({"path": name, "mode": mode, "kind": kind, "gitObject": obj,
                         "bytes": int(size) if size != "-" else None})
    if not rows:
        raise EvidenceError("Empty Git tree is not launcher source")
    if len({row["path"].casefold() for row in rows}) != len(rows):
        raise EvidenceError("Git tree has case-colliding material paths")
    return tree, sorted(rows, key=lambda row: row["path"])


def git_blobs(root: Path, rows: list[dict], reader: Reader) -> list[dict]:
    results = []
    process = subprocess.Popen(["git", "cat-file", "--batch"], cwd=root,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for row in rows:
            if row["kind"] != "blob" or row["mode"] not in {"100644", "100755"}:
                raise EvidenceError("Nonregular Git source requires an explicit material contract")
            process.stdin.write((row["gitObject"] + "\n").encode("ascii"))
            process.stdin.flush()
            header = process.stdout.readline().decode("ascii").strip().split()
            if header != [row["gitObject"], "blob", str(row["bytes"])]:
                raise EvidenceError("Git inventory/blob header mismatch")
            actual = reader.hash_stream(process.stdout, row["bytes"])
            if process.stdout.read(1) != b"\n":
                raise EvidenceError("Truncated Git blob delimiter")
            results.append({**row, "sha256": actual})
    finally:
        process.stdin.close()
        process.stdout.close()
        try:
            result = process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=15)
            raise EvidenceError("Owned git cat-file process did not finish")
    if result:
        raise EvidenceError("Git blob reader failed")
    return results


def classification(row: dict) -> tuple[str, str]:
    name, parts = row["path"], PurePosixPath(row["path"])
    if row["kind"] != "blob" or row["mode"] not in {"100644", "100755"}:
        return "requires-contract", "nonregular/submodule input cannot silently be omitted"
    if any(part in GENERATED_PARTS or part.startswith((".build", ".scratch")) for part in parts.parts):
        return "excluded", "generated or preserved historical evidence, not preferred production source"
    if parts.suffix.lower() in OUTPUT_SUFFIXES or parts.name in {"CMakeCache.txt", "compile_commands.json", "install_manifest.txt"}:
        return "excluded", "binary/build observation; final corresponding code is inventoried separately"
    if parts.suffix.lower() in {".zip", ".7z", ".gz", ".xz"}:
        return "excluded", "archive material; validate separately rather than claim it as launcher source"
    return "preferred-source-candidate", "regular tracked preferred source, asset, license or build/configuration input"


def capture_coverage(rows: list[dict], captured: dict) -> dict:
    files = captured.get("committedFiles", captured.get("files"))
    excluded = captured.get("excludedFiles", captured.get("excluded", []))
    if not isinstance(files, list) or not isinstance(excluded, list):
        raise EvidenceError("Capture must contain actual files and an explicit exclusion list")
    def names(values):
        answer = [relative_name(row if isinstance(row, str) else row.get("path")) for row in values]
        if len(answer) != len(set(answer)):
            raise EvidenceError("Duplicate capture/exclusion paths")
        return set(answer)
    present, removed = names(files), names(excluded)
    tree = {row["path"]: row for row in rows}
    if present & removed or (present | removed) - set(tree):
        raise EvidenceError("Capture has overlapping paths or paths outside its exact Git tree")
    for row in files:
        if not isinstance(row, dict):
            raise EvidenceError("Captured files need actual Git object/byte records")
        expected = tree[row["path"]]
        for key in ("mode", "gitObject", "bytes"):
            if key in row and row[key] != expected[key]:
                raise EvidenceError(f"Captured Git identity drift: {row['path']}")
    unlisted = sorted(set(tree) - present - removed)
    return {"treeFiles": len(rows), "capturedFiles": len(present), "excludedFiles": len(removed),
            "unlistedFiles": len(unlisted), "unlistedPaths": unlisted,
            "classificationClosed": not unlisted,
            "actualCompilerConsumptionProved": False, "finalCorrespondingSourceProved": False}


def inventory(root: Path, commit: str, capture: dict | None = None) -> dict:
    tree, rows = git_tree(root, commit)
    selected, excluded, unresolved = [], [], []
    for row in rows:
        group, reason = classification(row)
        record = {**row, "classification": group, "reason": reason}
        {"preferred-source-candidate": selected, "excluded": excluded, "requires-contract": unresolved}[group].append(record)
    reader = Reader(root)
    sources = git_blobs(root, selected, reader)
    result = {"schemaVersion": 1, "kind": "palladium-classified-git-source-candidate-not-final-source",
              "sourceCommit": commit, "sourceTree": tree, "treeFileCount": len(rows),
              "treeInventorySha256": digest(rows), "preferredSourceCandidate": sources,
              "preferredSourceCount": len(sources), "preferredSourceBytes": sum(row["bytes"] for row in sources),
              "preferredSourceSha256": digest(sources), "excludedFiles": excluded,
              "requiresContract": unresolved, "classificationClosed": len(sources) + len(excluded) + len(unresolved) == len(rows),
              "sourceCaptureIsFinalSource": False, "actualBuildInputsComplete": False,
              "readBytes": reader.read_bytes, "batchLimitBytes": BATCH_LIMIT, **FALSE_READINESS}
    if capture is not None:
        if capture.get("sourceCommit") != commit or capture.get("sourceTree") != tree:
            raise EvidenceError("Capture is from another immutable source generation")
        result["captureCoverage"] = capture_coverage(rows, capture)
    return result


def notice_inventory(root: Path, commit: str, name: str) -> dict:
    _, rows = git_tree(root, commit)
    by_name = {row["path"]: row for row in rows}
    name = relative_name(name)
    if name not in by_name or by_name[name]["bytes"] > OUTPUT_LIMIT:
        raise EvidenceError("Notice manifest missing or exceeds metadata quota")
    raw = git(root, "show", commit + ":" + name)
    document = json.loads(raw)
    notices = document.get("notices")
    if not isinstance(notices, list) or not notices:
        raise EvidenceError("Notice input has no original material rows")
    selected, output_names, source_names = [], set(), set()
    for row in notices:
        source, output = relative_name(row.get("path")), relative_name(row.get("name"))
        if output.casefold() in output_names or source in source_names or source not in by_name:
            raise EvidenceError("Notice collision or missing immutable original blob")
        output_names.add(output.casefold())
        source_names.add(source)
        original = by_name[source]
        if row.get("bytes") != original["bytes"] or not SHA.fullmatch(str(row.get("sha256", ""))):
            raise EvidenceError("Notice candidate byte/hash metadata invalid")
        selected.append(original)
    reader = Reader(root)
    actual = {row["path"]: row for row in git_blobs(root, selected, reader)}
    material_rows = []
    for row in notices:
        if actual[row["path"]]["sha256"] != row["sha256"]:
            raise EvidenceError(f"Original notice hash drift: {row['path']}")
        material_rows.append({"name": row["name"], "sha256": row["sha256"], "bytes": row["bytes"]})
    material_rows.sort(key=lambda row: row["name"])
    actual_set = digest(material_rows)
    if document.get("notice_set_sha256", actual_set) != actual_set:
        raise EvidenceError("Notice set digest does not match original payloads")
    readiness_keys = ("trusted_launcher_ready", "nativeLegalMaterialValidationPassed", "nativeReleaseReady")
    return {"schemaVersion": 1, "kind": "immutable-original-notice-byte-check-not-release",
            "commit": commit, "manifest": {"path": name, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)},
            "noticeCount": len(notices), "noticeBytes": sum(row["bytes"] for row in notices),
            "noticeSetSha256": actual_set, "originalTextHashesVerified": True,
            "originalTextsModified": False,
            "candidateReadiness": {key: document.get(key) for key in readiness_keys},
            "compiledNoticeStampVerified": False, "finalSourceBindingVerified": False, **FALSE_READINESS}


def archive_members(path: Path, reader: Reader) -> list[dict]:
    """No extraction. Enforce the existing release member inventory contract/quota."""
    # Reuse the release owner's archive adapter, but preflight a smaller read batch.
    sys.path.insert(0, str(ROOT / "tests/native-release"))
    import archive_inventory
    total, count = 0, 0
    if zipfile.is_zipfile(path):
        kind = "zip"
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            total, count = sum(row.file_size for row in members), len(members)
    elif tarfile.is_tarfile(path):
        kind = "tar"
        with tarfile.open(path, "r|*") as archive:
            for member in archive:
                total += member.size
                count += 1
                if total + reader.read_bytes > reader.limit or count > archive_inventory.MAX_MEMBERS:
                    raise EvidenceError("Source archive exceeds the bounded read/member batch")
    else:
        raise EvidenceError("Actual corresponding source must be a readable ZIP/TAR, not a promise")
    if not count or count > archive_inventory.MAX_MEMBERS or total + reader.read_bytes > reader.limit:
        raise EvidenceError("Source archive exceeds the bounded read/member batch")
    try:
        members = archive_inventory.inventory(path, kind)
    except (ValueError, OSError, zipfile.BadZipFile, tarfile.TarError) as error:
        raise EvidenceError(str(error)) from error
    reader.read_bytes += total
    return [{"path": row["distributionPath"], "bytes": row["bytes"], "sha256": row["sha256"]} for row in members]


def source_archive_closure(required: list[dict], members: list[dict]) -> dict:
    by_name = {row["path"]: row for row in members}
    if len(by_name) != len(members) or not required:
        raise EvidenceError("Source closure needs a nonempty unique archive and source list")
    missing, drift = [], []
    for row in required:
        name = relative_name(row.get("sourceArchivePath", row["path"]))
        actual = by_name.get(name)
        if actual is None:
            missing.append(name)
        elif any(actual[key] != row[key] for key in ("bytes", "sha256")):
            drift.append(name)
    return {"requiredFiles": len(required), "archiveFiles": len(members), "missingPaths": sorted(missing),
            "driftPaths": sorted(drift), "closed": not missing and not drift}


def command_path(value: str, cwd: str = "") -> str:
    if not isinstance(value, str) or not value.strip() or any(c in value for c in "\x00\r\n"):
        raise EvidenceError("Command provenance path must be concrete")
    value = value.strip().strip('"').replace("\\", "/")
    windows = bool(re.match(r"^[A-Za-z]:/", value)) or value.startswith("//")
    if cwd and not windows and not value.startswith("/"):
        value = cwd.replace("\\", "/").rstrip("/") + "/" + value
        windows = bool(re.match(r"^[A-Za-z]:/", value)) or value.startswith("//")
    value = posixpath.normpath(value)
    return value.casefold() if windows else value


def link_inputs(reader: Reader, log: dict, responses: list[dict]) -> set[str]:
    argv, cwd = log.get("argv"), log.get("workingDirectory", "")
    if not isinstance(argv, list) or not argv or any(not isinstance(arg, str) or not arg.strip() for arg in argv) or not isinstance(cwd, str):
        raise EvidenceError("Actual link log needs string-array argv and concrete workingDirectory")
    registered = {}
    for row in responses:
        key = command_path(row.get("commandPath"), cwd)
        if key in registered:
            raise EvidenceError("Duplicate response command path")
        registered[key] = reader.file(row)
    pending, seen, inputs = list(argv[1:]), set(), set()
    while pending:
        arg = pending.pop(0).strip().strip('"')
        if arg.startswith("-Wl,"):
            pending.extend(arg[4:].split(","))
        elif arg.startswith("@"):
            key = command_path(arg[1:], cwd)
            if key not in registered or key in seen:
                raise EvidenceError("Missing, repeated or cyclic actual response input")
            seen.add(key)
            data = registered[key].read_bytes()
            if len(data) > OUTPUT_LIMIT:
                raise EvidenceError("Response metadata over quota")
            encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
            pending.extend(shlex.split(data.decode(encoding), posix=False))
        else:
            if arg.lower().startswith("/wholearchive:"):
                arg = arg[len("/wholearchive:"):].strip('"')
            if re.search(r"(?i)\.(?:obj|o|lib|a|dll|so|dylib)$", arg):
                inputs.add(command_path(arg, cwd))
    if seen != set(registered):
        raise EvidenceError("Unconsumed response file cannot be evidence")
    return inputs


def load_legal_tool():
    name = ROOT / "scripts/native/brand-legal.py"
    spec = importlib.util.spec_from_file_location("pd_existing_legal_materials", name)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except ImportError as error:
        raise EvidenceError("Existing legal verifier dependencies unavailable: " + str(error),
                            code="missing-verifier-dependency", owner=SOURCE_OWNER) from error
    return module


def verify(root: Path, request: dict) -> dict:
    reader, checks, gaps = Reader(root), {}, []
    def check(label, owner, action):
        try:
            value = action()
            checks[label] = {"status": "passed", "evidence": value}
            return value
        except (EvidenceError, ValueError, OSError, KeyError, TypeError, ImportError) as error:
            checks[label] = {"status": "missing" if getattr(error, "code", "") == "missing-material" else "failed"}
            gaps.append({"id": label, "owner": getattr(error, "owner", owner),
                         "code": getattr(error, "code", "invalid-or-missing-evidence"), "required": str(error)})
            return None
    if request.get("schemaVersion") != 1 or request.get("kind") != "palladium-final-source-materials-request":
        raise EvidenceError("Not a final source-material request")
    commit, tree = request.get("sourceCommit"), request.get("sourceTree")
    actual_tree = check("immutable-source-generation", SOURCE_OWNER, lambda: git_tree(root, commit)[0])
    if actual_tree and actual_tree != tree:
        gaps.append({"id": "source-tree-drift", "owner": SOURCE_OWNER, "required": "Request sourceTree differs from its immutable Git commit"})
    candidate = check("complete-launcher-source-inventory", SOURCE_OWNER,
                      lambda: reader.json(request["launcherSourceInventory"]))
    if candidate is not None:
        expected = check("source-inventory-independent-reconstruction", SOURCE_OWNER, lambda: inventory(root, commit))
        if expected is not None and any(candidate.get(key) != expected[key] for key in
                ("sourceCommit", "sourceTree", "treeInventorySha256", "preferredSourceCandidate", "excludedFiles", "requiresContract")):
            gaps.append({"id": "source-inventory-drift", "owner": SOURCE_OWNER, "required": "Do not omit source paths or alter a classified immutable tree"})
    consumed = check("actual-compiler-generated-resource-consumption", BUILD_OWNER,
                     lambda: reader.json(request["buildInputConsumption"]))
    build_files = []
    if consumed is not None:
        if (consumed.get("kind") != "actual-native-build-input-consumption" or
                consumed.get("origin") != "actual-build-commands-depfiles-and-resource-compilation" or
                consumed.get("sourceCommit") != commit or consumed.get("sourceTree") != tree or
                any(consumed.get(key) is not True for key in
                    ("compilerDependencyClosureComplete", "generatedInputsComplete", "resourceInputsComplete"))):
            gaps.append({"id": "actual-consumption-not-closed", "owner": BUILD_OWNER,
                         "required": "Capture/listing is not same-generation complete commands/depfiles/generated/resource evidence"})
        build_files = consumed.get("files", [])
        def consumed_files():
            if not isinstance(build_files, list) or not build_files or not consumed.get("rawEvidence"):
                raise EvidenceError("Supply nonempty actual consumed source files and raw evidence")
            roles = {row.get("role") for row in build_files}
            if {"compiled-source", "included-header", "generated-input", "resource-input", "build-configuration"} - roles:
                raise EvidenceError("Consumed inputs omit an entire compiler/header/generator/resource/configuration class")
            for row in build_files + consumed["rawEvidence"]:
                reader.file(row, nonempty=False)
            if consumed.get("buildSourceSha256") != digest(build_files):
                raise EvidenceError("Actual buildSourceSha256 must hash the complete sorted input records")
            return {"fileCount": len(build_files), "buildSourceSha256": digest(build_files)}
        check("actual-consumed-file-hashes", BUILD_OWNER, consumed_files)
    archive = check("actual-corresponding-source-archive", BUILD_OWNER,
                    lambda: archive_members(reader.file(request["correspondingSourceArchive"]), reader))
    if archive is not None and candidate is not None:
        closure = check("launcher-preferred-source-and-build-input-closure", SOURCE_OWNER,
                        lambda: source_archive_closure(candidate["preferredSourceCandidate"] + build_files, archive))
        if closure and (not closure["closed"] or candidate.get("requiresContract")):
            gaps.append({"id": "corresponding-source-not-closed", "owner": BUILD_OWNER,
                         "required": "Archive must reproduce every launcher preferred source and actual consumed generated/resource/config input",
                         "missingPaths": closure["missingPaths"], "driftPaths": closure["driftPaths"]})
    legal = check("actual-native-release-input", BUILD_OWNER, lambda: reader.json(request["legalInput"]))
    if legal is not None:
        if legal.get("scope") != "native-release" or any(legal.get(key) != request.get(key) for key in ("sourceCommit", "sourceTree", "releaseId")):
            gaps.append({"id": "legal-generation-binding", "owner": BUILD_OWNER,
                         "required": "No asset-only/history input; legal input must bind the identical final source generation/release"})
        def existing_legal():
            module = load_legal_tool()
            errors = module.validate_input(legal, root)
            if errors:
                raise EvidenceError("Existing brand legal verifier rejected actual source/staticlib/relink: " + "; ".join(errors))
            return {"tool": "scripts/native/brand-legal.py", "actualReplacementRelinkVerified": True}
        check("existing-legal-source-staticlib-replacement-link", BUILD_OWNER, existing_legal)
    link = check("actual-link-command", BUILD_OWNER, lambda: reader.json(request["linkCommand"]))
    linked = None
    if link is not None:
        def actual_link():
            executable = request["executable"]
            reader.file(executable)
            if (type(link.get("exitCode")) is not int or link["exitCode"] != 0 or
                    link.get("outputSha256") != executable["sha256"] or
                    (consumed is not None and consumed.get("executableSha256") != executable["sha256"])):
                raise EvidenceError("Actual successful link, consumption and final executable SHA differ")
            inputs = link_inputs(reader, link, request.get("responseFiles", []))
            records = request["linkInputs"]
            declared = set()
            for row in records:
                key = command_path(row["commandPath"], link.get("workingDirectory", ""))
                if key in declared or not row.get("componentId") or not row.get("owner"):
                    raise EvidenceError("Each link input requires unique path, component and object/library owner")
                declared.add(key)
                reader.file(row)
            if not records or inputs != declared:
                raise EvidenceError("Actual response/command link input set is not exactly covered by object/static/system records")
            return {"inputCount": len(records), "componentIds": sorted({row["componentId"] for row in records}),
                    "inputsSha256": digest(records)}
        linked = check("exact-actual-object-library-link-closure", BUILD_OWNER, actual_link)
    installed = check("actual-installed-library-inventory", BUILD_OWNER, lambda: reader.json(request["installedInventory"]))
    if installed is not None:
        def actual_installed():
            rows = installed.get("libraries")
            if not isinstance(rows, list) or not rows or len({row["path"] for row in rows}) != len(rows):
                raise EvidenceError("Supply the actual installed library/plugin path inventory, not an inferred 40-count")
            for row in rows:
                reader.file(row)
            return {"installedLibraryCount": len(rows), "installedInventorySha256": digest(rows),
                    "installedIsNotFinalUsed": True}
        check("installed-library-plugin-hashes", BUILD_OWNER, actual_installed)
    check("actual-link-map-and-used-members", BUILD_OWNER, lambda: reader.file(request["linkMap"]).as_posix())
    def objects():
        rows = request["objects"]
        if not rows:
            raise EvidenceError("Supply actual application AND generated Qt resource objects with source ownership")
        for row in rows:
            reader.file(row)
            if not row.get("sourcePaths") or row.get("role") not in {"application-object", "generated-resource-object", "dependency-initializer-object"}:
                raise EvidenceError("Dependency initializer objects cannot masquerade as application code")
        if not {"application-object", "generated-resource-object"}.issubset({row["role"] for row in rows}):
            raise EvidenceError("Corresponding Application Code must include application and generated resource objects")
        return {"objectCount": len(rows), "objectOwnershipSha256": digest(rows)}
    check("actual-application-object-resource-ownership", BUILD_OWNER, objects)
    def sbom():
        row = request["sbom"]
        path = reader.file(row)
        module = load_legal_tool()
        errors = module.validate_spdx(path)
        if errors:
            raise EvidenceError("Independent SPDX validation: " + "; ".join(errors))
        if linked is None:
            raise EvidenceError("Actual used dependency set unavailable; installed SPDX is not final EXE SBOM")
        return {"spdxValidationPassed": True, "sha256": row["sha256"]}
    check("actual-final-used-sbom", BUILD_OWNER, sbom)
    check("msvc-runtime-system-exception-material", BUILD_OWNER,
          lambda: reader.json(request["runtimeException"]))
    check("compiled-notice-and-source-stamp", FACTORY_OWNER,
          lambda: reader.json(request["compiledStampEvidence"]))
    passed = not gaps
    return {"schemaVersion": 1, "kind": "palladium-final-source-materials-closure-receipt",
            "sourceCommit": commit, "sourceTree": tree, "releaseId": request.get("releaseId"),
            "checks": checks, "missingOrInvalidMaterials": gaps, "sourceMaterialsClosed": passed,
            "actualBuildInputsComplete": passed, "actualReplacementRelinkVerified": passed,
            "readBytes": reader.read_bytes, "batchLimitBytes": BATCH_LIMIT,
            "limits": ["This checks exact files/closure; it never runs a replacement link or authorizes a release.",
                       "Independent release/feature/platform/legal acceptance remains with release-final-evidence."],
            **FALSE_READINESS}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    source = subs.add_parser("inventory", help="Classify EVERY immutable Git tree path and hash preferred source candidates")
    source.add_argument("--commit", required=True)
    source.add_argument("--capture", help="Existing exact root-relative builder capture JSON")
    notices = subs.add_parser("notices", help="Hash every original notice blob without copying/modifying it")
    notices.add_argument("--commit", required=True)
    notices.add_argument("--manifest", required=True)
    final = subs.add_parser("verify", help="Check actual supplied final files; report missing material to its owner")
    final.add_argument("--request", required=True)
    for sub in (source, notices, final):
        sub.add_argument("--output", required=True, help="Fresh root-relative small JSON receipt")
    args = parser.parse_args()
    try:
        if args.command == "inventory":
            capture = json.loads(inside(ROOT, args.capture).read_bytes()) if args.capture else None
            result = inventory(ROOT, args.commit, capture)
        elif args.command == "notices":
            result = notice_inventory(ROOT, args.commit, args.manifest)
        else:
            request = json.loads(inside(ROOT, args.request).read_bytes())
            result = verify(ROOT, request)
        result["capacity"] = capacity(ROOT)
        output = write_new(ROOT, args.output, result)
        print(json.dumps({"output": output, "sourceMaterialsClosed": result.get("sourceMaterialsClosed", False),
                          **FALSE_READINESS}, sort_keys=True))
        return 2 if args.command == "verify" and not result["sourceMaterialsClosed"] else 0
    except (EvidenceError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(json.dumps({"error": str(error), "code": getattr(error, "code", "invalid-evidence"),
                          "owner": getattr(error, "owner", SOURCE_OWNER), **FALSE_READINESS}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
