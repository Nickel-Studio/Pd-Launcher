"""Audit actual PE imports and optional observed modules; never launch a binary."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import struct


# Exact Windows API DLLs and minimum OS evidence: windows-system-imports.json.
# This import policy never exempts app-local DLLs from the runtime path audit.
SYSTEM_DLLS = frozenset({
    "advapi32.dll", "authz.dll", "avrt.dll", "bcrypt.dll", "bcryptprimitives.dll", "cabinet.dll", "cfgmgr32.dll",
    "clbcatq.dll", "combase.dll", "comctl32.dll", "comdlg32.dll", "crypt32.dll", "cryptbase.dll",
    "cryptnet.dll", "cryptsp.dll", "cryptui.dll", "d2d1.dll", "d3d11.dll", "d3d12.dll", "d3d9.dll",
    "dbghelp.dll", "devobj.dll", "dnsapi.dll", "dwmapi.dll", "dwrite.dll", "dxgi.dll", "dxva2.dll",
    "gdi32.dll", "gdi32full.dll", "hid.dll", "imagehlp.dll", "imm32.dll", "iphlpapi.dll",
    "kernel32.dll", "kernelbase.dll", "ksuser.dll", "mf.dll", "mfcore.dll", "mfplat.dll", "mfreadwrite.dll",
    "mfuuid.dll", "mpr.dll", "msimg32.dll", "msvcrt.dll", "ncrypt.dll", "netapi32.dll", "normaliz.dll", "ntasn1.dll",
    "ntdll.dll", "ole32.dll", "oleacc.dll", "oleaut32.dll", "opengl32.dll", "pdh.dll", "powrprof.dll",
    "propsys.dll", "psapi.dll", "rasapi32.dll", "rpcrt4.dll", "secur32.dll", "setupapi.dll",
    "shcore.dll", "shell32.dll", "shlwapi.dll", "sspicli.dll", "ucrtbase.dll", "uiautomationcore.dll", "urlmon.dll", "user32.dll",
    "userenv.dll", "usp10.dll", "uxtheme.dll", "version.dll", "win32u.dll", "windowscodecs.dll",
    "winhttp.dll", "wininet.dll", "winmm.dll", "winscard.dll", "winspool.drv", "wintrust.dll", "wldap32.dll",
    "ws2_32.dll", "wtsapi32.dll", "winsta.dll", "dhcpcsvc.dll", "dhcpcsvc6.dll", "mswsock.dll", "xmllite.dll",
})
API_SET = re.compile(r"(?:api|ext)-ms-[a-z0-9-]+\.dll\Z", re.IGNORECASE)
# Only Rust needs a prefix boundary; WebView must also match msedgewebview2.
FORBIDDEN = re.compile(
    r"webview|webengine|qt[0-9]*(?:qml|quick|webengine)|javascript|libnode|"
    r"(?:^|[_.-])(?:node(?=\.exe\Z)|(?:lib)?rust)|tauri", re.IGNORECASE)
DYNAMIC_CRT = re.compile(r"(?:vcruntime|msvcp|concrt|vcomp)\d+[^/\\]*\.dll\Z", re.IGNORECASE)


class PeFormatError(ValueError):
    pass


class PeImage:
    def __init__(self, data: bytes):
        self.data = data
        self._require(0, 64)
        if data[:2] != b"MZ":
            raise PeFormatError("Missing DOS signature")
        pe = self.u32(0x3C)
        self._require(pe, 24)
        if data[pe:pe + 4] != b"PE\0\0":
            raise PeFormatError("Missing PE signature")
        self.machine = self.u16(pe + 4)
        count = self.u16(pe + 6)
        if not 1 <= count <= 512:
            raise PeFormatError("Invalid section count")
        opt = pe + 24
        opt_size = self.u16(pe + 20)
        self._require(opt, opt_size)
        magic = self.u16(opt)
        if magic == 0x20B:
            self.bitness = 64
            minimum, count_at, directory_at = 112, 108, 112
            self.image_base = self.u64(opt + 24)
        elif magic == 0x10B:
            self.bitness = 32
            minimum, count_at, directory_at = 96, 92, 96
            self.image_base = self.u32(opt + 28)
        else:
            raise PeFormatError("Unsupported optional header")
        if opt_size < minimum:
            raise PeFormatError("Truncated optional header")
        self.header_size = self.u32(opt + 60)
        if self.header_size == 0 or self.header_size > len(data):
            raise PeFormatError("Invalid header size")
        self.subsystem = self.u16(opt + 68)
        directory_count = self.u32(opt + count_at)
        if directory_count > (opt_size - directory_at) // 8:
            raise PeFormatError("Data directory exceeds optional header")
        self.directories = [struct.unpack_from("<II", data, opt + directory_at + index * 8)
                            for index in range(directory_count)]
        table = opt + opt_size
        self._require(table, count * 40)
        self.sections = []
        for index in range(count):
            at = table + index * 40
            virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, at + 8)
            if raw_size:
                self._require(raw, raw_size)
            self.sections.append((rva, max(virtual_size, raw_size), raw, raw_size))

    def _require(self, at: int, size: int) -> None:
        if at < 0 or size < 0 or at > len(self.data) - size:
            raise PeFormatError("PE field lies outside file")

    def u16(self, at: int) -> int:
        self._require(at, 2)
        return struct.unpack_from("<H", self.data, at)[0]

    def u32(self, at: int) -> int:
        self._require(at, 4)
        return struct.unpack_from("<I", self.data, at)[0]

    def u64(self, at: int) -> int:
        self._require(at, 8)
        return struct.unpack_from("<Q", self.data, at)[0]

    def offset(self, rva: int, size: int = 1) -> int:
        if rva < 0 or size < 1 or rva > 0xFFFFFFFF - size + 1:
            raise PeFormatError("Invalid RVA range")
        if rva < self.header_size:
            if rva + size > self.header_size:
                raise PeFormatError("RVA crosses header boundary")
            self._require(rva, size)
            return rva
        matches = [section for section in self.sections if section[0] <= rva < section[0] + section[1]]
        if len(matches) != 1:
            raise PeFormatError("RVA is absent or ambiguous")
        start, _, raw, raw_size = matches[0]
        relative = rva - start
        if relative + size > raw_size:
            raise PeFormatError("RVA lies in an unbacked virtual region")
        at = raw + relative
        self._require(at, size)
        return at

    def dll_name(self, rva: int) -> str:
        characters = bytearray()
        for index in range(260):
            at = self.offset(rva + index)
            byte = self.data[at]
            if byte == 0:
                break
            characters.append(byte)
        else:
            raise PeFormatError("Unterminated import library name")
        try:
            name = characters.decode("ascii")
        except UnicodeDecodeError as error:
            raise PeFormatError("Non-ASCII import library name") from error
        if not name or not re.fullmatch(r"[a-zA-Z0-9_.-]+", name):
            raise PeFormatError("Invalid import library name")
        return name.lower()

    def imports(self, delay: bool = False) -> list[str]:
        directory = 13 if delay else 1
        if len(self.directories) <= directory:
            return []
        rva, size = self.directories[directory]
        if rva == size == 0:
            return []
        stride = 32 if delay else 20
        if not rva or size < stride or size // stride > 8192:
            raise PeFormatError("Malformed import directory")
        result = []
        for index in range(size // stride):
            at = self.offset(rva + index * stride, stride)
            fields = struct.unpack_from("<" + "I" * (stride // 4), self.data, at)
            if not any(fields):
                return result
            if delay:
                attributes, name_at = fields[:2]
                if attributes not in (0, 1):
                    raise PeFormatError("Unknown delay-load descriptor attributes")
                name_rva = name_at if attributes == 1 else name_at - self.image_base
            else:
                name_rva = fields[3]
            if not name_rva:
                raise PeFormatError("Import descriptor has no name")
            result.append(self.dll_name(name_rva))
        raise PeFormatError("Import descriptor table lacks terminator")


def system_import(name: str) -> bool:
    return name in SYSTEM_DLLS or API_SET.fullmatch(name) is not None


def check_imports(normal: list[str], delay: list[str]) -> list[str]:
    failures = []
    for name in sorted(set(normal + delay)):
        if FORBIDDEN.search(name):
            failures.append(f"Forbidden runtime component: {name}")
        elif DYNAMIC_CRT.fullmatch(name):
            failures.append(f"Third-party compiler runtime requires a separate DLL: {name}")
        elif not system_import(name):
            failures.append(f"Non-system import requires verification/static linking: {name}")
    return failures


def within(path: str, root: str) -> bool:
    parsed, base = PureWindowsPath(path), PureWindowsPath(root)
    if not parsed.is_absolute() or not base.is_absolute() or ".." in parsed.parts:
        return False
    return parsed.is_relative_to(base)


def audit_modules(evidence: dict, executable: str, executable_sha256: str, windows_root: str) -> list[str]:
    failures = []
    if evidence.get("schemaVersion") != 1 or evidence.get("executableSha256") != executable_sha256:
        failures.append("Observed-module evidence does not identify this exact executable")
    snapshots = evidence.get("snapshots", [])
    if not snapshots:
        failures.append("Observed-module evidence contains no runtime snapshots")
    expected_exe = PureWindowsPath(executable)
    allowed_roots = [str(PureWindowsPath(windows_root) / "System32"),
                     str(PureWindowsPath(windows_root) / "SysWOW64"),
                     str(PureWindowsPath(windows_root) / "WinSxS")]
    for index, snapshot in enumerate(snapshots):
        if not snapshot.get("scenario") or not isinstance(snapshot.get("pid"), int) or snapshot["pid"] <= 0:
            failures.append(f"Snapshot {index} lacks a concrete scenario/PID")
        modules = snapshot.get("modules", [])
        if not modules:
            failures.append(f"Snapshot {index} has no observed modules")
        found_exe = False
        for raw in modules:
            path = PureWindowsPath(raw)
            name = path.name.lower()
            if path == expected_exe:
                found_exe = True
                continue
            if FORBIDDEN.search(name):
                failures.append(f"Snapshot {index} loads forbidden component: {raw}")
            elif DYNAMIC_CRT.fullmatch(name):
                failures.append(f"Snapshot {index} loads a dynamic compiler runtime: {raw}")
            elif not any(within(raw, root) for root in allowed_roots):
                failures.append(f"Snapshot {index} loads a non-system file: {raw}")
        if not found_exe:
            failures.append(f"Snapshot {index} omits the executable module")
        for child in snapshot.get("children", []):
            purpose = child.get("purpose")
            if purpose not in {"game", "java-probe", "installer", "browser", "user-prelaunch-command"}:
                failures.append(f"Snapshot {index} contains an unexplained launcher child: {child.get('executable', '')}")
            if FORBIDDEN.search(PureWindowsPath(child.get("executable", "")).name):
                failures.append(f"Snapshot {index} contains a forbidden child component")
    return failures


def audit(executable: Path, module_evidence: Path | None = None, windows_root: str | None = None) -> dict:
    if executable.is_symlink() or not executable.is_file():
        raise ValueError("Executable must be an exact regular file")
    data = executable.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    pe = PeImage(data)
    normal, delay = pe.imports(), pe.imports(delay=True)
    import_failures = check_imports(normal, delay)
    runtime_failures = []
    runtime_status = "unverified"
    if module_evidence is not None:
        evidence = json.loads(module_evidence.read_text(encoding="utf-8-sig"))
        runtime_failures = audit_modules(evidence, str(executable.resolve()), digest,
                                        windows_root or os.environ.get("SystemRoot", "C:\\Windows"))
        runtime_status = "failed" if runtime_failures else "passed-observed-scenarios"
    return {"schemaVersion": 1, "kind": "windows-native-artifact-audit", "executable": str(executable.resolve()),
            "sha256": digest, "bytes": len(data), "machine": hex(pe.machine), "bitness": pe.bitness,
            "subsystem": pe.subsystem, "imports": normal, "delayImports": delay,
            "importGate": "failed" if import_failures else "passed",
            "runtimeGate": runtime_status, "failures": import_failures + runtime_failures,
            "limitations": ["PE imports alone do not prove absence of late DLL loads, extraction or browser engines.",
                            "Runtime evidence covers only the named observed scenarios, not all launcher functionality.",
                            "Clean-machine deployment, visual fidelity and complete performance are separate gates."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--modules", type=Path)
    parser.add_argument("--windows-root")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-runtime", action="store_true")
    args = parser.parse_args()
    result = audit(args.executable, args.modules, args.windows_root)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 1 if result["failures"] or (args.require_runtime and args.modules is None) else 0


if __name__ == "__main__":
    raise SystemExit(main())
