"""Independent actual Pd install/ZIP deployment smoke. No builder or product edits."""
from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import hashlib
import importlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import stat
import subprocess
import sys
import uuid
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).absolute().parents[2]
SCOPE = ROOT / "tests/native-pd-delivery-smoke"
LOCAL = SCOPE / "_local"
MAX_SCOPE = 256 * 1024 * 1024
MAX_PACKAGE = 200 * 1024 * 1024
MAX_JSON = 16 * 1024 * 1024
MAX_FILES = 20000
C_RESERVE = 1024 * 1024 * 1024
D_RESERVE = 256 * 1024 * 1024
HISTORICAL_NI_SHA = "75fbb88641709662eac296dfedb8c395bdb3cb91bd5815270cce2ea5a41321f7"
SHA = re.compile(r"[0-9a-f]{64}\Z")
COMMIT = re.compile(r"[0-9a-f]{40}(?:[0-9a-f]{24})?\Z")
INSTALL_ROLES = {"application", "data-resource", "license-material", "embedded-notice"}


class SmokeError(ValueError):
    pass


class NotRun(SmokeError):
    pass


class CapacityError(NotRun):
    pass


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(256 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def canonical(value) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=True, allow_nan=False).encode("ascii")).hexdigest()


def safe_name(value: str) -> str:
    if (not isinstance(value, str) or not value or len(value) > 32700 or
            "\\" in value or PureWindowsPath(value).drive or value.startswith("/")):
        raise SmokeError("An exact canonical worktree-relative slash path is required")
    for part in value.split("/"):
        if (part in {"", ".", ".."} or part.endswith((".", " ")) or
                any(ord(char) < 32 or char in '<>:"|?*' for char in part) or
                re.fullmatch(r"(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", part, re.I)):
            raise SmokeError("Unsafe or Windows-ambiguous path: " + value)
    return value


def plain(path: Path, *, exists: bool = True) -> Path:
    path = path.absolute()
    for item in reversed([path, *path.parents]):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise SmokeError("Reparse/link ancestor is not a real-D evidence path: " + str(item))
    if exists and not path.exists():
        raise NotRun("Actual input is absent: " + str(path.relative_to(ROOT)))
    return path


def inside(value: str, root: Path = ROOT, *, exists: bool = True) -> Path:
    root = plain(root)
    path = plain(root / safe_name(value), exists=exists)
    if not path.is_relative_to(root):
        raise SmokeError("Evidence escaped its explicit worktree root")
    return path


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise SmokeError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def read_json(path: Path):
    plain(path)
    if not path.is_file() or path.stat().st_size > MAX_JSON:
        raise SmokeError("JSON input is not a bounded regular file")
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=_pairs,
                      parse_constant=lambda raw: (_ for _ in ()).throw(SmokeError("Nonfinite JSON: " + raw)))


def file_ref(path: Path) -> dict:
    path = plain(path)
    before = path.stat()
    result = {"path": path.relative_to(ROOT).as_posix(), "bytes": before.st_size, "sha256": digest(path)}
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise SmokeError("Input changed while hashing: " + result["path"])
    return result


def reference(row: dict, *, limit: int | None = None) -> Path:
    if not isinstance(row, dict) or not SHA.fullmatch(str(row.get("sha256", ""))):
        raise SmokeError("Input reference needs an exact lowercase SHA256")
    path = inside(row.get("path"))
    if not path.is_file() or type(row.get("bytes")) is not int or row["bytes"] < 1:
        raise SmokeError("Input reference needs actual positive regular-file bytes")
    if limit is not None and row["bytes"] > limit:
        raise SmokeError("Input exceeds this smoke's bounded read quota")
    actual = file_ref(path)
    if any(actual[key] != row[key] for key in ("path", "sha256", "bytes")):
        raise SmokeError("Input byte identity drift: " + row["path"])
    return path


def tree_files(directory: Path, *, hashes: bool = True, limit: int = MAX_SCOPE) -> list[dict]:
    plain(directory)
    if not directory.is_dir():
        raise SmokeError("Actual directory input is not a directory")
    result, pending, total, names = [], [directory], 0, set()
    while pending:
        base = pending.pop()
        plain(base)
        with os.scandir(base) as entries:
            for item in entries:
                path = plain(Path(item.path))
                name = safe_name(path.relative_to(directory).as_posix())
                folded = name.casefold()
                if folded in names or len(names) >= MAX_FILES:
                    raise SmokeError("Directory collision or bounded-entry overflow")
                names.add(folded)
                if item.is_dir(follow_symlinks=False):
                    pending.append(path)
                elif item.is_file(follow_symlinks=False):
                    info = path.stat()
                    if info.st_nlink != 1:
                        raise SmokeError("Hard-linked distribution material is not independent")
                    total += info.st_size
                    if total > limit:
                        raise SmokeError("Directory exceeds bounded scope/package byte quota")
                    row = {"distributionPath": name, "bytes": info.st_size}
                    if hashes:
                        row["sha256"] = file_ref(path)["sha256"]
                    result.append(row)
                else:
                    raise SmokeError("Nonregular distribution entry: " + name)
    return sorted(result, key=lambda row: row["distributionPath"])


def capacity(required_bytes: int = 0) -> dict:
    if os.name != "nt" or C.sizeof(C.c_void_p) != 8:
        raise NotRun("Actual Windows x64 host is required; host fixtures cannot pass deployment")
    if type(required_bytes) is not int or required_bytes < 0:
        raise SmokeError("Invalid storage reservation")
    plain(ROOT)
    if PureWindowsPath(str(ROOT)).drive.casefold() != "d:":
        raise CapacityError("Owned worktree must reside on actual D, not a mapped C junction")
    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.GetVolumePathNameW.argtypes = [W.LPCWSTR, W.LPWSTR, W.DWORD]
    kernel.GetVolumePathNameW.restype = W.BOOL
    kernel.GetVolumeNameForVolumeMountPointW.argtypes = [W.LPCWSTR, W.LPWSTR, W.DWORD]
    kernel.GetVolumeNameForVolumeMountPointW.restype = W.BOOL
    kernel.GetDiskFreeSpaceExW.argtypes = [W.LPCWSTR, C.POINTER(C.c_ulonglong),
                                         C.POINTER(C.c_ulonglong), C.POINTER(C.c_ulonglong)]
    kernel.GetDiskFreeSpaceExW.restype = W.BOOL
    mount, guid, actual_guid = (C.create_unicode_buffer(1024) for _ in range(3))
    if (not kernel.GetVolumePathNameW(str(ROOT), mount, 1024) or
            not kernel.GetVolumeNameForVolumeMountPointW(mount.value, actual_guid, 1024) or
            not kernel.GetVolumeNameForVolumeMountPointW("D:\\", guid, 1024)):
        raise CapacityError("Actual write-volume identity could not be measured")
    if mount.value.casefold() != "d:\\" or actual_guid.value != guid.value:
        raise CapacityError("Worktree resolves to a volume other than real D")
    drives = {}
    for drive, reserve in (("C", C_RESERVE), ("D", D_RESERVE)):
        available, total, free = C.c_ulonglong(), C.c_ulonglong(), C.c_ulonglong()
        if not kernel.GetDiskFreeSpaceExW(drive + ":\\", C.byref(available), C.byref(total), C.byref(free)):
            raise CapacityError("Fresh available capacity is unavailable for " + drive)
        need = reserve + (required_bytes if drive == "D" else 0)
        drives[drive] = {"availableBytes": available.value, "totalFreeBytes": free.value,
                         "reserveBytes": reserve, "requiredWithNextWriteBytes": need}
    result = {"capturedUtc": now(), "source": "GetDiskFreeSpaceExW.availableToCaller",
              "actualWriteMount": mount.value, "actualWriteVolume": actual_guid.value, "drives": drives}
    if any(row["availableBytes"] < row["requiredWithNextWriteBytes"] for row in drives.values()):
        raise CapacityError("Fresh C>=1GiB/D>=256MiB+next-write gate blocked: " + json.dumps(result))
    if SCOPE.exists():
        total = sum(row["bytes"] for row in tree_files(SCOPE, hashes=False))
        if total + required_bytes > MAX_SCOPE:
            raise CapacityError("Owned smoke scope would exceed 256MiB; no unknown/history file is deleted")
        result["ownedScopeBytes"] = total
    return result


def write_new(path: Path, data: bytes) -> None:
    plain(path, exists=False)
    if not path.is_relative_to(SCOPE):
        raise SmokeError("Output is outside the exclusive smoke scope")
    capacity(len(data))
    path.parent.mkdir(parents=True, exist_ok=True)
    plain(path.parent)
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def write_json(path: Path, value) -> None:
    write_new(path, (json.dumps(value, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("ascii"))


class Worker:
    def __init__(self):
        self.path = LOCAL / "active-worker.json"
        self.raw = (json.dumps({"token": uuid.uuid4().hex, "pid": os.getpid(), "createdUtc": now()}) + "\n").encode("ascii")
        self.keep = False

    def __enter__(self):
        if self.path.exists():
            raise NotRun("Single-worker lease already exists; never steal or delete another worker's lease")
        write_new(self.path, self.raw)
        return self

    def __exit__(self, *_):
        if not self.keep and plain(self.path).read_bytes() == self.raw:
            self.path.unlink()


def fresh_directory(label: str) -> Path:
    capacity(24 * 1024 * 1024)
    name = label + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:12]
    output = inside("tests/native-pd-delivery-smoke/_local/" + safe_name(name), exists=False)
    output.mkdir(parents=True, exist_ok=False)
    write_json(output / "owned-directory.json", {"kind": "test-owned-preserved-directory", "createdUtc": now(),
                                                "pid": os.getpid(), "automaticCleanup": False})
    return output


def load_dependencies() -> dict:
    lock = read_json(SCOPE / "dependency-lock.json")
    if lock.get("schemaVersion") != 1 or len(lock.get("files", [])) != 8:
        raise SmokeError("Exact existing acceptance/observer dependency lock is required")
    for row in lock["files"]:
        reference(row, limit=MAX_JSON)
    sys.path.insert(0, str(ROOT / "tests/native-release"))
    common = importlib.import_module("evidence_common")
    modules = {name: importlib.import_module(name) for name in
               ("windows_metrics", "release_evidence", "archive_inventory", "event_trace", "bounded_process")}
    spec = importlib.util.spec_from_file_location("pd_existing_acceptance", ROOT / "tests/native-acceptance/audit_windows.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    modules.update(acceptance=module, common=common)
    for row in lock["files"]:
        reference(row, limit=MAX_JSON)
    modules["lock"] = file_ref(SCOPE / "dependency-lock.json")
    modules["files"] = lock["files"]
    return modules


def git_read(*args: str) -> bytes:
    process = subprocess.run(["git", *args], cwd=ROOT, stdin=subprocess.DEVNULL,
                             capture_output=True, timeout=30, check=False)
    if process.returncode:
        raise SmokeError("Read-only Git identity check failed: " + process.stderr.decode("utf-8", errors="replace"))
    return process.stdout


def source_review(candidate: str | None = None) -> dict:
    head = git_read("rev-parse", "HEAD").decode("ascii").strip()
    main = git_read("rev-parse", "refs/heads/main").decode("ascii").strip()
    commit = candidate or main
    if (not COMMIT.fullmatch(commit) or
            git_read("rev-parse", "--verify", commit + "^{commit}").decode("ascii").strip() != commit):
        raise SmokeError("Source review requires an exact immutable commit")
    files, metadata = [], None
    for name in ("CMakeLists.txt", "native/metadata.json", "native/app/CMakeLists.txt", "native/app/main.cpp"):
        raw = git_read("show", commit + ":" + name)
        files.append({"path": name, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
                      "identityBasis": "Immutable Git blob bytes, not old worktree checkout bytes"})
        if name == "native/metadata.json":
            metadata = json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs)
    return {"checkoutHead": head, "mainRefAtReview": main, "reviewedCommit": commit,
            "files": files, "displayName": metadata.get("displayName"), "executableName": metadata.get("executableName"),
            "nativePdMetadata": metadata.get("displayName") == "Palladium" and metadata.get("executableName") == "pd-launcher",
            "claim": "Pinned source/packaging review only; production EXE requires separate actual byte receipt"}


def verify_snapshot(artifact: dict) -> dict:
    commit = artifact.get("sourceCommit")
    if not isinstance(commit, str) or not COMMIT.fullmatch(commit):
        raise SmokeError("Full actual production sourceCommit is required")
    if git_read("rev-parse", "--verify", commit + "^{commit}").decode("ascii").strip() != commit:
        raise SmokeError("Source commit did not resolve exactly")
    document = read_json(reference(artifact.get("sourceSnapshot"), limit=MAX_JSON))
    files = document.get("sourceFiles")
    if (document.get("kind") != "native-final-source-audit" or document.get("candidate") != commit or
            not isinstance(files, list) or not 1 <= len(files) <= MAX_FILES or
            canonical(files) != artifact.get("sourceSnapshotSha256") or
            document.get("sourceSnapshotSha256") != artifact.get("sourceSnapshotSha256")):
        raise SmokeError("Exact committed source snapshot does not bind this generation")
    entries = git_read("ls-tree", "-r", "-z", commit).split(b"\0")
    expected = []
    for entry in entries:
        if not entry:
            continue
        header, name = entry.split(b"\t", 1)
        mode, kind, object_id = header.decode("ascii").split(" ")
        if kind != "blob":
            raise SmokeError("Source snapshot contains unsupported Git submodule material")
        expected.append({"path": name.decode("utf-8"), "mode": mode, "gitObject": object_id})
    if len(files) != len(expected) or any({key: row.get(key) for key in expected[index]} != expected[index]
                                         for index, row in enumerate(files)):
        raise SmokeError("Snapshot omits or reorders actual immutable commit objects")
    reader = subprocess.Popen(["git", "cat-file", "--batch"], cwd=ROOT, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    total = 0
    try:
        for row in files:
            reader.stdin.write((row["gitObject"] + "\n").encode("ascii"))
            reader.stdin.flush()
            header = reader.stdout.readline(256).decode("ascii").split()
            if len(header) != 3 or header[:2] != [row["gitObject"], "blob"]:
                raise SmokeError("Immutable Git blob batch returned an unexpected identity")
            size = int(header[2])
            total += size
            if total > 512 * 1024 * 1024 or type(row.get("bytes")) is not int or size != row["bytes"]:
                raise SmokeError("Source snapshot bytes/quota differ from immutable commit")
            sha, remaining = hashlib.sha256(), size
            while remaining:
                chunk = reader.stdout.read(min(remaining, 256 * 1024))
                if not chunk:
                    raise SmokeError("Git blob stream ended before its declared length")
                sha.update(chunk)
                remaining -= len(chunk)
            if reader.stdout.read(1) != b"\n" or sha.hexdigest() != row.get("sha256"):
                raise SmokeError("Source snapshot hashes differ from immutable commit blobs")
        reader.stdin.close()
        if reader.wait(timeout=10):
            raise SmokeError("Immutable Git blob reader failed")
    finally:
        if reader.poll() is None:
            reader.terminate()
            reader.wait(timeout=5)
        reader.stdout.close()
        if not reader.stdin.closed:
            reader.stdin.close()
    return {"sourceCommit": commit, "sourceSnapshotSha256": canonical(files), "fileCount": len(files),
            "identityBasis": "Actual immutable Git blob bytes; physical build inputs remain distinct"}


def identity_contract(artifact: dict) -> None:
    wanted = {"schemaVersion": 1, "kind": "native-release", "scope": "full-launcher", "configuration": "Release",
              "platform": "windows", "architecture": "x64"}
    if not isinstance(artifact, dict) or any(artifact.get(key) != value for key, value in wanted.items()):
        raise SmokeError("Only the actual complete native Windows Release artifact is eligible")
    if artifact.get("product") not in {"Palladium", "pd-launcher"}:
        raise SmokeError("Historical Nickel/component/fixture metadata cannot produce a new Pd PASS")
    executable = artifact.get("executable", {})
    if executable.get("sha256") == HISTORICAL_NI_SHA:
        raise SmokeError("Explicitly historical Ni bytes are forbidden for new Pd acceptance, even if renamed")
    if PureWindowsPath(str(executable.get("path", ""))).name != "pd-launcher.exe":
        raise SmokeError("New Pd executable must be precisely pd-launcher.exe")
    if not artifact.get("releaseId") or not artifact.get("version"):
        raise SmokeError("Actual release generation and version are required")
    deployment = artifact.get("deployment", {})
    if any(deployment.get(key) != value for key, value in
           {"windowsSingleExe": True, "selfExtractsRuntime": False, "fileSetComplete": True}.items()):
        raise SmokeError("Windows deployment must be an actual static-Qt nonextracting complete file set")
    if not isinstance(deployment.get("externalRequirements"), list):
        raise NotRun("Explicit system/graphics-driver requirements are missing")
    for row in deployment["externalRequirements"]:
        if row.get("kind") not in {"system", "graphics-driver"} or not row.get("name") or not row.get("minimum"):
            raise SmokeError("Installed Rust/Node/Qt/development/shared runtimes cannot satisfy clean deployment")


def verify_build(artifact: dict, dependencies: dict) -> dict:
    result = dependencies["release_evidence"].verify_build_inputs(ROOT, artifact)
    expected = {"sourceCommit": artifact["sourceCommit"], "configuration": "Release",
                "executableSha256": artifact["executable"]["sha256"]}
    provenance = read_json(reference(artifact.get("releaseProvenance"), limit=MAX_JSON))
    if (provenance.get("kind") != "actual-release-provenance" or provenance.get("origin") != "actual-source-build" or
            any(provenance.get(key) != value for key, value in expected.items()) or not provenance.get("rawEvidence")):
        raise SmokeError("Actual build receipt/provenance does not identify the same final EXE/source")
    for row in provenance["rawEvidence"]:
        reference(row)
    for role in ("build-log", "link-command", "link-map", "dependency-inventory"):
        reference(artifact.get("buildEvidence", {}).get(role))
    inventory = read_json(reference(artifact["buildSourceInventory"], limit=MAX_JSON))
    metadata = [row for row in inventory["files"] if row["sourceArchivePath"] == "native/metadata.json"]
    if len(metadata) != 1:
        raise NotRun("Actual consumed Pd metadata input has not been identified unambiguously")
    value = read_json(reference(metadata[0], limit=MAX_JSON))
    if (value.get("displayName") != "Palladium" or value.get("executableName") != "pd-launcher" or
            value.get("version") != artifact["version"]):
        raise SmokeError("Compiled metadata is old Ni or differs from the release generation")
    resources = [row for row in inventory["files"] if row["role"] == "resource-input"]
    result["resourceInputsSha256"] = canonical(resources)
    result["resourceInputCount"] = len(resources)
    return result


def zip_records(path: Path) -> list[dict]:
    records, names, total = [], set(), 0
    with zipfile.ZipFile(path) as archive:
        if len(archive.infolist()) > MAX_FILES:
            raise SmokeError("ZIP member quota exceeded")
        for item in archive.infolist():
            name = safe_name(item.filename[:-1] if item.is_dir() else item.filename)
            if name.casefold() in names:
                raise SmokeError("ZIP duplicate or Windows case collision")
            names.add(name.casefold())
            mode = item.external_attr >> 16
            allowed = {0, stat.S_IFDIR} if item.is_dir() else {0, stat.S_IFREG}
            if stat.S_IFMT(mode) not in allowed or item.flag_bits & 1:
                raise SmokeError("Linked/nonregular/encrypted ZIP input is rejected")
            if item.is_dir():
                continue
            total += item.file_size
            if total > MAX_PACKAGE:
                raise SmokeError("ZIP expands beyond the bounded deployment budget")
            result, count = hashlib.sha256(), 0
            with archive.open(item) as stream:
                for chunk in iter(lambda: stream.read(256 * 1024), b""):
                    count += len(chunk)
                    if count > item.file_size:
                        raise SmokeError("ZIP member exceeds declared bytes")
                    result.update(chunk)
            if count != item.file_size:
                raise SmokeError("ZIP member is truncated")
            records.append({"distributionPath": name, "bytes": count, "sha256": result.hexdigest()})
    return sorted(records, key=lambda row: row["distributionPath"])


def package_contract(artifact: dict) -> tuple[Path, Path, list[dict]]:
    install = artifact.get("install")
    if not isinstance(install, dict) or not isinstance(install.get("files"), list) or not install["files"]:
        raise NotRun("Actual cmake --install directory plus complete installed-file inventory is missing")
    path = inside(install.get("path"))
    expected, names, applications, licenses = [], set(), [], 0
    for row in install["files"]:
        name = safe_name(row.get("distributionPath"))
        if name.casefold() in names or row.get("role") not in INSTALL_ROLES:
            raise SmokeError("Installed files collide or require undeclared runtime/developer material")
        names.add(name.casefold())
        if type(row.get("bytes")) is not int or row["bytes"] < 1 or not SHA.fullmatch(str(row.get("sha256", ""))):
            raise SmokeError("Every installed file needs concrete bytes and SHA256")
        suffix = PureWindowsPath(name).suffix.casefold()
        if suffix in {".dll", ".qml", ".js", ".mjs", ".node", ".lib", ".obj", ".pdb", ".h", ".hpp", ".a"}:
            raise SmokeError("Static-Qt runtime package contains a runtime sidecar or whole development prefix")
        if suffix == ".exe" and (name != "pd-launcher.exe" or row["role"] != "application"):
            raise SmokeError("Additional executable/sidecar is not a single Pd runtime")
        if row["role"] == "application":
            applications.append(row)
        licenses += row["role"] in {"license-material", "embedded-notice"}
        expected.append({key: row[key] for key in ("distributionPath", "bytes", "sha256")})
    expected.sort(key=lambda row: row["distributionPath"])
    if (len(applications) != 1 or applications[0]["distributionPath"] != "pd-launcher.exe" or
            any(applications[0][key] != artifact["executable"][key] for key in ("bytes", "sha256"))):
        raise SmokeError("The actual installed application does not match the final EXE")
    if not licenses:
        raise NotRun("Installed program lacks explicit license/notice material; source-tree licenses do not substitute")
    if sum(row["bytes"] for row in expected) > MAX_PACKAGE:
        raise SmokeError("Installed program exceeds smoke's bounded copy budget")
    if tree_files(path, limit=MAX_PACKAGE) != expected:
        raise SmokeError("Actual install directory has missing/extra/changed files versus its receipt")
    provenance = read_json(reference(install.get("provenance"), limit=MAX_JSON))
    if (provenance.get("kind") != "actual-cmake-install" or provenance.get("sourceCommit") != artifact["sourceCommit"] or
            provenance.get("executableSha256") != artifact["executable"]["sha256"] or
            provenance.get("files") != expected or not provenance.get("rawEvidence")):
        raise SmokeError("Install provenance must bind actual install command/log/manifest, not a hand-copied prefix")
    for row in provenance["rawEvidence"]:
        reference(row)
    package = artifact.get("programPackage", {})
    if package.get("compression", {}).get("format") != "zip" or package.get("scope") != "program-runtime-only":
        raise NotRun("Actual program-runtime ZIP is required, not a guessed filename or source/game bundle")
    zip_path = reference(package, limit=MAX_PACKAGE)
    if zip_records(zip_path) != expected:
        raise SmokeError("Actual ZIP members do not match the installed Pd application/resources/licenses")
    return path, zip_path, expected


def extract_exact(zip_path: Path, destination: Path, expected: list[dict]) -> None:
    capacity(sum(row["bytes"] for row in expected) + 24 * 1024 * 1024)
    plain(destination, exists=False)
    destination.mkdir(exist_ok=False)
    with zipfile.ZipFile(zip_path) as archive:
        for row in expected:
            target = inside(row["distributionPath"], root=destination, exists=False)
            target.parent.mkdir(parents=True, exist_ok=True)
            plain(target.parent)
            capacity(row["bytes"])
            with archive.open(row["distributionPath"]) as source, target.open("xb") as output:
                count = 0
                for chunk in iter(lambda: source.read(256 * 1024), b""):
                    count += len(chunk)
                    if count > row["bytes"]:
                        raise SmokeError("ZIP changed during owned extraction")
                    output.write(chunk)
            if count != row["bytes"] or digest(target) != row["sha256"]:
                raise SmokeError("Extracted bytes do not match independently observed install/ZIP SHA")
    if tree_files(destination, limit=MAX_PACKAGE) != expected:
        raise SmokeError("Clean extracted directory is not the exact actual installed file set")


def receipt_binding(row: dict | None, artifact: dict, owner: str) -> dict:
    if row is None:
        return {"status": "NOT_RUN", "owner": owner, "detail": "No actual owner runtime receipt supplied"}
    document = read_json(reference(row, limit=MAX_JSON))
    if (document.get("sourceCommit") != artifact["sourceCommit"] or
            document.get("executableSha256") != artifact["executable"]["sha256"]):
        return {"status": "FAIL", "owner": owner, "receipt": row, "detail": "Owner receipt binds a different source/actualexe"}
    if document.get("origin") != "actual-production-executable" or document.get("actualExecutableObserved") is not True:
        return {"status": "NOT_RUN", "owner": owner, "receipt": row, "detail": "Actual production EXE observation is not attested"}
    return {"status": "PASS", "owner": owner, "receipt": row,
            "ownerReportedStatus": document.get("status"), "detail": "Byte binding only; owner retains visual/functional judgment"}


def status(gates: dict) -> str:
    values = {row["status"] for row in gates.values()}
    return "FAIL" if "FAIL" in values else "NOT_RUN" if "NOT_RUN" in values or not values else "PASS"


def release_collector_evidence(artifact: dict, runtime: dict, output: Path) -> dict:
    import platform

    raw = [file_ref(output / name) for name in
           ("launch-environment.json", "observed-modules.json", "runtime-module-audit.json", "observed-windows.json", "runtime-receipt.json")]
    module_audit = read_json(output / "runtime-module-audit.json")
    observed = (runtime.get("productionExecutableObserved") is True and
                module_audit.get("runtimeGate") == "passed-observed-scenarios" and not module_audit.get("failures"))
    checks = [{"id": "no-dev-path-dll", "status": "passed" if observed else "pending", "rawEvidence": raw,
               "scope": "Only birth-bound actual startup module snapshots under OS-only PATH"}]
    for name in ("no-runtime-extraction", "portable-move", "unicode-long-path", "read-only-location",
                 "static-images-tls-dialogs", "offline-license-ui", "first-run-cli-portable-once"):
        checks.append({"id": name, "status": "pending", "rawEvidence": [],
                       "detail": "Not a complete actual clean-host/functional scenario in this isolated startup smoke"})
    document = {"schemaVersion": 1, "kind": "windows-clean-deployment-evidence", "status": "pending",
                "origin": "actual-isolated-deployment-on-developer-host", "sourceCommit": artifact["sourceCommit"],
                "executableSha256": artifact["executable"]["sha256"], "checks": checks,
                "machine": {"platform": platform.platform(), "architecture": platform.machine(), "python": platform.python_version()},
                "actualProductionExecutableObserved": runtime.get("productionExecutableObserved", False),
                "cleanWindowsHostProven": False, "completeProductVerified": False,
                "limitation": "Collector must not promote this dev-host isolated run to actual-clean-windows-host-run"}
    write_json(output / "windows-clean-deployment-evidence.json", document)
    return file_ref(output / "windows-clean-deployment-evidence.json")


def run(receipt_name: str | None, expected_commit: str | None, *, launch: bool, seconds: float) -> dict:
    fresh = capacity(24 * 1024 * 1024)
    with Worker() as worker:
        output = fresh_directory("run")
        gates = {key: {"status": "NOT_RUN", "detail": "Actual production evidence not yet observed"} for key in
                 ("sameGeneration", "actualInstallZip", "nativeStartup", "ownedWmClose", "visualByteBinding")}
        result = {"schemaVersion": 1, "kind": "pd-independent-delivery-smoke", "scope": "actual-clean-deployment-startup-only",
                  "createdUtc": now(), "status": "NOT_RUN", "completeProductVerified": False, "launchRequested": launch,
                  "productionExecutableObserved": False, "freshCapacity": fresh, "gates": gates,
                  "limitations": ["No feature/noGame/visual PASS is inferred from source or this bounded startup scenario.",
                                  "No Rust/Node/WebView developer prefix is copied or inherited by the target.",
                                  "WMI plus live snapshots are bounded observations, not a zero-loss/all-lifetime guarantee.",
                                  "All owned run directories are preserved; unknown files and processes are never cleaned."]}
        stage, dependencies = "sameGeneration", None
        try:
            result["sourceReview"] = source_review(expected_commit)
            dependencies = load_dependencies()
            result["acceptanceDependencies"] = {"lock": dependencies["lock"], "files": dependencies["files"]}
            if receipt_name is None:
                raise NotRun("No precise new Pd build receipt/production EXE supplied; historical Ni is deliberately not run")
            receipt_path = inside(receipt_name)
            artifact = read_json(receipt_path)
            identity_contract(artifact)
            reference(artifact["executable"], limit=MAX_PACKAGE)
            if expected_commit is None:
                raise NotRun("Independent expected production source commit is required with an actual receipt")
            if not COMMIT.fullmatch(expected_commit) or artifact["sourceCommit"] != expected_commit:
                raise SmokeError("Actual source differs from independently supplied production generation")
            result.update(sourceCommit=artifact["sourceCommit"], executableSha256=artifact["executable"]["sha256"],
                          buildSourceSha256=artifact["buildSourceSha256"], buildReceipt=file_ref(receipt_path))
            snapshot = verify_snapshot(artifact)
            build = verify_build(artifact, dependencies)
            gates[stage] = {"status": "PASS", "source": snapshot, "build": build,
                            "detail": "Observed EXE/source/compiled inputs/resources/raw build receipts are SHA bound"}
            stage = "actualInstallZip"
            install, archive, expected = package_contract(artifact)
            destination = output / "deployed"
            extract_exact(archive, destination, expected)
            executable = destination / "pd-launcher.exe"
            write_json(output / "install-zip-members.json", {"installPath": str(install), "programPackage": artifact["programPackage"],
                                                            "files": expected, "filesSha256": canonical(expected)})
            acceptance = dependencies["acceptance"].audit(executable)
            write_json(output / "pe-import-audit.json", acceptance)
            if (acceptance["sha256"] != artifact["executable"]["sha256"] or acceptance["importGate"] != "passed" or
                    acceptance["machine"] != "0x8664" or acceptance["bitness"] != 64 or acceptance["subsystem"] != 2):
                raise SmokeError("Existing acceptance PE parser rejected the actual deployed native Widgets EXE")
            gates[stage] = {"status": "PASS", "filesSha256": canonical(expected), "installedFileCount": len(expected),
                            "deployedExecutable": file_ref(executable), "peAudit": file_ref(output / "pe-import-audit.json")}
            stage = "nativeStartup"
            if not launch:
                raise NotRun("Prepare verifies actual bytes only; explicit --launch is required for real native startup")
            sys.path.insert(0, str(SCOPE))
            runtime_tool = importlib.import_module("windows_runtime")
            runtime = runtime_tool.observe(executable, artifact, output, dependencies, seconds, capacity, worker=worker)
            result["runtime"] = runtime
            result["productionExecutableObserved"] = runtime["productionExecutableObserved"]
            gates["nativeStartup"] = runtime["startupGate"]
            gates["ownedWmClose"] = runtime["closeGate"]
            result["releaseCollectorEvidence"] = release_collector_evidence(artifact, runtime, output)
            stage = "visualByteBinding"
            gates[stage] = receipt_binding(artifact.get("visualEvidence"), artifact, "ui-resume")
            result["productionServicesReceiptBinding"] = receipt_binding(artifact.get("functionalEvidence"), artifact, "production-smoke")
            reference(artifact["executable"], limit=MAX_PACKAGE)
            reference(artifact["programPackage"], limit=MAX_PACKAGE)
            if tree_files(install, limit=MAX_PACKAGE) != expected:
                raise SmokeError("Original install mutated during independent startup observation")
            for row in dependencies["files"]:
                reference(row, limit=MAX_JSON)
            result["finalCapacity"] = capacity()
        except NotRun as error:
            gates[stage] = {"status": "NOT_RUN", "detail": str(error)}
        except (OSError, ValueError, KeyError, TypeError, ImportError, subprocess.SubprocessError) as error:
            missing = dependencies is not None and isinstance(error, dependencies["common"].MissingEvidence)
            gates[stage] = {"status": "NOT_RUN" if missing else "FAIL", "detail": str(error)}
        result["singleWorkerLeaseRetained"] = worker.keep
        result["status"] = status(gates)
        result["completedUtc"] = now()
        result["receiptPath"] = (output / "receipt.json").relative_to(ROOT).as_posix()
        write_json(output / "receipt.json", result)
        return {"status": result["status"], "receipt": file_ref(output / "receipt.json"),
                "productionExecutableObserved": result["productionExecutableObserved"], "gates": gates}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", help="Exact worktree-relative actual builder Artifact receipt")
    parser.add_argument("--expected-source-commit", help="Independently supplied full new Pd production commit")
    parser.add_argument("--launch", action="store_true", help="Observe actual deployed EXE, never a fixture")
    parser.add_argument("--seconds", type=float, default=8.0)
    args = parser.parse_args()
    try:
        if not 3.0 <= args.seconds <= 20.0:
            raise SmokeError("Native observation is bounded to 3..20 seconds")
        result = run(args.receipt, args.expected_source_commit, launch=args.launch, seconds=args.seconds)
    except NotRun as error:
        result = {"status": "NOT_RUN", "receipt": None, "productionExecutableObserved": False, "detail": str(error)}
    except (OSError, ValueError, TypeError, subprocess.SubprocessError) as error:
        result = {"status": "FAIL", "receipt": None, "productionExecutableObserved": False, "detail": str(error)}
    print(json.dumps(result, indent=2, ensure_ascii=True))
    return {"PASS": 0, "FAIL": 1, "NOT_RUN": 2}[result["status"]]


if __name__ == "__main__":
    sys.modules["delivery_smoke"] = sys.modules[__name__]
    raise SystemExit(main())
