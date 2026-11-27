"""Independent evidence checks; none of these checks creates launcher trust."""
from __future__ import annotations

import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import unicodedata
import zipfile

GIB = 1024**3
MIB = 1024**2
MAX_IMAGE = 512 * MIB
MAX_NOTICE = 4 * MIB
MAX_MEMBERS = 4096
MAX_PACKAGE = 1024 * MIB
REQUIRED_NOTICES = frozenset({"LICENSE", "LICENSE-ASSETS", "NOTICE.md", "SOURCE.txt"})
HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


class Rejected(ValueError):
    def __init__(self, code: str, detail: str):
        super().__init__(detail)
        self.code = code


def require(condition: bool, code: str, detail: str) -> None:
    if not condition:
        raise Rejected(code, detail)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def strict_json(data: bytes | str, maximum: int = 64 * 1024):
    if isinstance(data, bytes):
        require(len(data) <= maximum, "limit", "JSON byte budget exceeded")
        data = data.decode("utf-8", errors="strict")
    require(len(data.encode("utf-8")) <= maximum, "limit", "JSON byte budget exceeded")

    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate_key", "Duplicate JSON key: " + key)
            result[key] = value
        return result

    def constant(value):
        raise Rejected("invalid_schema", "Non-finite JSON constant: " + value)

    return json.loads(data, object_pairs_hook=pairs, parse_constant=constant)


def member_name(name: str, directory: bool = False) -> str:
    require(isinstance(name, str) and bool(name), "unsafe_path", "Empty archive path")
    if directory and name.endswith("/"):
        name = name[:-1]
    require(len(name.encode("utf-8")) <= 1024, "limit", "Archive name budget exceeded")
    require(not name.startswith("/") and "\\" not in name, "unsafe_path", "Absolute or backslash archive path")
    for part in name.split("/"):
        require(part not in {"", ".", ".."} and not part.endswith((".", " ")), "unsafe_path", "Unsafe archive component")
        require(not any(ord(c) < 32 or ord(c) == 127 or c in ':*?<>|"' for c in part), "unsafe_path", "Unsafe archive character")
        stem = part.split(".", 1)[0].rstrip(" ").casefold()
        require(stem not in {"con", "prn", "aux", "nul", "clock$", "conin$", "conout$"}, "unsafe_path", "Reserved device path")
        require(not re.fullmatch(r"(?:com|lpt)[1-9\u00b9\u00b2\u00b3]", stem), "unsafe_path", "Reserved numbered device path")
    return name


def notice_name(name: str) -> str:
    checked = member_name(name)
    require(checked in REQUIRED_NOTICES or checked.startswith("LICENSES/"), "unsafe_content", "Notice is outside embedded whitelist")
    return checked


def regular_owned_path(path: Path, root: Path | None = None) -> Path:
    path = Path(os.path.abspath(path))
    if root is not None:
        root = Path(os.path.abspath(root))
        require(path != root and path.is_relative_to(root), "unauthorized", "Path escaped owned root")
    current = path
    while True:
        status = current.lstat()
        require(not stat.S_ISLNK(status.st_mode) and not getattr(status, "st_file_attributes", 0) & 0x400,
                "unsafe_path", "Symlink/reparse path is not an owned file")
        if current.parent == current:
            break
        current = current.parent
    info = path.stat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "unsafe_path", "Expected a one-link regular file")
    return path


def _pinned_fd(path: Path) -> int:
    if os.name != "nt":
        return os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    import msvcrt
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                  ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    native = "\\\\?\\" + str(path) if not str(path).startswith("\\\\") else "\\\\?\\UNC\\" + str(path)[2:]
    handle = kernel.CreateFileW(native, 0x80000000, 1, None, 3, 0x00200000, None)
    require(handle not in {None, ctypes.c_void_p(-1).value}, "sharing_denied", "Cannot pin input against mutation")
    try:
        return msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except BaseException:
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle(handle)
        raise


def _fd_key(fd: int) -> tuple:
    info = os.fstat(fd)
    modified, changed = info.st_mtime_ns, info.st_ctime_ns
    if os.name == "nt":
        import msvcrt

        class BasicInfo(ctypes.Structure):
            _fields_ = [("creation", ctypes.c_int64), ("access", ctypes.c_int64),
                        ("write", ctypes.c_int64), ("change", ctypes.c_int64),
                        ("attributes", ctypes.c_uint32)]

        basic = BasicInfo()
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetFileInformationByHandleEx.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
        kernel.GetFileInformationByHandleEx.restype = ctypes.c_int
        require(bool(kernel.GetFileInformationByHandleEx(msvcrt.get_osfhandle(fd), 0, ctypes.byref(basic), ctypes.sizeof(basic))),
                "io", "Cannot read native file revision")
        require(not basic.attributes & (0x400 | 0x10), "unsafe_path", "Native file became reparse/directory")
        # Python Windows stat/fstat expose different creation/change-time semantics.
        # Use FILE_BASIC_INFO ChangeTime on both handles, not a tolerance or omission.
        modified, changed = basic.write * 100, basic.change * 100
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "unsafe_path", "Pinned input is not one-link regular")
    return info.st_dev, info.st_ino, info.st_size, modified, changed, info.st_nlink


def file_snapshot(path: Path, root: Path | None = None, maximum: int = MAX_IMAGE) -> dict:
    """Read with Windows no-write/no-delete sharing, then recheck path identity."""
    path = regular_owned_path(path, root)
    parent_before = path.parent.stat()
    with os.fdopen(_pinned_fd(path), "rb") as source:
        before = _fd_key(source.fileno())
        require(before[2] <= maximum, "limit", "Input is not a bounded regular image")
        digest = hashlib.sha256()
        size = 0
        while block := source.read(64 * 1024):
            size += len(block)
            require(size <= maximum, "limit", "Input grew beyond budget")
            digest.update(block)
        after = _fd_key(source.fileno())
        require(size == before[2] and before == after, "source_changed", "Input changed during read")
        with os.fdopen(_pinned_fd(path), "rb") as current:
            require(_fd_key(current.fileno()) == after, "source_changed", "Input path changed identity/revision")
        parent_after = path.parent.stat()
        require((parent_before.st_dev, parent_before.st_ino) == (parent_after.st_dev, parent_after.st_ino),
                "source_changed", "Input parent changed identity")
    return {"path": str(path), "volume": before[0], "index": before[1], "bytes": size,
            "modifiedNs": before[3], "changedNs": before[4],
            "parentVolume": parent_before.st_dev, "parentIndex": parent_before.st_ino, "sha256": digest.hexdigest()}


def unchanged(expected: dict, path: Path, root: Path | None = None) -> dict:
    current = file_snapshot(path, root)
    require(current == expected, "source_changed", "Captured source identity, revision or SHA256 changed")
    return current


def pe_image(data: bytes, expected_arch: str | None = None) -> dict:
    require(64 <= len(data) <= MAX_IMAGE and data[:2] == b"MZ", "wrong_type", "Not a DOS/PE image")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    require(64 <= offset <= len(data) - 24 and data[offset:offset + 4] == b"PE\x00\x00", "wrong_type", "PE header offset/signature is invalid")
    machine, sections, _, _, _, optional_size, flags = struct.unpack_from("<HHIIIHH", data, offset + 4)
    arch = {0x8664: "x86_64", 0xAA64: "arm64", 0x014C: "x86"}.get(machine)
    require(arch is not None, "wrong_arch", "PE machine is not an approved Windows architecture")
    require(flags & 0x0002 and not flags & (0x2000 | 0x1000), "wrong_type", "Not a non-DLL user executable")
    require(1 <= sections <= 96 and optional_size >= 96, "wrong_type", "PE section/optional header is invalid")
    optional = offset + 24
    require(optional + optional_size + sections * 40 <= len(data), "wrong_type", "PE table is truncated")
    magic = struct.unpack_from("<H", data, optional)[0]
    require(magic == (0x010B if arch == "x86" else 0x020B), "wrong_type", "PE optional header and machine disagree")
    subsystem = struct.unpack_from("<H", data, optional + 68)[0]
    require(subsystem in {2, 3}, "wrong_type", "Not a user-mode Windows GUI/console executable")
    for index in range(sections):
        section = optional + optional_size + index * 40
        size, raw = struct.unpack_from("<II", data, section + 16)
        require(raw <= len(data) and size <= len(data) - raw, "wrong_type", "PE section exceeds image bytes")
    if expected_arch is not None:
        require(arch == expected_arch, "wrong_arch", "PE differs from compiled architecture")
    return {"os": "windows", "arch": arch, "type": "pe-executable", "subsystem": subsystem,
            "bytes": len(data), "sha256": sha256_bytes(data)}


def portable_manifest(value: dict, arch: str | None = None) -> dict:
    required = {"schemaVersion", "format", "packageId", "name", "version", "instanceName", "payload", "gameDirectory", "dataDirectory"}
    require(isinstance(value, dict) and required <= value.keys() <= required | {"launcher", "personalization"},
            "invalid_schema", "Portable marker field set is invalid")
    require(type(value["schemaVersion"]) is int and value["schemaVersion"] == 1 and value["format"] == "nickel-portable",
            "invalid_schema", "Expected canonical schema 1")
    require(isinstance(value["packageId"], str) and re.fullmatch(r"[0-9a-fA-F]{32}", value["packageId"]) is not None,
            "invalid_schema", "Invalid package identity")
    for field in ("name", "version", "instanceName"):
        text = value[field]
        require(isinstance(text, str) and bool(text.strip()) and len(text.encode("utf-8")) <= 512
                and not any(ord(c) < 32 or ord(c) == 127 for c in text), "invalid_schema", "Invalid portable text")
    member_name(value["instanceName"])
    require("/" not in value["instanceName"], "unsafe_path", "Instance name is not a single leaf")
    require(value["payload"] == "Nickel/pack.mrpack" and value["gameDirectory"] == ".minecraft" and value["dataDirectory"] == "Nickel",
            "unsafe_path", "Schema 1 layout must remain canonical")
    require(value.get("personalization") in {None, "Nickel/personalization.json"}, "unsafe_path", "Invalid personalization path")
    launcher = value.get("launcher")
    if launcher is not None:
        require(isinstance(launcher, dict) and set(launcher) == {"path", "os", "arch"}, "invalid_schema", "Invalid launcher field set")
        require(launcher["os"] == "windows" and launcher["path"] == "Nickel.exe", "wrong_os", "Expected schema 1 Windows launcher")
        require(launcher["arch"] in {"x86_64", "arm64", "x86"}, "wrong_arch", "Unknown Windows launcher arch")
        if arch is not None:
            require(launcher["arch"] == arch, "wrong_arch", "Portable launcher differs from compiled architecture")
    return value


def zip_members(archive: zipfile.ZipFile) -> dict[str, zipfile.ZipInfo]:
    entries = archive.infolist()
    require(len(entries) <= MAX_MEMBERS, "limit", "Archive member budget exceeded")
    result = {}
    folded = set()
    total = 0
    for entry in entries:
        name = member_name(entry.filename, entry.is_dir())
        key = unicodedata.normalize("NFC", name).casefold()
        require(key not in folded, "conflict", "Duplicate/case-colliding archive member")
        folded.add(key)
        mode = entry.external_attr >> 16
        require(not stat.S_ISLNK(mode) and stat.S_IFMT(mode) in {0, stat.S_IFREG, stat.S_IFDIR}, "unsafe_path", "Archive special member")
        require(not entry.flag_bits & 1 and entry.compress_type in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}, "archive", "Unsupported/encrypted ZIP member")
        require(0 <= entry.file_size <= MAX_IMAGE and (not entry.is_dir() or entry.file_size == 0), "limit", "Archive file budget exceeded")
        total += entry.file_size
        require(total <= MAX_PACKAGE, "limit", "Archive total budget exceeded")
        result[name] = entry
    for name in result:
        parts = name.split("/")
        for end in range(1, len(parts)):
            ancestor = "/".join(parts[:end])
            require(ancestor not in result or result[ancestor].is_dir(), "conflict", "Archive file/directory collision")
    return result


def read_member(archive: zipfile.ZipFile, entry: zipfile.ZipInfo, maximum: int) -> bytes:
    require(entry.file_size <= maximum, "limit", "Member exceeds read budget")
    with archive.open(entry) as source:
        data = source.read(maximum + 1)
    require(len(data) == entry.file_size and len(data) <= maximum, "archive", "Member has inconsistent bytes/CRC")
    return data


def audit_package(package: Path, actual_image: Path, compiled_arch: str, embedded_notices: dict[str, str],
                  root: Path | None = None) -> dict:
    """Compare an export to externally pinned actualPd bytes; this is not a trust factory."""
    require(set(embedded_notices) >= REQUIRED_NOTICES, "missing_service", "Compiled legal hash inventory is incomplete")
    for name, digest in embedded_notices.items():
        notice_name(name)
        require(isinstance(digest, str) and HEX64.fullmatch(digest) is not None, "invalid_schema", "Invalid embedded notice hash")
    image = file_snapshot(actual_image, root)
    package_before = file_snapshot(package, root, MAX_PACKAGE)
    with Path(actual_image).open("rb") as source:
        actual = source.read(MAX_IMAGE + 1)
    image_type = pe_image(actual, compiled_arch)
    require(image_type["sha256"] == image["sha256"], "source_changed", "actualPd changed between pin/type reads")
    with zipfile.ZipFile(package) as archive:
        members = zip_members(archive)
        require("nickel.portable.json" in members, "missing_service", "Portable marker is absent")
        marker = portable_manifest(strict_json(read_member(archive, members["nickel.portable.json"], 64 * 1024)), compiled_arch)
        require(marker.get("launcher") is not None, "missing_service", "Export does not contain a launcher")
        launcher = marker["launcher"]["path"]
        require(launcher in members, "missing_service", "Launcher archive member is absent")
        copied = read_member(archive, members[launcher], MAX_IMAGE)
        pe_image(copied, compiled_arch)
        require(sha256_bytes(copied) == image["sha256"], "integrity", "Portable launcher differs from actualPd image")
        names = {name for name, entry in members.items() if not entry.is_dir()}
        allowed = {launcher, "nickel.portable.json", "Nickel/pack.mrpack", "README-portable.txt",
                   "Nickel/instance-settings.json", "Nickel/personalization.json"} | set(embedded_notices)
        allowed |= {"Nickel/background." + ext for ext in ("png", "jpg", "jpeg", "webp", "gif", "bmp", "avif")}
        require(names <= allowed, "unsafe_content", "Unapproved file in portable envelope")
        require(REQUIRED_NOTICES <= names and set(embedded_notices) <= names, "missing_service", "Required embedded notices are absent")
        for name, digest in embedded_notices.items():
            require(sha256_bytes(read_member(archive, members[name], MAX_NOTICE)) == digest,
                    "integrity", "Notice differs from compiled embedded bytes: " + name)
        require("Nickel/pack.mrpack" in names, "missing_service", "Portable payload is absent")
        payload = read_member(archive, members["Nickel/pack.mrpack"], MAX_IMAGE)
        import io
        with zipfile.ZipFile(io.BytesIO(payload)) as pack:
            payload_members = zip_members(pack)
            require("modrinth.index.json" in payload_members, "missing_service", "mrpack index is absent")
            index = strict_json(read_member(pack, payload_members["modrinth.index.json"], 1024 * 1024), 1024 * 1024)
            require(isinstance(index, dict) and index.get("name") == marker["name"] and index.get("versionId") == marker["version"],
                    "integrity", "Payload name/version differs from portable marker")
    unchanged(image, actual_image, root)
    require(file_snapshot(package, root, MAX_PACKAGE) == package_before, "source_changed", "Package changed during audit")
    return {"schema": "pd.trusted-export.package-audit.v1", "status": "PASS", "scope": "independent-package-audit-only",
            "productionTrustMinted": False, "actualImage": image, "imageType": image_type, "package": package_before,
            "manifest": marker, "embeddedNoticeHashes": embedded_notices, "payloadSha256": sha256_bytes(payload)}


def capacity_gate(c_free: int, d_free: int) -> None:
    require(c_free >= GIB and d_free >= 256 * MIB, "capacity", "C>=1GiB and D>=256MiB are required")


def aggregate(cases: list[dict], required: list[str]) -> dict:
    require(len(required) == len(set(required)) and bool(required), "invalid_schema", "Case inventory is empty/duplicated")
    seen = {}
    for case in cases:
        require(isinstance(case, dict) and case.get("id") in required and case["id"] not in seen,
                "invalid_schema", "Unexpected or repeated case result")
        require(case.get("status") in {"PASS", "FAIL", "NOT_RUN"}, "invalid_schema", "Unknown case status")
        if case["status"] == "PASS":
            require(case.get("executed") is True and case.get("assertions", 0) > 0,
                    "invalid_evidence", "PASS without executed assertions")
        seen[case["id"]] = case
    output = [seen.get(name, {"id": name, "status": "NOT_RUN", "executed": False, "assertions": 0,
                             "reason": "Required case was not observed"}) for name in required]
    counts = {status: sum(item["status"] == status for item in output) for status in ("PASS", "FAIL", "NOT_RUN")}
    status = "FAIL" if counts["FAIL"] else "NOT_RUN" if counts["NOT_RUN"] else "PASS"
    return {"status": status, "counts": counts, "cases": output}


def write_new_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, indent=2, ensure_ascii=True, allow_nan=False)
        output.write("\n")
