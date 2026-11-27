#!/usr/bin/env python3
"""Fail-closed macOS delivery tooling. No CI dispatch, credential reads, or Git writes."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parent.parent
QT_VERSION = "6.8.3"
QT_MODULES = ("qtbase", "qtsvg", "qtimageformats")
ARCHITECTURES = ("arm64", "x86_64")
MAX_SMALL_INPUT = 4 * 1024 * 1024
MAX_FILES = 20000
MAX_LOG_BYTES = 8 * 1024 * 1024

# Upstream source archives legitimately ship relative internal symlinks (the
# pinned zstd-1.5.7 tarball ships tests/cli-tests/bin/unzstd -> zstd), so
# verified source extraction accepts exactly those via validate_source_symlink
# and fails closed on every other link shape before any bytes are written.
MAX_ARCHIVE_SYMLINKS = 4096
MAX_SYMLINK_TARGET = 4096

# The brand resource CMake tests (native/tests/brand/CMakeLists.txt) read the
# original artwork and the immutable hash baseline from an explicitly passed
# trusted ASSET_ORACLE_ROOT; included builds never infer it from
# CMAKE_SOURCE_DIR. The macOS delivery binds that root to the verified,
# hash-checked source stage and fails closed otherwise. macos-delivery.ci.py
# stages exactly these contract files with the brand tests so the verified
# stage always satisfies the CMake oracle existence checks.
BRAND_ORACLE_REQUIRED = ("src/assets/mod-placeholder.png", "src/assets/fonts/nickel-line.woff2",
                         "tests/brand/functional-icons.sha256")

# Exact reviewed `--version` transcripts, keyed to the pinned tool wheel version.
# The pinned ninja-1.13.0-py3-none-macosx_10_9_universal2.whl
# (sha256 fa2a8bfc62e31b08f83127d1613d10821775a0eb334197154c4d6067b7068ff1)
# is Kitware's build and legitimately emits the reviewed build suffix below.
# Anything not listed here is rejected: no prefix/semver matching, so a different
# version, an arbitrary `.git.*` suffix, empty output, or merged extra lines cannot
# pass by formatting alone.
REVIEWED_TOOL_OUTPUTS = {
    "cmake": {
        "3.31.6": {
            "firstLine": "cmake version 3.31.6",
            "trailingLines": {"", "CMake suite maintained and supported by Kitware (kitware.com/cmake)."},
        },
    },
    "ninja": {
        "1.13.0": {
            "exactLines": {("1.13.0",), ("1.13.0.git.kitware.jobserver-pipe-1",)},
        },
    },
}
FORBIDDEN = re.compile(
    r"webview|webengine|webview2|chromium|javascriptcore|quickjs|tauri|rustsidecar|"
    r"(?:^|[/_.-])(?:qml|qtquick|node|v8)(?:$|[/_.-])|"
    r"Qt(?:Network|Qml|Quick|Web|Multimedia|Sql|Pdf)", re.IGNORECASE
)
MACH_MAGIC = {b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
              b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
              b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}


class DeliveryError(RuntimeError):
    pass


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
        return value.hexdigest()


def small_json(path: Path) -> dict:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_SMALL_INPUT:
        raise DeliveryError("Missing, linked, or oversized JSON input: " + str(path))
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise DeliveryError("JSON input must be an object: " + str(path))
    return value


def require_sha(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise DeliveryError("An exact lowercase SHA256 is required")
    return value


def safe_relative(value: str) -> PurePosixPath:
    if not isinstance(value, str) or not value or "\\" in value or "\x00" in value:
        raise DeliveryError("Unsafe relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise DeliveryError("Unsafe relative path: " + value)
    if any(":" in part for part in path.parts):
        raise DeliveryError("Unsafe relative path: " + value)
    return path


def within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except (ValueError, OSError, RuntimeError):
        return False


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise DeliveryError("Refusing linked evidence destination")
    temporary = path.with_name(path.name + ".tmp-" + str(os.getpid()))
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def capture(argv: list[str], timeout: int = 15, environment: dict | None = None) -> dict:
    record = {"argv": argv, "executedUtc": utc_now(), "executed": True}
    try:
        result = subprocess.run(argv, capture_output=True, text=True, errors="replace",
                                timeout=timeout, env=environment)
        record.update(returnCode=result.returncode, stdout=result.stdout[:65536],
                      stderr=result.stderr[:8192])
    except (OSError, subprocess.TimeoutExpired) as error:
        record.update(returnCode=None, stdout="", stderr=str(error), complete=False)
    return record


def discover() -> dict:
    environment_names = ("RUNNER_OS", "RUNNER_ARCH", "RUNNER_ENVIRONMENT", "GITHUB_ACTIONS",
                         "ImageOS", "ImageVersion", "DEVELOPER_DIR", "SDKROOT")
    result = {"schema": 1, "executedUtc": utc_now(), "system": platform.system(),
              "release": platform.release(), "machine": platform.machine(),
              "pythonVersion": platform.python_version(), "appleHost": sys.platform == "darwin",
              "environment": {name: os.environ[name] for name in environment_names if name in os.environ},
              "tools": {name: shutil.which(name) for name in
                        ("cmake", "ninja", "xcrun", "xcodebuild", "otool", "lipo", "codesign", "dsymutil", "gh", "ssh")},
              "commands": [], "appleRuntimeExecuted": False}
    if sys.platform == "darwin":
        for arguments in (["sw_vers"], ["uname", "-m"], ["xcode-select", "-p"],
                          ["xcodebuild", "-version"], ["xcrun", "--sdk", "macosx", "--show-sdk-version"],
                          ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
                          ["xcrun", "--find", "clang++"], ["xcrun", "clang++", "--version"],
                          ["sysctl", "-n", "sysctl.proc_translated"]):
            result["commands"].append(capture(arguments))
    return result


def apple_environment(policy: dict, architecture: str) -> tuple[dict, dict]:
    if sys.platform != "darwin":
        raise DeliveryError("Real Darwin host required; Windows/Linux cross compilation is not Apple runtime evidence")
    if architecture not in ARCHITECTURES:
        raise DeliveryError("Unsupported architecture")
    native = capture(["uname", "-m"])
    if native.get("returnCode") != 0 or native["stdout"].strip() != architecture:
        raise DeliveryError("Each architecture must build and run on a matching native Apple host; no Rosetta pass")
    translated = capture(["sysctl", "-n", "sysctl.proc_translated"])
    if translated.get("returnCode") == 0 and translated["stdout"].strip() == "1":
        raise DeliveryError("Translated host cannot satisfy native architecture gate")
    developer = Path(policy["developerDirectory"])
    if not developer.is_dir():
        raise DeliveryError("Pinned Xcode directory missing: " + str(developer))
    environment = dict(os.environ)
    for name in tuple(environment):
        if name.startswith(("DYLD_", "QT_", "QML")) or name in ("CMAKE_PREFIX_PATH", "CPATH", "LIBRARY_PATH"):
            environment.pop(name, None)
    environment["DEVELOPER_DIR"] = str(developer)
    xcode = capture(["xcodebuild", "-version"], environment=environment)
    expected = "Xcode " + policy["xcodeVersion"] + "\nBuild version " + policy["xcodeBuild"]
    if xcode.get("returnCode") != 0 or xcode["stdout"].strip() != expected:
        raise DeliveryError("Actual Xcode does not match frozen version/build")
    sdk = capture(["xcrun", "--sdk", "macosx", "--show-sdk-version"], environment=environment)
    if sdk.get("returnCode") != 0 or sdk["stdout"].strip() != policy["sdkVersion"]:
        raise DeliveryError("Actual macOS SDK does not match frozen version")
    compiler = capture(["xcrun", "clang++", "--version"], environment=environment)
    if compiler.get("returnCode") != 0 or policy["appleClangVersion"] not in compiler["stdout"]:
        raise DeliveryError("Actual compiler is not the pinned AppleClang")
    sdk_path = capture(["xcrun", "--sdk", "macosx", "--show-sdk-path"], environment=environment)
    if sdk_path.get("returnCode") != 0:
        raise DeliveryError("Cannot locate pinned Apple SDK")
    environment["SDKROOT"] = sdk_path["stdout"].strip()
    environment["MACOSX_DEPLOYMENT_TARGET"] = policy["deploymentTarget"]
    return environment, {"nativeArchitecture": architecture, "xcode": xcode, "sdk": sdk,
                         "compiler": compiler, "sdkPath": sdk_path, "rosetta": translated}


def verify_tool_output(name: str, pinned: str, output: str) -> None:
    """Accept only the reviewed exact transcript for the pinned tool version."""
    rules = REVIEWED_TOOL_OUTPUTS.get(name, {}).get(pinned)
    if rules is None:
        raise DeliveryError("No reviewed actual output is pinned for the required tool version")
    if not isinstance(output, str) or not output:
        raise DeliveryError("Actual " + name + " version output is missing")
    lines = tuple(output.splitlines())
    if "exactLines" in rules:
        if lines not in rules["exactLines"]:
            raise DeliveryError("Actual " + name + " version output differs from the reviewed pinned transcript")
        return
    if not lines or lines[0] != rules["firstLine"]:
        raise DeliveryError("Actual " + name + " version output differs from the reviewed pinned transcript")
    if any(line not in rules["trailingLines"] for line in lines[1:]):
        raise DeliveryError("Actual " + name + " version output carries unreviewed extra lines")


def validate_inputs(path: Path) -> dict:
    inputs = small_json(path)
    if inputs.get("schema") != 1 or inputs.get("qtVersion") != QT_VERSION:
        raise DeliveryError("Input schema and Qt exact pin must be 1 / 6.8.3")
    policy = inputs.get("policy", {})
    if policy.get("deploymentTarget") != "12.0" or policy.get("architectures") != list(ARCHITECTURES):
        raise DeliveryError("macOS policy must explicitly select 12.0 and both native architectures")
    if policy.get("qtLinkage") != "dynamic-framework" or policy.get("qtBusinessNetwork") is not False:
        raise DeliveryError("macOS Qt policy requires dynamic frameworks and no QtNetwork business stack")
    sources = inputs.get("sources", [])
    names = [entry.get("name") for entry in sources]
    if names != list(QT_MODULES):
        raise DeliveryError("Only qtbase, qtsvg and qtimageformats are permitted Qt source modules")
    for entry in sources:
        if entry.get("version") != QT_VERSION:
            raise DeliveryError("Qt source version mismatch")
        require_sha(entry.get("sha256"))
        safe_relative(entry["archive"])
        safe_relative(entry["sourceDirectory"])
        url = urllib.parse.urlsplit(entry["url"])
        expected = "/archive/qt/6.8/6.8.3/submodules/" + entry["archive"]
        if url.scheme != "https" or url.netloc != "download.qt.io" or url.path != expected or url.query or url.fragment:
            raise DeliveryError("Qt sources must match the official exact archive URL")
    tools = inputs.get("tools", {}).get("sources")
    if not isinstance(tools, list) or not tools:
        raise DeliveryError("Pinned macOS tool wheels are required")
    for entry in tools:
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]*-wheel", str(entry.get("name", ""))):
            raise DeliveryError("Tool wheel names must be exact -wheel pins")
        require_sha(entry.get("sha256"))
        safe_relative(entry.get("archive"))
        directory = safe_relative(entry.get("binaryDirectory"))
        if len(directory.parts) < 2:
            raise DeliveryError("Tool binary directory must be an explicit nested wheel path")
        if type(entry.get("bytes")) is not int or entry["bytes"] <= 0:
            raise DeliveryError("Tool wheel byte count must be an exact positive integer")
        url = urllib.parse.urlsplit(entry.get("url", ""))
        if url.scheme != "https" or url.username or url.password or url.query or url.fragment:
            raise DeliveryError("Tool wheels must use pinned credential-free HTTPS")
    return inputs


def source_records(value: list[dict], label: str) -> dict[str, dict]:
    if not isinstance(value, list) or not value:
        raise DeliveryError(label + " needs exact source records")
    result = {}
    for item in value:
        name = item["name"]
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name) or name in result:
            raise DeliveryError(label + " duplicate/unsafe dependency name")
        require_sha(item["sha256"])
        safe_relative(item["archive"])
        safe_relative(item["sourceDirectory"])
        url = urllib.parse.urlsplit(item["url"])
        if url.scheme != "https" or url.username or url.password or url.query or url.fragment:
            raise DeliveryError(label + " source must use pinned credential-free HTTPS")
        result[name] = item
    return result


def verify_source_contract(lock: Path, contract: Path, expected: str, inputs: dict,
                           rules_path: Path | None = None, supplement_path: Path | None = None,
                           supplement_sha256: str | None = None) -> dict:
    require_sha(expected)
    if digest(contract) != expected:
        raise DeliveryError("Acceptance source contract hash mismatch")
    accepted = small_json(contract)
    frozen = small_json(lock)
    if frozen.get("schema") != 1 or frozen.get("qtVersion") != QT_VERSION:
        raise DeliveryError("Dependency lock version mismatch")
    lock_hash = digest(lock)
    fields = ("name", "version", "archive", "url", "sha256", "sourceDirectory")
    frozen_by_name = source_records(frozen["sources"], "Frozen lock")
    extra = None
    if accepted.get("schemaVersion") == 1 and isinstance(accepted.get("files"), dict):
        pins = inputs["frozenInputs"]
        if expected != pins["sourceContractSha256"] or lock_hash != pins["dependencyLockSha256"]:
            raise DeliveryError("Frozen publication source/lock SHA256 differs from policy")
        if rules_path is None or digest(rules_path) != pins["verificationRulesSha256"]:
            raise DeliveryError("Frozen verification rules SHA256 required")
        rules = small_json(rules_path)
        if rules.get("schemaVersion") != 1 or rules.get("upstreamSourceContractSha256") != expected or rules.get("dependencyLockSha256") != lock_hash:
            raise DeliveryError("Verification rules do not bind this source/lock publication")
        approved_by_name = source_records(rules["dependencySources"], "Verification rules")
        if set(approved_by_name) != set(frozen_by_name):
            raise DeliveryError("Verification rules and dependency lock names differ")
        manifest_entries(accepted, "Acceptance source")
        if accepted.get("snapshotSha256") != pins["snapshotSha256"] or accepted.get("sourceSetSha256") != pins["sourceSetSha256"] or len(accepted["files"]) != pins["sourceFiles"]:
            raise DeliveryError("Frozen production source-set differs from publication")
        if supplement_path is not None:
            require_sha(supplement_sha256)
            if digest(supplement_path) != supplement_sha256:
                raise DeliveryError("macOS input supplement SHA256 mismatch")
            extra = small_json(supplement_path)
            if extra.get("schema") != 1 or extra.get("dependencyLockSha256") != lock_hash or extra.get("sourceContractSha256") != expected or extra.get("verificationRulesSha256") != pins["verificationRulesSha256"]:
                raise DeliveryError("Supplement does not bind the frozen publication")
            extras = source_records(extra["sources"], "macOS supplement")
            if set(extras) != {"openssl"}:
                raise DeliveryError("Only lead-approved OpenSSL may supplement the frozen dependency lock")
            ssl = extras["openssl"]
            ssl_url = "https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz"
            if ssl.get("version") != "3.5.9" or ssl.get("url") != ssl_url or ssl.get("sha256") != "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a" or ssl.get("bytes") != 53279637 or ssl.get("systemFallbackAllowed") is not False:
                raise DeliveryError("OpenSSL must be the exact approved 3.5.9 LTS official source, not system/latest")
            require_sha(ssl["checksumSha256"])
            approved_by_name.update(extras)
            verify_tools_contract(inputs, extra)
    else:
        if accepted.get("schema") != 1 or accepted.get("lockSha256") != lock_hash:
            raise DeliveryError("Dependency lock is not the acceptance-frozen source contract")
        approved_by_name = source_records(accepted.get("sources"), "Acceptance contract")
    for name, entry in frozen_by_name.items():
        if name not in approved_by_name or any(entry.get(key) != approved_by_name[name].get(key) for key in fields):
            raise DeliveryError("Frozen dependency tuple differs from acceptance: " + name)
    for entry in inputs["sources"]:
        approved = approved_by_name.get(entry["name"])
        if approved is None or any(entry.get(key) != approved.get(key) for key in fields):
            raise DeliveryError("macOS source not yet acceptance-frozen: " + entry["name"])
    return {"executed": True, "lockSha256": lock_hash, "contractSha256": expected,
            "rulesSha256": digest(rules_path) if rules_path else None,
            "supplementSha256": digest(supplement_path) if supplement_path else None,
            "inputsSha256": digest(Path(inputs["_path"])) if "_path" in inputs else None,
            "sourceTupleVerified": True, "sourceRecords": list(approved_by_name.values()),
            "tools": extra.get("tools", []) if extra else accepted.get("tools", []),
            "archiveBytesVerified": False, "productionSourceBytesVerified": False}


def verify_archives(inputs: dict, downloads: Path, entries: list[dict] | None = None) -> list[dict]:
    records = []
    for entry in entries if entries is not None else inputs["sources"]:
        archive = downloads / entry["archive"]
        if archive.is_symlink() or not archive.is_file() or digest(archive) != entry["sha256"]:
            raise DeliveryError("Missing or mismatched source archive: " + entry["archive"])
        if "bytes" in entry and archive.stat().st_size != entry["bytes"]:
            raise DeliveryError("Source archive exact byte count mismatch")
        records.append({**entry, "archivePath": str(archive.resolve()), "bytes": archive.stat().st_size,
                        "verifiedSha256": digest(archive), "executed": True})
    return records


@contextmanager
def exclusive_root(root: Path):
    root.mkdir(parents=True, exist_ok=True)
    if root.is_symlink():
        raise DeliveryError("Linked tools root rejected")
    lock = root / ".macos-delivery.lock"
    with lock.open("a+b") as stream:
        if sys.platform == "darwin":
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            if sys.platform == "darwin":
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def validate_source_symlink(member: tarfile.TarInfo, path: PurePosixPath,
                            real_members: dict[str, tarfile.TarInfo]) -> str:
    """Validate one source-archive symlink against the safe upstream-link policy.

    Upstream source archives legitimately carry relative internal symlinks (the
    pinned zstd-1.5.7 tarball ships tests/cli-tests/bin/unzstd -> zstd), so the
    former blanket link rejection became a strict per-link policy. The target
    must be relative (no absolute, UNC, Windows-style, or backslash targets),
    must normalize inside the extraction root without escaping it, and must
    resolve to an existing regular file or directory member of the same
    archive - never to another link, a dangling name, or the archive root - so
    link chains and cycles are structurally impossible. Oversized targets fail
    closed; the caller bounds the total link count.
    """
    target = member.linkname
    if (not isinstance(target, str) or not target or "\\" in target or "\x00" in target
            or len(target) > MAX_SYMLINK_TARGET
            or target.startswith("/") or (len(target) > 1 and target[1] == ":")):
        raise DeliveryError("Unsafe source symlink target: " + path.as_posix())
    parts = list(path.parts[:-1])
    for segment in target.split("/"):
        if segment in ("", "."):
            continue
        if segment == "..":
            if not parts:
                raise DeliveryError("Source symlink target escapes the extraction root: "
                                    + path.as_posix() + " -> " + target)
            parts.pop()
        else:
            parts.append(segment)
    if not parts:
        raise DeliveryError("Source symlink target resolves to the extraction root: "
                            + path.as_posix() + " -> " + target)
    resolved = "/".join(parts)
    target_member = real_members.get(resolved)
    if target_member is None or not (target_member.isfile() or target_member.isdir()):
        raise DeliveryError("Source symlink must resolve to an existing regular archive member: "
                            + path.as_posix() + " -> " + target)
    return resolved


def extract_verified_archive(archive: Path, entry: dict, destination: Path,
                             entrypoint: str = "CMakeLists.txt") -> Path:
    if archive.is_symlink() or digest(archive) != entry["sha256"]:
        raise DeliveryError("Source hash changed before extraction")
    if destination.exists():
        raise DeliveryError("Extraction destination must be new; source stamps do not prove an existing tree")
    destination.mkdir(parents=True)
    with tarfile.open(archive) as bundle:
        members = bundle.getmembers()
        if len(members) > 150000 or sum(max(0, item.size) for item in members) > 4 * 1024 ** 3:
            raise DeliveryError("Source archive expansion exceeds bound")
        real_members: dict[str, tarfile.TarInfo] = {}
        ancestors: set[str] = set()
        for member in members:
            path = safe_relative(member.name.rstrip("/"))
            if path.as_posix() in real_members:
                raise DeliveryError("Duplicate source archive member: " + path.as_posix())
            if path.parts[0] != entry["sourceDirectory"]:
                raise DeliveryError("Unexpected source archive root")
            real_members[path.as_posix()] = member
            ancestors.update("/".join(path.parts[:index]) for index in range(1, len(path.parts)))
        links = 0
        for name, member in real_members.items():
            if member.issym():
                links += 1
                if links > MAX_ARCHIVE_SYMLINKS:
                    raise DeliveryError("Source archive contains too many symlinks")
                validate_source_symlink(member, PurePosixPath(name), real_members)
            elif member.islnk() or not (member.isfile() or member.isdir()):
                raise DeliveryError("Linked or special source archive member rejected")
            if not member.isdir() and name in ancestors:
                raise DeliveryError("Source archive member conflicts with a nested path: " + name)
        bundle.extractall(destination, members=members, filter="data")
    source = destination / entry["sourceDirectory"]
    if not (source / str(safe_relative(entrypoint))).is_file():
        raise DeliveryError("Source archive lacks real declared build entry: " + entrypoint)
    return source


class Recorder:
    def __init__(self, output: Path, initial: dict):
        self.output = output
        self.record = {"schema": 1, "startedUtc": utc_now(), "commands": [],
                       "complete": False, "appleRuntimeExecuted": False, **initial}
        self.save()

    def save(self) -> None:
        write_json(self.output, self.record)

    def run(self, argv: list[str], cwd: Path, environment: dict, timeout: int = 5400) -> str:
        number = len(self.record["commands"])
        log = self.output.parent / (self.output.stem + f"-{number:03}.log")
        item = {"argv": argv, "cwd": str(cwd), "startedUtc": utc_now(), "executed": True,
                "log": str(log), "returnCode": None}
        self.record["commands"].append(item)
        self.save()
        with log.open("x", encoding="utf-8", newline="\n") as stream:
            try:
                result = subprocess.run(argv, cwd=cwd, env=environment, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=timeout)
                item["returnCode"] = result.returncode
            except (OSError, subprocess.TimeoutExpired) as error:
                item["failure"] = str(error)
                stream.write("\n" + str(error))
        item["finishedUtc"] = utc_now()
        item["logSha256"] = digest(log)
        self.save()
        if item["returnCode"] != 0:
            raise DeliveryError("Command failed or timed out; see " + str(log))
        if log.stat().st_size > MAX_LOG_BYTES:
            raise DeliveryError("Command log exceeds evidence budget")
        return log.read_text(encoding="utf-8", errors="replace")

    def finish(self, **values) -> None:
        self.record.update(complete=True, finishedUtc=utc_now(), **values)
        self.save()


def parse_cache(path: Path) -> dict[str, str]:
    values = {}
    if not path.is_file():
        raise DeliveryError("Actual CMake cache missing")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, rest = line.split(":", 1)
            values[key] = rest.split("=", 1)[1]
    return values


def parse_otool_libraries(text: str) -> list[str]:
    result = []
    for line in text.splitlines():
        match = re.match(r"^\s+(.+?) \(compatibility version [^\n]+\)$", line)
        if match:
            result.append(match.group(1))
        elif line.startswith(("\t", "        ")) and line.strip():
            raise DeliveryError("Unrecognized otool library line")
    if not text.strip():
        raise DeliveryError("Empty otool output")
    return result


def parse_load_commands(text: str) -> dict:
    result = {"rpaths": [], "installId": None, "minimums": [], "platforms": [], "sdkVersions": [], "uuid": None}
    command = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("cmd "):
            command = line[4:]
        elif command == "LC_RPATH" and line.startswith("path "):
            result["rpaths"].append(line[5:].rsplit(" (offset ", 1)[0])
        elif command == "LC_ID_DYLIB" and line.startswith("name "):
            result["installId"] = line[5:].rsplit(" (offset ", 1)[0]
        elif command == "LC_UUID" and line.startswith("uuid "):
            result["uuid"] = line[5:]
        elif command == "LC_BUILD_VERSION" and line.startswith("platform "):
            result["platforms"].append(line[9:])
        elif command == "LC_BUILD_VERSION" and line.startswith("minos "):
            result["minimums"].append(line[6:])
        elif command == "LC_BUILD_VERSION" and line.startswith("sdk "):
            result["sdkVersions"].append(line[4:])
        elif command == "LC_VERSION_MIN_MACOSX" and line.startswith("version "):
            result["minimums"].append(line[8:])
            result["platforms"].append("MACOS")
        elif command == "LC_VERSION_MIN_MACOSX" and line.startswith("sdk "):
            result["sdkVersions"].append(line[4:])
    return result


def parse_otool_abi_versions(text: str) -> list[dict]:
    records = []
    for line in text.splitlines():
        match = re.match(r"^\s+(.+?) \(compatibility version ([0-9]+(?:\.[0-9]+){0,2}), current version ([0-9]+(?:\.[0-9]+){0,2})\)$", line)
        if match:
            records.append({"name": match[1], "compatibilityVersion": match[2], "currentVersion": match[3]})
    if [item["name"] for item in records] != parse_otool_libraries(text):
        raise DeliveryError("Actual otool dependency ABI versions are missing or malformed")
    return records


def version_tuple(value: str) -> tuple[int, ...]:
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", value):
        raise DeliveryError("Invalid macOS version value")
    parts = tuple(int(part) for part in value.split("."))
    return parts + (0,) * (3 - len(parts))


def macho(path: Path) -> bool:
    with path.open("rb") as stream:
        return stream.read(4) in MACH_MAGIC


def bundle_inventory(bundle: Path) -> tuple[list[dict], list[Path]]:
    if bundle.suffix != ".app" or bundle.is_symlink() or not bundle.is_dir():
        raise DeliveryError("A real complete .app directory is required")
    records, images = [], []
    for index, path in enumerate(sorted(bundle.rglob("*"))):
        if index >= MAX_FILES:
            raise DeliveryError("Bundle entry count exceeds bound")
        relative = path.relative_to(bundle).as_posix()
        if FORBIDDEN.search(relative) or path.suffix.lower() in (".qml", ".js", ".exe", ".dll", ".node"):
            raise DeliveryError("Forbidden runtime material in bundle: " + relative)
        if path.is_symlink():
            if not within(path, bundle):
                raise DeliveryError("Bundle symlink escapes package: " + relative)
            records.append({"path": relative, "kind": "symlink", "target": os.readlink(path)})
        elif path.is_file():
            if stat.S_IMODE(path.stat().st_mode) & 0o022:
                raise DeliveryError("Group/world-writable bundle file: " + relative)
            records.append({"path": relative, "kind": "file", "bytes": path.stat().st_size,
                            "sha256": digest(path), "mode": oct(stat.S_IMODE(path.stat().st_mode))})
            if macho(path):
                images.append(path.resolve())
        elif not path.is_dir():
            raise DeliveryError("Special file in bundle")
    return records, sorted(set(images))


def expand_dyld(value: str, loader: Path, executable: Path) -> Path | None:
    for prefix, base in (("@loader_path", loader.parent), ("@executable_path", executable.parent)):
        if value == prefix or value.startswith(prefix + "/"):
            return base / value[len(prefix):].lstrip("/")
    return Path(value) if value.startswith("/") else None


def system_library(value: str) -> bool:
    return value.startswith(("/System/Library/", "/usr/lib/")) and ".." not in PurePosixPath(value).parts


def resolve_dependency(value: str, loader: Path, executable: Path, rpaths: list[str], bundle: Path) -> dict:
    if FORBIDDEN.search(value):
        raise DeliveryError("Forbidden dynamic dependency: " + value)
    if system_library(value):
        return {"name": value, "system": True, "resolution": "Apple system dyld cache/library"}
    if value.startswith("/"):
        raise DeliveryError("Absolute non-system dependency leaks development prefix: " + value)
    candidates = []
    if value.startswith("@rpath/"):
        for rpath in rpaths:
            base = expand_dyld(rpath, loader, executable)
            if base is not None:
                candidates.append(base / value[len("@rpath/"):])
    else:
        direct = expand_dyld(value, loader, executable)
        if direct is not None:
            candidates.append(direct)
    for candidate in candidates:
        if within(candidate, bundle) and candidate.is_file():
            return {"name": value, "system": False,
                    "resolution": candidate.resolve().relative_to(bundle.resolve()).as_posix()}
    raise DeliveryError("Unresolved or out-of-bundle non-system dependency: " + value)


def audit_bundle(bundle: Path, architecture: str, deployment_target: str, recorder: Recorder,
                 environment: dict, signed: bool = False) -> dict:
    if sys.platform != "darwin":
        raise DeliveryError("otool/codesign audit requires real Apple host")
    records, images = bundle_inventory(bundle)
    with checked_file(bundle, "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    executable_name = str(safe_relative(info.get("CFBundleExecutable")))
    if "/" in executable_name:
        raise DeliveryError("Bundle executable must be a single actual filename")
    executable = bundle / "Contents/MacOS" / executable_name
    if executable.resolve() not in images or not os.access(executable, os.X_OK):
        raise DeliveryError("Bundle main executable missing or non-executable")
    if info.get("CFBundleIdentifier") != "studio.nickel.launcher":
        raise DeliveryError("Bundle application identifier must preserve studio.nickel.launcher")
    if version_tuple(str(info.get("LSMinimumSystemVersion", "0"))) != version_tuple(deployment_target):
        raise DeliveryError("Info.plist deployment target mismatch")
    executable_load = parse_load_commands(recorder.run(["xcrun", "otool", "-l", str(executable)], bundle.parent, environment, 30))
    nodes, present_frameworks, system_abi = [], set(), []
    for image in images:
        relative = image.relative_to(bundle.resolve()).as_posix()
        archs = recorder.run(["xcrun", "lipo", "-archs", str(image)], bundle.parent, environment, 30).strip().split()
        if archs != [architecture]:
            raise DeliveryError("Every Mach-O/plugin must contain exactly the requested architecture: " + relative)
        library_output = recorder.run(["xcrun", "otool", "-L", str(image)], bundle.parent, environment, 30)
        abi = parse_otool_abi_versions(library_output)
        commands = parse_load_commands(recorder.run(["xcrun", "otool", "-l", str(image)], bundle.parent, environment, 30))
        if not commands["platforms"] or any(value not in ("1", "MACOS", "macOS") for value in commands["platforms"]):
            raise DeliveryError("Mach-O platform is not actual macOS: " + relative)
        if not commands["minimums"] or any(version_tuple(value) > version_tuple(deployment_target) for value in commands["minimums"]):
            raise DeliveryError("Mach-O minimum macOS exceeds declared policy or is missing: " + relative)
        if commands["installId"] and (FORBIDDEN.search(commands["installId"]) or not commands["installId"].startswith(("@rpath/", "@loader_path/", "@executable_path/"))):
            raise DeliveryError("Installed image has a forbidden/development install ID: " + relative)
        rpaths = []
        for owner, declared in ((image, commands["rpaths"]), (executable, executable_load["rpaths"])):
            for rpath in declared:
                base = expand_dyld(rpath, owner, executable)
                if base is None or not within(base, bundle):
                    raise DeliveryError("Untrusted development/escaping LC_RPATH: " + rpath)
                rpaths.append(str(base))
        resolutions = []
        for item in abi:
            if item["name"] == commands["installId"]:
                continue
            resolved = {**resolve_dependency(item["name"], image, executable, rpaths, bundle),
                        "compatibilityVersion": item["compatibilityVersion"], "currentVersion": item["currentVersion"]}
            resolutions.append(resolved)
            if resolved["system"]:
                system_abi.append({"consumer": relative, **resolved})
        nodes.append({"path": relative, "sha256": digest(image), "architecture": archs,
                      "loadCommands": commands, "dependencies": resolutions})
        present_frameworks.update(re.findall(r"(Qt[A-Za-z0-9]+)\.framework", relative))
        if signed:
            recorder.run(["codesign", "--verify", "--strict", "--verbose=2", str(image)], bundle.parent, environment, 30)
    required = {"QtCore", "QtGui", "QtWidgets", "QtSvg"}
    if not required.issubset(present_frameworks):
        raise DeliveryError("Required Qt frameworks missing from actual bundle")
    if not (bundle / "Contents/PlugIns/platforms/libqcocoa.dylib").is_file():
        raise DeliveryError("Native Cocoa plugin missing")
    if not system_abi:
        raise DeliveryError("Actual Mach-O system ABI dependency inventory is missing")
    if signed:
        recorder.run(["codesign", "--verify", "--deep", "--strict", "--verbose=2", str(bundle)], bundle.parent, environment, 30)
    return {"schema": 1, "executed": True, "kind": "actual-apple-bundle-static-closure",
            "architecture": architecture, "deploymentTarget": deployment_target,
            "bundle": str(bundle.resolve()), "files": records, "nodes": nodes,
            "qtFrameworks": sorted(present_frameworks), "closureComplete": True,
            "systemAbiDependencies": system_abi, "systemAbiInspectionExecuted": True,
            "systemAbiScope": "actual-linked-install-names-and-version-load-commands; not-minimum-os-runtime-verification",
            "signatureVerified": signed, "runtimeLoadedImagesVerified": False,
            "appleRuntimeExecuted": False, "minimumOsRuntimeVerified": False}


def build_qt(options: argparse.Namespace, inputs: dict, contract_evidence: dict) -> dict:
    environment, host = apple_environment(inputs["policy"], options.arch)
    root = options.tools_root.resolve()
    if within(root, REPOSITORY):
        raise DeliveryError("Real Qt build must use an independent external Apple tools root, not the source/shared mirror")
    if shutil.disk_usage(root.parent).free < 8 * 1024 ** 3:
        raise DeliveryError("Apple source build needs at least 8 GiB free; never attempt it on the constrained Windows host")
    archives = verify_archives(inputs, options.downloads.resolve())
    recorder = Recorder(options.output, {"kind": "qt-source-build", "host": host,
                                        "architecture": options.arch, "sourceContract": contract_evidence,
                                        "archives": archives})
    try:
        with exclusive_root(root):
            prefix = root / ("qt-6.8.3-" + options.arch)
            if prefix.exists():
                raise DeliveryError("Fresh isolated Qt prefix required; never mutate a shared or existing prefix")
            cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
            if not cmake or not ninja:
                raise DeliveryError("Verified CMake and Ninja must already be on PATH")
            cmake_version = recorder.run([cmake, "--version"], root, environment, 30)
            ninja_version = recorder.run([ninja, "--version"], root, environment, 30)
            verify_tool_output("cmake", inputs["tools"]["cmakeVersion"], cmake_version)
            verify_tool_output("ninja", inputs["tools"]["ninjaVersion"], ninja_version)
            for entry in inputs["sources"]:
                source = extract_verified_archive(options.downloads / entry["archive"], entry, root / ("source-" + entry["name"]))
                build = root / ("build-" + entry["name"])
                definitions = ["-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DCMAKE_INSTALL_PREFIX={prefix}",
                               f"-DCMAKE_PREFIX_PATH={prefix}", "-DBUILD_SHARED_LIBS=ON",
                               f"-DCMAKE_OSX_ARCHITECTURES={options.arch}",
                               "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0", f"-DCMAKE_OSX_SYSROOT={environment['SDKROOT']}",
                               "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
                               "-DQT_BUILD_EXAMPLES=OFF", "-DQT_BUILD_TESTS=OFF"]
                if entry["name"] == "qtbase":
                    definitions += ["-DFEATURE_framework=ON", "-DFEATURE_network=OFF", "-DFEATURE_sql=OFF",
                                    "-DFEATURE_dbus=OFF", "-DFEATURE_printsupport=OFF", "-DFEATURE_concurrent=OFF",
                                    "-DFEATURE_gui=ON", "-DFEATURE_widgets=ON", "-DFEATURE_testlib=ON",
                                    "-DFEATURE_macdeployqt=ON", "-DFEATURE_opengl=OFF", "-DFEATURE_vulkan=OFF",
                                    "-DQT_BUILD_TOOLS_BY_DEFAULT=ON", "-DFEATURE_system_zlib=OFF",
                                    "-DFEATURE_system_png=OFF", "-DFEATURE_system_jpeg=OFF",
                                    "-DFEATURE_system_freetype=OFF", "-DFEATURE_system_harfbuzz=OFF",
                                    "-DFEATURE_system_pcre2=OFF", "-DFEATURE_system_doubleconversion=OFF",
                                    "-DFEATURE_system_libb2=OFF"]
                elif entry["name"] == "qtimageformats":
                    definitions += ["-DINPUT_webp=qt", "-DINPUT_tiff=qt", "-DFEATURE_jasper=OFF", "-DFEATURE_mng=OFF"]
                if entry["name"] != "qtbase":
                    definitions.extend(qt_cmake_options(prefix, ("Core", "Gui", "Widgets")))
                recorder.run([cmake, "-S", str(source), "-B", str(build), "-G", "Ninja", *definitions], root, environment)
                recorder.run([cmake, "--build", str(build), "--parallel", str(options.jobs)], root, environment)
                recorder.run([cmake, "--install", str(build)], root, environment)
                if entry["name"] == "qtbase":
                    cache = parse_cache(build / "CMakeCache.txt")
                    for key, value in {"QT_FEATURE_framework": "ON", "QT_FEATURE_network": "OFF",
                                       "QT_FEATURE_widgets": "ON", "QT_FEATURE_macdeployqt": "ON"}.items():
                        if cache.get(key) != value:
                            raise DeliveryError("Qt feature cache does not match required policy: " + key)
            if (prefix / "lib/cmake/Qt6Network").exists() or not (prefix / "bin/macdeployqt").is_file():
                raise DeliveryError("Built Qt prefix violates minimal network/deployment tool policy")
            recorder.finish(prefix=str(prefix), qtVersion=QT_VERSION, linkage="dynamic-framework",
                            actualFrameworks=[str(path) for path in (prefix / "lib").glob("Qt*.framework")])
    except Exception as error:
        recorder.record["failure"] = str(error)
        recorder.save()
        raise
    return recorder.record


def https_download(entry: dict, directory: Path) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / str(safe_relative(entry["archive"]))
    if destination.exists():
        if destination.is_symlink() or digest(destination) != entry["sha256"]:
            raise DeliveryError("Existing owned download has the wrong hash")
        return {**entry, "path": str(destination), "verifiedSha256": digest(destination), "downloaded": False}
    url = urllib.parse.urlsplit(entry["url"])
    if url.scheme != "https" or url.username or url.password:
        raise DeliveryError("Dependency URL must use credential-free HTTPS")
    temporary = destination.with_name(destination.name + ".part-" + str(os.getpid()))
    request = urllib.request.Request(entry["url"], headers={"User-Agent": "Nickel-native-macos-source-verifier/1"})
    maximum = entry.get("bytes", 100 * 1024 * 1024)
    maximum = min(maximum, 256 * 1024 * 1024)
    with urllib.request.urlopen(request, timeout=60) as response, temporary.open("xb") as output:
        if urllib.parse.urlsplit(response.url).scheme != "https":
            raise DeliveryError("Source redirected to non-HTTPS transport")
        received = 0
        while block := response.read(256 * 1024):
            received += len(block)
            if received > maximum:
                raise DeliveryError("Source download exceeds its bound")
            output.write(block)
        output.flush()
        os.fsync(output.fileno())
    if digest(temporary) != entry["sha256"]:
        raise DeliveryError("Downloaded source SHA256 mismatch")
    if "bytes" in entry and received != entry["bytes"]:
        raise DeliveryError("Downloaded tool archive byte count mismatch")
    temporary.replace(destination)
    return {**entry, "path": str(destination), "verifiedSha256": digest(destination), "downloaded": True}


def verify_tools_contract(inputs: dict, accepted: dict) -> None:
    entries = {entry["name"]: entry for entry in accepted.get("tools", [])}
    fields = ("name", "version", "archive", "url", "sha256", "bytes")
    for tool in inputs["tools"]["sources"]:
        if tool["name"] not in entries or any(tool[key] != entries[tool["name"]].get(key) for key in fields):
            raise DeliveryError("Local macOS tool not yet acceptance-frozen: " + tool["name"])
        require_sha(tool["sha256"])


def fetch_sources(options: argparse.Namespace, inputs: dict, contract: dict) -> dict:
    if sys.platform != "darwin":
        raise DeliveryError("Source downloads are Apple-host-only; constrained Windows adds only small text")
    entries = contract["sourceRecords"] if options.include_core else inputs["sources"]
    if options.include_core and "openssl" not in {entry["name"] for entry in entries}:
        raise DeliveryError("Core downloads require the exact approved OpenSSL supplement")
    records = [https_download(entry, options.downloads) for entry in entries]
    if options.include_tools:
        verify_tools_contract(inputs, contract)
        records += [https_download(entry, options.downloads) for entry in inputs["tools"]["sources"]]
    return {"schema": 1, "kind": "actual-verified-apple-downloads", "sourceContract": contract,
            "records": records, "appleRuntimeExecuted": False, "productDelivered": False}


def install_tool_wheel(root: Path, archive: Path, entry: dict) -> dict:
    """Verify, extract, and resolve exactly one executable from a pinned wheel.

    Wheels disagree on where scripts live: cmake ships ``cmake/data/bin/cmake``
    while ninja ships ``ninja-1.13.0.data/scripts/ninja`` (the PEP 427
    ``.data/scripts`` install scheme). The declared ``binaryDirectory`` is
    authoritative and must name the single archive location that carries the
    executable; candidates are limited to ``bin/`` and ``scripts/`` install
    directories so non-executable lookalikes (e.g. cmake's bash-completion
    file) do not count. Missing, duplicated, or contradictory locations fail
    closed before extraction, so the install root is never partially populated
    by a wheel whose declared layout does not match its actual bytes.
    """
    name = entry.get("name")
    if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*-wheel", name):
        raise DeliveryError("Tool wheel entry name must be an exact -wheel pin")
    tool = name.removesuffix("-wheel")
    if len(safe_relative(tool).parts) != 1:
        raise DeliveryError("Tool wheel executable name must be a bare file name")
    declared_value = entry.get("binaryDirectory")
    if not isinstance(declared_value, str):
        raise DeliveryError("Tool wheel must declare an explicit binary directory: " + name)
    declared = safe_relative(declared_value)
    if not archive.is_file() or archive.is_symlink() or digest(archive) != entry.get("sha256") or archive.stat().st_size != entry.get("bytes"):
        raise DeliveryError("Tool archive must be hash/byte-count verified before extraction")
    import zipfile
    with zipfile.ZipFile(archive) as wheel:
        if sum(item.file_size for item in wheel.infolist()) > 384 * 1024 * 1024:
            raise DeliveryError("Tool wheel expansion exceeds bound")
        members = []
        for item in wheel.infolist():
            relative = safe_relative(item.filename.rstrip("/"))
            if stat.S_ISLNK(item.external_attr >> 16):
                raise DeliveryError("Linked tool wheel entry rejected")
            members.append((item, relative))
        located = [relative for item, relative in members
                   if not item.is_dir() and relative.name == tool and relative.parent.name in ("bin", "scripts")]
        expected = declared / tool
        if not located:
            raise DeliveryError("Verified tool wheel binary path missing: " + name)
        if len(located) != 1 or located[0] != expected:
            raise DeliveryError("Verified tool wheel binary path is duplicated or contradicts its declared directory: " + name)
        wheel.extractall(root)
    directory = root / str(declared)
    binary = root / str(expected)
    if not within(directory, root) or directory.is_symlink() or not directory.is_dir():
        raise DeliveryError("Verified tool wheel binary directory missing: " + name)
    if not within(binary, root) or binary.is_symlink() or not binary.is_file():
        raise DeliveryError("Verified tool wheel binary path missing: " + name)
    for item in directory.iterdir():
        if item.is_file() and not item.is_symlink():
            item.chmod(0o755)
    return {"name": name, "binary": str(binary.resolve()), "binarySha256": digest(binary),
            "wheelSha256": entry["sha256"]}


def setup_tools(options: argparse.Namespace, inputs: dict, contract: dict) -> dict:
    if sys.platform != "darwin":
        raise DeliveryError("Tool setup is Apple-host-only; no large local tool installations")
    verify_tools_contract(inputs, contract)
    root = options.tools_root
    if root.exists():
        raise DeliveryError("Tool installation root must be new and isolated")
    root.mkdir(parents=True)
    records = [install_tool_wheel(root, options.downloads / entry["archive"], entry)
               for entry in inputs["tools"]["sources"]]
    return {"schema": 1, "kind": "actual-verified-apple-tool-install", "records": records,
            "sourceContract": contract,
            "pathDirectories": [str(root / str(safe_relative(entry["binaryDirectory"])))
                                for entry in inputs["tools"]["sources"]],
            "appleRuntimeExecuted": False, "productDelivered": False}


def manifest_entries(data: dict, label: str) -> list[dict]:
    files = data.get("files", data.get("sources"))
    if isinstance(files, dict) and data.get("schemaVersion") == 1:
        canonical = json.dumps(files, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
        if hashlib.sha256(canonical).hexdigest() != require_sha(data.get("snapshotSha256")):
            raise DeliveryError(label + " canonical file-map snapshot SHA256 differs")
        entries = [{"path": path, **item} for path, item in files.items()]
        if any("bytes" not in item for item in entries):
            raise DeliveryError(label + " frozen file records need exact byte counts")
    elif isinstance(files, list) and data.get("schema") == 1:
        entries = files
    else:
        raise DeliveryError(label + " requires a nonempty real input file list/map")
    if not entries or len(entries) > MAX_FILES:
        raise DeliveryError(label + " real input file set is empty/oversized")
    seen = set()
    for item in entries:
        relative = str(safe_relative(item["path"]))
        if relative in seen:
            raise DeliveryError(label + " duplicate path")
        seen.add(relative)
        require_sha(item["sha256"])
        if "bytes" in item and (type(item["bytes"]) is not int or item["bytes"] < 0):
            raise DeliveryError(label + " invalid file byte count")
    return entries


def checked_file(root: Path, relative: str) -> Path:
    root = root.absolute()
    target = root / str(safe_relative(relative))
    path = root
    for part in [None, *safe_relative(relative).parts]:
        path = path if part is None else path / part
        if path.is_symlink() or (path.exists() and getattr(path.lstat(), "st_file_attributes", 0) & 0x400):
            raise DeliveryError("Linked/reparse input rejected: " + relative)
    if not within(target, root) or not target.is_file():
        raise DeliveryError("Missing/unsafe actual input file: " + relative)
    return target


def verify_file_manifest(root: Path, manifest: Path, expected: str, label: str) -> dict:
    require_sha(expected)
    if digest(manifest) != expected:
        raise DeliveryError(label + " manifest SHA256 mismatch")
    data = small_json(manifest)
    files = manifest_entries(data, label)
    root = root.resolve()
    for item in files:
        target = checked_file(root, item["path"])
        if "bytes" in item and target.stat().st_size != item["bytes"]:
            raise DeliveryError(label + " actual file byte count differs: " + item["path"])
        if digest(target) != item["sha256"]:
            raise DeliveryError(label + " actual file hash differs: " + item["path"])
    return {"manifestSha256": expected, "root": str(root), "filesVerified": len(files),
            "data": data, "paths": sorted(item["path"] for item in files)}


def compiler_file_api_query(build: Path) -> None:
    query = build / ".cmake/api/v1/query"
    query.mkdir(parents=True, exist_ok=True)
    (query / "codemodel-v2").touch()


def verify_cmake_target(build: Path, target_name: str, source: Path, approved_paths: set[str]) -> Path:
    replies = build / ".cmake/api/v1/reply"
    indexes = sorted(replies.glob("index-*.json"))
    if not indexes:
        raise DeliveryError("Real CMake file API reply missing")
    index = small_json(indexes[-1])
    reply = index.get("reply", {}).get("codemodel-v2", {})
    model = small_json(replies / reply["jsonFile"])
    configurations = model.get("configurations", [])
    if not configurations:
        raise DeliveryError("CMake target model has no configuration")
    targets = {entry["id"]: entry for entry in configurations[0]["targets"]}
    roots = [entry for entry in targets.values() if entry["name"] == target_name]
    if len(roots) != 1:
        raise DeliveryError("The exact production target is missing from actual CMake codemodel")
    pending, visited, main = [roots[0]], set(), None
    while pending:
        target = pending.pop()
        if target["id"] in visited:
            continue
        visited.add(target["id"])
        data = small_json(replies / target["jsonFile"])
        if target["name"] == target_name:
            main = data
        for item in data.get("sources", []):
            path = Path(item["path"])
            path = path if path.is_absolute() else source / path
            if within(path, source):
                relative = path.resolve().relative_to(source.resolve()).as_posix()
                if relative not in approved_paths:
                    raise DeliveryError("CMake consumed an unapproved source/material: " + relative)
            elif not within(path, build):
                raise DeliveryError("CMake target source escapes approved source/build roots")
        for dependency in data.get("dependencies", []):
            if dependency["id"] in targets:
                pending.append(targets[dependency["id"]])
    if main is None or main.get("type") != "EXECUTABLE":
        raise DeliveryError("Production target is not a real executable")
    artifacts = [build / item["path"] for item in main.get("artifacts", [])]
    bundle = next((Path(str(path).split(".app/", 1)[0] + ".app") for path in artifacts if ".app/" in str(path)), None)
    if bundle is None:
        raise DeliveryError("Production owner target has no MACOSX_BUNDLE artifact; report owner instead of wrapping a fake bundle")
    return bundle


def production_target(build: Path, source: Path, requested: str | None) -> str:
    replies = build / ".cmake/api/v1/reply"
    indexes = sorted(replies.glob("index-*.json"))
    if not indexes:
        raise DeliveryError("Actual production CMake codemodel required")
    index = small_json(indexes[-1])
    model = small_json(replies / index["reply"]["codemodel-v2"]["jsonFile"])
    candidates = []
    for entry in model["configurations"][0]["targets"]:
        data = small_json(replies / entry["jsonFile"])
        paths = []
        for item in data.get("sources", []):
            path = Path(item["path"])
            path = path if path.is_absolute() else source / path
            if within(path, source):
                paths.append(path.resolve().relative_to(source).as_posix())
        if data.get("type") == "EXECUTABLE" and "native/app/main.cpp" in paths and any(".app/" in item["path"] for item in data.get("artifacts", [])):
            candidates.append(entry["name"])
    if len(candidates) != 1 or (requested is not None and requested != candidates[0]):
        raise DeliveryError("Expected one actual MACOSX_BUNDLE production target consuming native/app/main.cpp")
    return candidates[0]


def asset_oracle_root(stage: Path, candidate: Path | None = None, manifest_paths: set[str] | None = None) -> Path:
    """Return the absolute trusted ASSET_ORACLE_ROOT for the brand resource tests.

    Fails closed unless the root is a real directory inside the verified source
    stage that holds every required brand oracle file and, when a frozen source
    manifest is supplied, until that manifest pins each file's exact bytes."""
    root = stage if candidate is None else candidate
    if not isinstance(root, Path):
        raise DeliveryError("Trusted ASSET_ORACLE_ROOT must be a filesystem path")
    if not root.is_absolute():
        raise DeliveryError("Trusted ASSET_ORACLE_ROOT must be an absolute trusted checkout path")
    if root.is_symlink():
        raise DeliveryError("Trusted ASSET_ORACLE_ROOT must not be a linked path")
    resolved = root.resolve()
    if not resolved.is_dir():
        raise DeliveryError("Trusted ASSET_ORACLE_ROOT directory is missing: " + str(root))
    if not within(resolved, stage):
        raise DeliveryError("Trusted ASSET_ORACLE_ROOT must stay inside the verified source stage: " + str(root))
    for required in BRAND_ORACLE_REQUIRED:
        if manifest_paths is not None and required not in manifest_paths:
            raise DeliveryError("Verified source manifest omits the brand oracle file: " + required)
        try:
            checked_file(resolved, required)
        except DeliveryError as error:
            raise DeliveryError("Trusted ASSET_ORACLE_ROOT is missing " + required) from error
    return resolved


def production_options(source: Path, paths: list[str], data: dict) -> dict[str, str | bool]:
    definitions = dict(data.get("cmakeDefinitions", {}))
    declarations = {}
    for relative in paths:
        if relative.endswith("CMakeLists.txt") or relative.endswith(".cmake"):
            text = checked_file(source, relative).read_text(encoding="utf-8-sig")
            for match in re.finditer(r'option\(\s*(NICKEL_[A-Z0-9_]+)\s+"[^"]*"\s+(ON|OFF)\s*\)', text, re.I):
                declarations[match[1]] = match[2]
    for key in declarations:
        if re.search(r"(?:BUILD|ENABLE)_(?:APP|UI|TESTS|DOMAINS)$", key):
            definitions[key] = True
        if key.endswith("REQUIRE_STATIC_QT"):
            definitions[key] = False
    return definitions


def qt_cmake_options(prefix: Path, modules: tuple[str, ...]) -> list[str]:
    config = checked_file(prefix, "lib/cmake/Qt6/Qt6Config.cmake")
    options = [f"-DQt6_DIR={config.parent}"]
    for module in modules:
        if not re.fullmatch(r"[A-Za-z0-9]+", module):
            raise DeliveryError("Unsafe Qt CMake module name")
        module_config = checked_file(prefix, f"lib/cmake/Qt6{module}/Qt6{module}Config.cmake")
        options.append(f"-DQt6{module}_DIR={module_config.parent}")
    return options


def fixture_python(options: argparse.Namespace, environment: dict) -> Path:
    supplied = getattr(options, "test_python", None)
    if supplied is None:
        raise DeliveryError("Explicit independent fixture Python interpreter required")
    # Do not resolve the venv executable symlink into the base interpreter.
    python = Path(supplied).absolute()
    if not python.is_file():
        raise DeliveryError("Actual independent fixture Python interpreter is missing")
    for variable in ("PYTHONHOME", "PYTHONPATH", "PYTHONUSERBASE"):
        environment.pop(variable, None)
    environment.update(PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1")
    return python


def build_app(options: argparse.Namespace, inputs: dict, contract: dict) -> dict:
    environment, host = apple_environment(inputs["policy"], options.arch)
    source = options.source_root.resolve()
    source_manifest = verify_file_manifest(source, options.production_manifest, options.production_sha256, "Production source")
    data = source_manifest["data"]
    oracle = asset_oracle_root(source, manifest_paths=set(source_manifest["paths"]))
    required_sources = {"CMakeLists.txt", "native/app/main.cpp", "native/app/AppServices.cpp", "native/ui/MainWindow.cpp", "native/tests/ui/CMakeLists.txt"}
    if not required_sources.issubset(source_manifest["paths"]):
        raise DeliveryError("Frozen source must contain the full real root/AppServices/MainWindow/test entrypoints")
    core = verify_file_manifest(options.core_prefix, options.core_manifest, options.core_sha256, "Core dependency prefix")
    if core["data"].get("architecture") != options.arch or core["data"].get("deploymentTarget") != "12.0" or core["data"].get("sourceContractSha256") != options.contract_sha256 or core["data"].get("supplementSha256") != contract.get("supplementSha256"):
        raise DeliveryError("Core prefix is not bound to the same native policy/source/supplement")
    qt = small_json(options.qt_evidence)
    if not qt.get("complete") or qt.get("qtVersion") != QT_VERSION or qt.get("architecture") != options.arch or qt.get("sourceContract", {}).get("contractSha256") != options.contract_sha256:
        raise DeliveryError("Exact native Qt build evidence/source binding missing")
    prefix = Path(qt["prefix"])
    if not prefix.is_dir():
        raise DeliveryError("Qt source-built prefix missing")
    build = options.build_root.resolve()
    if build.exists():
        raise DeliveryError("Application build directory must be fresh and owned")
    build.mkdir(parents=True)
    compiler_file_api_query(build)
    environment.update(QT_QPA_PLATFORM="cocoa", QT_PLUGIN_PATH=str(prefix / "plugins"), DYLD_FRAMEWORK_PATH=str(prefix / "lib"))
    recorder = Recorder(options.output, {"kind": "production-source-build", "host": host, "architecture": options.arch,
                                        "sourceContract": contract, "buildRoot": str(build), "sourceRoot": str(source),
                                        "assetOracleRoot": str(oracle),
                                        "resolvedSourceCommit": data.get("resolvedSourceCommit"),
                                        "productionManifest": {key: value for key, value in source_manifest.items() if key != "data"},
                                        "coreManifest": {key: value for key, value in core.items() if key != "data"}})
    test_python = fixture_python(options, environment)
    try:
        cmake = shutil.which("cmake")
        if not cmake:
            raise DeliveryError("Verified CMake missing")
        arguments = [cmake, "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                     f"-DCMAKE_OSX_ARCHITECTURES={options.arch}", "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0",
                     f"-DCMAKE_OSX_SYSROOT={environment['SDKROOT']}", f"-DCMAKE_PREFIX_PATH={prefix};{options.core_prefix.resolve()}",
                     "-DCMAKE_CXX_COMPILER=/usr/bin/clang++", "-DCMAKE_C_COMPILER=/usr/bin/clang",
                     "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -g", "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -g", "-DBUILD_TESTING=ON",
                     f"-DASSET_ORACLE_ROOT={oracle}",
                     "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DOPENSSL_USE_STATIC_LIBS=ON", f"-DOPENSSL_ROOT_DIR={options.core_prefix.resolve()}",
                     "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
                     "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local", f"-DPython3_EXECUTABLE={test_python}"]
        arguments.extend(qt_cmake_options(prefix, ("Core", "Gui", "Widgets", "Svg", "Test")))
        definitions = production_options(source, source_manifest["paths"], data)
        for key, value in definitions.items():
            if not re.fullmatch(r"NICKEL_[A-Z0-9_]+", key) or not isinstance(value, (str, bool)):
                raise DeliveryError("Production CMake definitions must be explicit non-secret NICKEL_ configuration")
            arguments.append("-D" + key + "=" + ("ON" if value is True else "OFF" if value is False else value))
        recorder.run(arguments, source, environment)
        cache = parse_cache(build / "CMakeCache.txt")
        for key, expected in {"CMAKE_OSX_ARCHITECTURES": options.arch, "CMAKE_OSX_DEPLOYMENT_TARGET": "12.0", "CMAKE_BUILD_TYPE": "Release"}.items():
            if cache.get(key) != expected:
                raise DeliveryError("Production build altered mandatory policy: " + key)
        target = production_target(build, source, options.target)
        bundle = verify_cmake_target(build, target, source, set(source_manifest["paths"]))
        graph = generated_installation_graph(build)
        recorder.record["installationGraph"] = graph
        preserve_installation_graph(build, graph, options.output.parent / "generated-install")
        recorder.save()
        recorder.run([cmake, "--build", str(build), "--parallel", str(options.jobs)], source, environment)
        discovered = recorder.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"], source, environment, 60)
        tests = json.loads(discovered).get("tests", [])
        registered = {item["name"] for item in tests}
        required = data.get("requiredCTestNames", sorted(registered))
        ui_tests = [item for item in tests if any("native/tests/ui" in str(prop.get("value", "")).replace("\\", "/") for prop in item.get("properties", []))]
        if not required or not set(required).issubset(registered) or not ui_tests:
            raise DeliveryError("Actual production/UI CTest matrix missing; zero tests or toolkit-only tests cannot pass")
        recorder.run(["ctest", "--test-dir", str(build), "--output-on-failure", "--output-junit", str(build / "ctest.xml")], source, environment, 1800)
        if not bundle.is_dir():
            raise DeliveryError("The actual owner target did not produce its declared .app")
        with checked_file(bundle, "Contents/Info.plist").open("rb") as stream:
            executable = checked_file(bundle, "Contents/MacOS/" + str(safe_relative(plistlib.load(stream)["CFBundleExecutable"])))
        compile_commands = checked_file(build, "compile_commands.json")
        recorder.finish(bundle=str(bundle), productionTarget=target, executable=str(executable), executableSha256=digest(executable),
                        compileCommandsSha256=digest(compile_commands), installationGraph=generated_installation_graph(build),
                        sourceInputManifestSha256=options.production_sha256, qtEvidenceSha256=digest(options.qt_evidence),
                        productDelivered=False, requiredTestsExecuted=required, ownerUiTestsExecuted=[item["name"] for item in ui_tests],
                        ctestScope="canonical-root-tests-on-native-cocoa; route/functional-semantic-review-still-required", appleCoreTestsExecuted=True)
    except Exception as error:
        recorder.record["failure"] = str(error)
        recorder.save()
        raise
    return recorder.record


def generated_installation_graph(build: Path) -> dict:
    checked_file(build, "cmake_install.cmake")
    scripts = sorted(path for path in build.rglob("*.cmake")
                     if path.name == "cmake_install.cmake" or "deploy" in path.name.lower())
    if not scripts or len(scripts) > MAX_FILES or not any("deploy" in path.name.lower() for path in scripts):
        raise DeliveryError("Actual generated Qt/application install graph is missing or oversized")
    files = []
    for path in scripts:
        relative = path.relative_to(build).as_posix()
        path = checked_file(build, relative)
        if path.stat().st_size > MAX_SMALL_INPUT:
            raise DeliveryError("Generated install script exceeds bounded input size")
        files.append({"path": relative, "bytes": path.stat().st_size, "sha256": digest(path)})
    return {"schema": 1, "kind": "actual-generated-application-qt-install-graph", "files": files}


def preserve_installation_graph(build: Path, graph: dict, output: Path) -> None:
    if output.exists() or output.is_symlink():
        raise DeliveryError("Generated install evidence directory must be fresh")
    output.mkdir(parents=True)
    for item in graph["files"]:
        source = checked_file(build, item["path"])
        if digest(source) != item["sha256"] or source.stat().st_size != item["bytes"]:
            raise DeliveryError("Generated install graph changed while preserving raw scripts")
        target = output / item["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)


def installed_production_bundle(build: Path, prefix: Path, original_bundle: Path) -> tuple[Path, dict]:
    manifest = checked_file(build, "install_manifest.txt")
    if manifest.stat().st_size > MAX_SMALL_INPUT:
        raise DeliveryError("Actual CMake installation manifest exceeds bound")
    entries = manifest.read_text(encoding="utf-8").splitlines()
    if not entries or len(entries) > MAX_FILES:
        raise DeliveryError("Actual CMake installation manifest is empty or oversized")
    prefix = prefix.absolute()
    bundles, installed = set(), set()
    for entry in entries:
        path = Path(entry)
        if not path.is_absolute() or ".." in path.parts or not within(path, prefix) or not path.exists():
            raise DeliveryError("CMake install graph wrote missing or out-of-prefix material: " + entry)
        try:
            relative = path.absolute().relative_to(prefix)
        except ValueError as error:
            raise DeliveryError("Install manifest path is not lexically under the product prefix") from error
        installed.add(relative.as_posix())
        for index, part in enumerate(relative.parts):
            if part.endswith(".app"):
                bundles.add(prefix.joinpath(*relative.parts[:index + 1]))
                break
    original_plist = checked_file(original_bundle, "Contents/Info.plist")
    with original_plist.open("rb") as stream:
        original_info = plistlib.load(stream)
    executable_name = str(safe_relative(original_info.get("CFBundleExecutable")))
    if "/" in executable_name or original_info.get("CFBundleIdentifier") != "studio.nickel.launcher":
        raise DeliveryError("Actual build bundle has an invalid production identity")
    identity = ("CFBundleIdentifier", "CFBundleExecutable", "CFBundleVersion", "CFBundleShortVersionString")
    matches = []
    for bundle in sorted(bundles):
        with checked_file(bundle, "Contents/Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
        if all(info.get(key) == original_info.get(key) for key in identity):
            matches.append(bundle)
    if len(matches) != 1:
        raise DeliveryError("Install manifest must identify exactly one actual production app bundle")
    bundle = matches[0]
    plist_path = checked_file(bundle, "Contents/Info.plist")
    executable = checked_file(bundle, "Contents/MacOS/" + executable_name)
    if any(path.relative_to(prefix).as_posix() not in installed for path in (plist_path, executable)):
        raise DeliveryError("Installed production executable/Info.plist is absent from the real install manifest")
    return bundle, {"path": str(manifest), "bytes": manifest.stat().st_size, "sha256": digest(manifest),
                    "installedPathCount": len(installed), "prefix": str(prefix), "bundle": str(bundle),
                    "buildInfoPlistSha256": digest(original_plist), "installedInfoPlistSha256": digest(plist_path),
                    "productionIdentity": {key: original_info.get(key) for key in identity}}


def deployed_plugin_evidence(bundle: Path, prefix: Path, audit: dict, policy: dict) -> list[dict]:
    format_plugins = {"jpeg": "qjpeg", "gif": "qgif", "svg": "qsvg", "webp": "qwebp", "tiff": "qtiff", "icns": "qicns"}
    required = {"platforms/libqcocoa.dylib", "iconengines/libqsvgicon.dylib", "imageformats/libqico.dylib"}
    required.update("imageformats/lib" + format_plugins[format_name] + ".dylib"
                    for format_name in policy["requiredImageFormats"] if format_name in format_plugins)
    available = set()
    for category in ("imageformats", "iconengines"):
        directory = prefix / "plugins" / category
        if not directory.is_dir() or directory.is_symlink():
            raise DeliveryError("Actual source-built Qt plugin category missing or linked: " + category)
        available.update(path.relative_to(prefix / "plugins").as_posix() for path in directory.glob("*.dylib"))
    if not (required - {"platforms/libqcocoa.dylib"}).issubset(available):
        raise DeliveryError("Source-built Qt prefix is missing required image-format/SvgIcon plugins")
    required.update(available)
    nodes = {item["path"]: item for item in audit["nodes"]}
    records = []
    for relative in sorted(required):
        source = checked_file(prefix, "plugins/" + relative)
        deployed = "Contents/PlugIns/" + relative
        path = checked_file(bundle, deployed)
        if not macho(source) or deployed not in nodes or digest(path) != nodes[deployed]["sha256"] or nodes[deployed].get("architecture") != [audit.get("architecture")]:
            raise DeliveryError("Required installed Qt plugin lacks audited actual Mach-O bytes/architecture: " + relative)
        records.append({"plugin": relative, "sourcePath": str(source), "sourceSha256": digest(source),
                        "installedPath": deployed, "installedSha256": nodes[deployed]["sha256"],
                        "installedArchitecture": nodes[deployed]["architecture"], "runtimePluginLoadExecuted": False})
    return records


def macho_uuid_pairs(text: str) -> set[tuple[str, str]]:
    pairs = {(identity.upper(), architecture) for identity, architecture in re.findall(
        r"UUID: ([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}) \(([^)]+)\)", text)}
    if not pairs or any(architecture not in ARCHITECTURES for _, architecture in pairs):
        raise DeliveryError("Actual Mach-O UUID/architecture output is missing or invalid")
    return pairs


def deployed_symbol_evidence(bundle: Path, build_bundle: Path, qt_prefix: Path, output: Path,
                             recorder: Recorder, environment: dict) -> list[dict]:
    if output.exists() or output.is_symlink():
        raise DeliveryError("dSYM directory must be new and separate from the runtime bundle")
    output.mkdir(parents=True)
    _, images = bundle_inventory(bundle)
    _, original_images = bundle_inventory(build_bundle)
    candidates = set(original_images)
    for directory in (qt_prefix / "lib", qt_prefix / "plugins"):
        for index, path in enumerate(directory.rglob("*")):
            if index >= MAX_FILES:
                raise DeliveryError("Source-built Qt debug image inventory exceeds bound")
            if path.is_file() and within(path, qt_prefix) and macho(path):
                candidates.add(path.resolve())
    source_uuids = {}
    records = []
    for index, image in enumerate(images):
        binary_uuids = macho_uuid_pairs(recorder.run(["xcrun", "dwarfdump", "--uuid", str(image)], bundle.parent, environment, 30))
        matches = []
        for source in sorted(candidate for candidate in candidates if candidate.name == image.name):
            if source not in source_uuids:
                source_uuids[source] = macho_uuid_pairs(recorder.run(["xcrun", "dwarfdump", "--uuid", str(source)], bundle.parent, environment, 30))
            if source_uuids[source] == binary_uuids:
                matches.append(source)
        if len(matches) != 1:
            raise DeliveryError("Installed Mach-O must match one unstripped source-built image by UUID/architecture")
        source = matches[0]
        symbol = output / (f"{index:04}-" + image.name + ".dSYM")
        # Public Qt install may strip its copy; use debug information from the recorded compilation.
        recorder.run(["xcrun", "dsymutil", str(source), "-o", str(symbol)], bundle.parent, environment, 300)
        symbol_uuids = macho_uuid_pairs(recorder.run(["xcrun", "dwarfdump", "--uuid", str(symbol)], bundle.parent, environment, 30))
        dwarf = list((symbol / "Contents/Resources/DWARF").glob("*"))
        if binary_uuids != symbol_uuids or len(dwarf) != 1 or not dwarf[0].is_file() or dwarf[0].is_symlink() or dwarf[0].stat().st_size < 1024:
            raise DeliveryError("dSYM UUID/architecture or meaningful DWARF differs from installed Mach-O")
        debug_info = recorder.run(["xcrun", "dwarfdump", "--debug-info", "--recurse-depth=0", str(symbol)], bundle.parent, environment, 120)
        compilation_units = debug_info.count("DW_TAG_compile_unit")
        if not compilation_units or "DW_AT_name" not in debug_info:
            raise DeliveryError("dSYM has no actual source compilation units; UUID-only skeleton cannot pass")
        records.append({"image": image.relative_to(bundle.resolve()).as_posix(), "imageSha256": digest(image),
                        "debugSourceImage": str(source), "debugSourceImageSha256": digest(source),
                        "debugSourceKind": "production-build-bundle" if within(source, build_bundle) else "source-built-qt-prefix",
                        "debugSourceUuidMatchesDeployed": True, "dSYM": str(symbol), "uuids": sorted(binary_uuids),
                        "actualCompilationUnits": compilation_units,
                        "compilationUnitLog": recorder.record["commands"][-1]["log"],
                        "compilationUnitLogSha256": recorder.record["commands"][-1]["logSha256"],
                        "dwarfFiles": [{"name": path.name, "bytes": path.stat().st_size, "sha256": digest(path)} for path in dwarf]})
    return records


def deploy(options: argparse.Namespace, inputs: dict) -> dict:
    environment, host = apple_environment(inputs["policy"], options.arch)
    for variable in ("DESTDIR", "CMAKE_INSTALL_MODE"):
        environment.pop(variable, None)
    source = small_json(options.build_evidence)
    if not source.get("complete") or source.get("kind") != "production-source-build" or source.get("architecture") != options.arch:
        raise DeliveryError("Deployment requires actual completed production source-build evidence")
    build_bundle = Path(source["bundle"]).resolve()
    built_executable = Path(source["executable"])
    if not within(built_executable, build_bundle) or digest(built_executable) != require_sha(source["executableSha256"]):
        raise DeliveryError("Actual production EXE SHA256 changed after the recorded build")
    build = Path(source["buildRoot"])
    graph = generated_installation_graph(build)
    if graph != source.get("installationGraph"):
        raise DeliveryError("Actual generated install graph changed after the production build")
    if digest(checked_file(build, "compile_commands.json")) != require_sha(source.get("compileCommandsSha256")):
        raise DeliveryError("Actual compilation commands changed after the recorded source build")
    qt = small_json(options.qt_evidence)
    prefix = Path(qt["prefix"])
    deployer = checked_file(prefix, "bin/macdeployqt")
    if not qt.get("complete") or qt.get("architecture") != options.arch or qt.get("qtVersion") != QT_VERSION or digest(options.qt_evidence) != source["qtEvidenceSha256"]:
        raise DeliveryError("Deployment must use the exact native source-built Qt evidence bound to this EXE")
    install_root = options.install_root.absolute()
    if install_root.exists() or install_root.is_symlink() or any(within(install_root, path) or within(path, install_root) for path in (build, prefix, Path(source["sourceRoot"]))):
        raise DeliveryError("Product installation prefix must be fresh and outside build/source/dependency prefixes")
    cmake = shutil.which("cmake")
    if not cmake:
        raise DeliveryError("Verified CMake is required to consume the actual application install graph")
    recorder = Recorder(options.output, {"kind": "macos-production-deployment", "host": host,
                                        "architecture": options.arch, "productionBuildSha256": digest(options.build_evidence),
                                        "resolvedSourceCommit": source.get("resolvedSourceCommit"),
                                        "qtBuildSha256": digest(options.qt_evidence), "builtExecutableSha256": source["executableSha256"],
                                        "installationGraph": graph})
    try:
        recorder.run([cmake, "--install", str(build), "--prefix", str(install_root), "--config", "Release"], build, environment, 900)
        bundle, installation = installed_production_bundle(build, install_root, build_bundle)
        with checked_file(bundle, "Contents/Info.plist").open("rb") as stream:
            executable = checked_file(bundle, "Contents/MacOS/" + str(safe_relative(plistlib.load(stream)["CFBundleExecutable"])))
        recorder.record["installation"] = installation
        shutil.copyfile(build / "install_manifest.txt", options.output.parent / "installation-manifest.txt")
        recorder.save()
        recorder.run([str(deployer), str(bundle), "-no-strip", "-verbose=2", "-codesign=-"], bundle.parent, environment, 600)
        audit = audit_bundle(bundle, options.arch, "12.0", recorder, environment, signed=True)
        plugins = deployed_plugin_evidence(bundle, prefix, audit, inputs["policy"])
        symbols = deployed_symbol_evidence(bundle, build_bundle, prefix, options.symbols_root, recorder, environment)
        if digest(built_executable) != source["executableSha256"]:
            raise DeliveryError("Installation/deployment must not mutate the production build executable")
        archive = options.archive
        if archive.exists() or archive.is_symlink():
            raise DeliveryError("Package destination already exists")
        recorder.run(["ditto", "-c", "-k", "--keepParent", "--sequesterRsrc", str(bundle), str(archive)], bundle.parent, environment, 300)
        recorder.finish(bundle=str(bundle), executable=str(executable), executableSha256=digest(executable), bundleAudit=audit,
                        installedPlugins=plugins, symbols=symbols,
                        package={"path": str(archive), "sha256": digest(archive), "bytes": archive.stat().st_size},
                        signing="ad-hoc-test-only", notarizationExecuted=False, gatekeeperAccepted=False,
                        appleRuntimeExecuted=False, productDelivered=False)
    except Exception as error:
        recorder.record["failure"] = str(error)
        recorder.save()
        raise
    return recorder.record


def material_evidence(options: argparse.Namespace) -> dict:
    deployment = small_json(options.deployment_evidence)
    if not deployment.get("complete") or deployment.get("kind") != "macos-production-deployment":
        raise DeliveryError("Materials must bind to actual macOS production deployment")
    verified = verify_file_manifest(options.material_root, options.material_manifest, options.material_sha256, "Corresponding source/legal")
    manifest = verified["data"]
    roles = {item.get("role") for item in manifest.get("files", [])}
    required = {"application-corresponding-source", "dependency-source", "build-and-relink-instructions",
                "sbom", "license-notices", "installation-information"}
    if not required.issubset(roles):
        raise DeliveryError("Matching source/SBOM/license/build/installation materials missing")
    if manifest.get("deploymentEvidenceSha256") != digest(options.deployment_evidence):
        raise DeliveryError("Legal/source materials do not identify this exact shipped deployment")
    return {"schema": 1, "kind": "actual-material-hash-verification", "executed": True,
            "deploymentEvidenceSha256": digest(options.deployment_evidence),
            "materialManifestSha256": options.material_sha256, "filesVerified": verified["filesVerified"],
            "rolesVerified": sorted(roles), "legalSufficiencyOpinion": False,
            "productDelivered": False, "appleRuntimeExecuted": False}


def run_probes(options: argparse.Namespace, inputs: dict, contract: dict) -> dict:
    environment, host = apple_environment(inputs["policy"], options.arch)
    qt = small_json(options.qt_evidence)
    if not qt.get("complete") or qt.get("qtVersion") != QT_VERSION or qt.get("architecture") != options.arch or qt.get("sourceContract", {}).get("contractSha256") != options.contract_sha256:
        raise DeliveryError("Probes require the actual exact native Qt build/source contract")
    prefix = Path(qt["prefix"])
    core = verify_file_manifest(options.core_prefix, options.core_manifest, options.core_sha256, "Core dependency prefix")
    if core["data"].get("architecture") != options.arch or core["data"].get("sourceContractSha256") != options.contract_sha256:
        raise DeliveryError("Probe prefix is not bound to the native architecture/source contract")
    source = options.source_root.resolve()
    verify_file_manifest(source, options.production_manifest, options.production_sha256, "Probe consumed source")
    build = options.build_root.resolve()
    if build.exists():
        raise DeliveryError("Probe build directory must be new and isolated")
    build.mkdir(parents=True)
    environment.update(QT_QPA_PLATFORM="cocoa", QT_PLUGIN_PATH=str(prefix / "plugins"), DYLD_FRAMEWORK_PATH=str(prefix / "lib"))
    recorder = Recorder(options.output, {"kind": "actual-native-apple-synthetic-probes", "host": host,
                                        "sourceContract": contract, "sourceManifestSha256": options.production_sha256,
                                        "architecture": options.arch, "qtEvidenceSha256": digest(options.qt_evidence), "productDelivered": False})
    try:
        cmake = shutil.which("cmake")
        if not cmake:
            raise DeliveryError("Verified CMake missing")
        test_python = fixture_python(options, environment)
        recorder.run([cmake, "-S", str(REPOSITORY / "tests/native-macos"), "-B", str(build), "-G", "Ninja",
                      "-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DCMAKE_OSX_ARCHITECTURES={options.arch}",
                      "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0", f"-DCMAKE_OSX_SYSROOT={environment['SDKROOT']}",
                      f"-DCMAKE_PREFIX_PATH={prefix};{options.core_prefix.resolve()}", f"-DNICKEL_MACOS_SOURCE_ROOT={source}",
                      "-DNICKEL_MACOS_INCLUDE_OWNER_FOUNDATION_TESTS=OFF", f"-DOPENSSL_ROOT_DIR={options.core_prefix.resolve()}",
                      "-DCMAKE_CXX_COMPILER=/usr/bin/clang++", f"-DPython3_EXECUTABLE={test_python}",
                      *qt_cmake_options(prefix, ("Core", "Gui", "Widgets", "Svg"))], source, environment)
        recorder.run([cmake, "--build", str(build), "--parallel", str(options.jobs)], source, environment)
        recorder.run(["ctest", "--test-dir", str(build), "--output-on-failure", "--output-junit", str(build / "ctest.xml")], source, environment, 1800)
        ui_records = []
        for scale in ("1", "1.25", "1.5", "2"):
            path = build / ("ui-probe-" + scale + ".json")
            value = small_json(path)
            if value.get("kind") != "real-apple-native-cocoa-toolkit-probe" or not value.get("executedOnApple"):
                raise DeliveryError("Native Cocoa probe result missing or misclassified")
            ui_records.append({"path": str(path), "sha256": digest(path), "scale": scale,
                               "platform": value.get("platformPlugin"), "windowDpr": value.get("windowDpr")})
        recorder.finish(appleRuntimeExecuted=True, testScope="synthetic-foundation-and-canonical-Theme-toolkit-only",
                        uiRecords=ui_records, productDelivered=False, productionLayoutVerified=False, fullMigrationVerified=False,
                        actualMinecraftVerified=False, minimumMacos12RuntimeVerified=False, physicalDpiImeAccessibilityVerified=False)
    except Exception as error:
        recorder.record["failure"] = str(error)
        recorder.save()
        raise
    return recorder.record


def verify_runtime_receipt(receipt: Path, expected: str, deployment: dict) -> dict:
    require_sha(expected)
    if digest(receipt) != expected:
        raise DeliveryError("Runtime receipt hash mismatch")
    value = small_json(receipt)
    audit = deployment["bundleAudit"]
    if value.get("schema") != 1 or value.get("kind") != "actual-apple-production-runtime":
        raise DeliveryError("CI design/toolkit probes/cross syntax cannot be a production runtime receipt")
    if value.get("host", {}).get("system") != "Darwin" or value.get("host", {}).get("nativeArchitecture") != deployment["architecture"]:
        raise DeliveryError("Receipt is not from a native matching Apple host")
    if not value.get("executed") or not value.get("processId") or value.get("platformPlugin") != "cocoa":
        raise DeliveryError("Actual production pid/native Cocoa runtime evidence missing")
    main = next((item for item in audit["nodes"] if item["path"].startswith("Contents/MacOS/")), None)
    if main is None or value.get("binarySha256") != main["sha256"]:
        raise DeliveryError("Production runtime binary does not match the deployed Mach-O hash")
    images = value.get("loadedImages", [])
    if not images:
        raise DeliveryError("Actual process loaded-image list is required; otool -L is not runtime evidence")
    bundled_paths = {item["path"]: item["sha256"] for item in audit["nodes"]}
    loaded = set()
    for item in images:
        name = item["path"]
        if FORBIDDEN.search(name):
            raise DeliveryError("Runtime process loaded a forbidden browser/network/sidecar image")
        if system_library(name):
            continue
        relative = item.get("bundleRelativePath")
        if relative not in bundled_paths or item.get("sha256") != bundled_paths[relative]:
            raise DeliveryError("Process loaded an external/development or mismatched non-system image")
        loaded.add(relative)
    if not any(path.endswith("libqcocoa.dylib") for path in loaded):
        raise DeliveryError("The process did not load the deployed native Cocoa plugin")
    required = {"routes-seven-topbar-four", "themes-12-light-dark", "font-chinese-emoji-metrics",
                "dpi-100-125-150-200", "legacy-whole-root", "first-run-version-migration",
                "game-owned-lifetime", "vault-0600-key-race", "no-data-writes-in-app",
                "image-formats-body-native-ui"}
    scenarios = {item["id"]: item for item in value.get("scenarios", [])}
    if not required.issubset(scenarios):
        raise DeliveryError("The actual production runtime scenario matrix is incomplete")
    for identity in required:
        item = scenarios[identity]
        if item.get("status") != "executed-pass" or not item.get("evidenceSha256") or not item.get("evidencePath"):
            raise DeliveryError("Production runtime scenario is not evidenced: " + identity)
        evidence = receipt.parent / str(safe_relative(item["evidencePath"]))
        if not within(evidence, receipt.parent) or not evidence.is_file() or digest(evidence) != require_sha(item["evidenceSha256"]):
            raise DeliveryError("Runtime scenario evidence hash/path mismatch")
    return {"receiptSha256": expected, "nativeArchitecture": deployment["architecture"],
            "productionRuntimeEvidenceVerified": True, "scenarioCount": len(required),
            "loadedNonSystemImagesVerified": len(loaded), "independentScenarioSemanticReview": False,
            "minimumMacos12RuntimeVerified": value.get("host", {}).get("osVersion", "").startswith("12.")}




def build_core(options: argparse.Namespace, inputs: dict, contract: dict) -> dict:
    import runpy
    module = runpy.run_path(str(HERE / "macos-delivery.core.py"))
    return module["build_core"](options, inputs, contract, globals())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    host = sub.add_parser("discover", help="Read only non-sensitive host metadata")
    host.add_argument("--output", required=True, type=Path)
    source_commands = ("verify-inputs", "verify-sources", "fetch-sources", "setup-tools", "build-qt", "build-core", "build-app", "run-probes", "verify-production")
    for name in source_commands:
        command = sub.add_parser(name)
        command.add_argument("--inputs", type=Path, default=HERE / "macos-delivery.inputs.json")
        command.add_argument("--dependency-lock", required=True, type=Path)
        command.add_argument("--source-contract", required=True, type=Path)
        command.add_argument("--contract-sha256", required=True)
        command.add_argument("--verification-rules", type=Path)
        command.add_argument("--supplement", type=Path)
        command.add_argument("--supplement-sha256")
        command.add_argument("--output", required=True, type=Path)
        if name in ("verify-sources", "fetch-sources", "setup-tools", "build-qt", "build-core"):
            command.add_argument("--downloads", required=True, type=Path)
        if name == "fetch-sources":
            command.add_argument("--include-tools", action="store_true")
            command.add_argument("--include-core", action="store_true")
        if name in ("setup-tools", "build-qt", "build-core"):
            command.add_argument("--tools-root", required=True, type=Path)
        if name in ("build-qt", "build-core", "build-app", "run-probes"):
            command.add_argument("--arch", required=True, choices=ARCHITECTURES)
            command.add_argument("--jobs", type=int, default=2, choices=range(1, 5))
        if name in ("build-core", "build-app", "run-probes", "verify-production"):
            command.add_argument("--source-root", required=True, type=Path)
            command.add_argument("--production-manifest", required=True, type=Path)
            command.add_argument("--production-sha256", required=True)
        if name == "build-core":
            command.add_argument("--prefix-manifest", required=True, type=Path)
        if name in ("build-app", "run-probes"):
            command.add_argument("--core-prefix", required=True, type=Path)
            command.add_argument("--core-manifest", required=True, type=Path)
            command.add_argument("--core-sha256", required=True)
            command.add_argument("--qt-evidence", required=True, type=Path)
            command.add_argument("--build-root", required=True, type=Path)
            command.add_argument("--test-python", required=True, type=Path, help="Pinned independent fixture interpreter; CI verifies its environment evidence first")
        if name == "build-app":
            command.add_argument("--target")
    deploy_command = sub.add_parser("deploy")
    deploy_command.add_argument("--inputs", type=Path, default=HERE / "macos-delivery.inputs.json")
    deploy_command.add_argument("--arch", required=True, choices=ARCHITECTURES)
    deploy_command.add_argument("--build-evidence", required=True, type=Path)
    deploy_command.add_argument("--qt-evidence", required=True, type=Path)
    deploy_command.add_argument("--install-root", required=True, type=Path, help="Fresh product prefix for cmake --install of the recorded production build")
    deploy_command.add_argument("--symbols-root", required=True, type=Path)
    deploy_command.add_argument("--archive", required=True, type=Path)
    deploy_command.add_argument("--output", required=True, type=Path)
    materials = sub.add_parser("verify-materials")
    materials.add_argument("--deployment-evidence", required=True, type=Path)
    materials.add_argument("--material-root", required=True, type=Path)
    materials.add_argument("--material-manifest", required=True, type=Path)
    materials.add_argument("--material-sha256", required=True)
    materials.add_argument("--output", required=True, type=Path)
    runtime = sub.add_parser("verify-runtime")
    runtime.add_argument("--deployment-evidence", required=True, type=Path)
    runtime.add_argument("--receipt", required=True, type=Path)
    runtime.add_argument("--receipt-sha256", required=True)
    runtime.add_argument("--output", required=True, type=Path)
    options = parser.parse_args()
    try:
        if options.output.exists():
            raise DeliveryError("New evidence destination required; do not let a failed attempt leave an old pass")
        if options.command == "discover":
            record = discover()
        elif options.command == "verify-materials":
            record = material_evidence(options)
        elif options.command == "verify-runtime":
            deployment = small_json(options.deployment_evidence)
            if not deployment.get("complete"):
                raise DeliveryError("Actual completed deployment evidence required")
            record = {"schema": 1, "kind": "production-runtime-receipt-hash-verification", "executedUtc": utc_now(),
                      **verify_runtime_receipt(options.receipt, options.receipt_sha256, deployment),
                      "appleRuntimeExecuted": False, "productDelivered": False}
        elif options.command == "deploy":
            record = deploy(options, validate_inputs(options.inputs))
        else:
            inputs = validate_inputs(options.inputs)
            inputs["_path"] = str(options.inputs)
            contract = verify_source_contract(options.dependency_lock, options.source_contract,
                                              options.contract_sha256, inputs, options.verification_rules,
                                              options.supplement, options.supplement_sha256)
            if options.command == "build-qt":
                record = build_qt(options, inputs, contract)
            elif options.command == "build-core":
                record = build_core(options, inputs, contract)
            elif options.command == "build-app":
                record = build_app(options, inputs, contract)
            elif options.command == "run-probes":
                record = run_probes(options, inputs, contract)
            elif options.command == "fetch-sources":
                record = fetch_sources(options, inputs, contract)
            elif options.command == "setup-tools":
                record = setup_tools(options, inputs, contract)
            else:
                record = {"schema": 1, "executedUtc": utc_now(), "kind": options.command,
                          "sourceContract": contract, "appleRuntimeExecuted": False, "productDelivered": False}
                if options.command == "verify-sources":
                    record["archives"] = verify_archives(inputs, options.downloads)
                if options.command == "verify-production":
                    verified = verify_file_manifest(options.source_root, options.production_manifest,
                                                    options.production_sha256, "Production source")
                    record["productionSource"] = {key: value for key, value in verified.items() if key != "data"}
                    record["productionSourceBytesVerified"] = True
        write_json(options.output, record)
        print(json.dumps({"evidence": str(options.output), "kind": record.get("kind", "host-discovery"),
                          "appleRuntimeExecuted": record.get("appleRuntimeExecuted", False)}))
        return 0
    except (DeliveryError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        failure = {"schema": 1, "executedUtc": utc_now(), "kind": options.command,
                   "complete": False, "productDelivered": False, "appleRuntimeExecuted": False,
                   "failure": str(error), "hostSystem": platform.system()}
        if not options.output.exists():
            write_json(options.output, failure)
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
