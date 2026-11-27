#!/usr/bin/env python3
"""Capture real host capabilities without reading private config or starting downloads."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(argv: list[str], timeout: int = 30) -> dict:
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    try:
        result = subprocess.run(argv, capture_output=True, timeout=timeout, check=False)
        def decode(data: bytes) -> str:
            encoding = "utf-16-le" if data and data.count(b"\x00") > len(data) // 5 else "utf-8"
            return data.decode(encoding, errors="replace")
        return {"argv": argv, "startedUtc": started, "exitCode": result.returncode,
                "stdout": decode(result.stdout), "stderr": decode(result.stderr)}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"argv": argv, "startedUtc": started, "exitCode": None, "error": str(exc)}


def wsl_storage() -> list[dict]:
    if os.name != "nt":
        return []
    import winreg
    result = []
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Lxss") as key:
            for index in range(winreg.QueryInfoKey(key)[0]):
                with winreg.OpenKey(key, winreg.EnumKey(key, index)) as child:
                    name = winreg.QueryValueEx(child, "DistributionName")[0]
                    base = winreg.QueryValueEx(child, "BasePath")[0]
                    result.append({"distribution": name, "basePath": base,
                                   "backingDrive": Path(base.removeprefix("\\\\?\\")).drive})
    except OSError as exc:
        result.append({"error": str(exc)})
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--distribution", default="Ubuntu-24.04")
    args = parser.parse_args()
    commands = []
    if os.name == "nt" and shutil.which("wsl.exe"):
        commands.extend([["wsl.exe", "--list", "--verbose"], ["wsl.exe", "--status"]])
        linux_commands = [
            ["uname", "-a"], ["cat", "/etc/os-release"], ["id"],
            ["c++", "--version"], ["cmake", "--version"], ["ninja", "--version"],
            ["python3", "--version"], ["pkg-config", "--modversion", "openssl"],
            ["pkg-config", "--modversion", "libcurl"],
            ["pkg-config", "--modversion", "libarchive"],
            ["pkg-config", "--modversion", "Qt6Widgets"],
            ["dpkg-query", "-W", "build-essential", "cmake", "ninja-build",
             "nlohmann-json3-dev", "libssl-dev", "libcurl4-openssl-dev", "libarchive-dev",
             "qt6-base-dev"],
            ["df", "-h", "/", "/mnt/c", "/mnt/d"],
        ]
        commands.extend([["wsl.exe", "-d", args.distribution, "--exec", *cmd]
                         for cmd in linux_commands])
    if shutil.which("docker.exe"):
        commands.extend([["docker.exe", "version", "--format", "{{json .}}"],
                         ["docker.exe", "context", "ls"],
                         ["docker.exe", "images", "--format", "{{.Repository}}:{{.Tag}} {{.ID}} {{.Size}}"]])
    if platform.system() == "Darwin":
        commands.extend([["sw_vers"], ["xcrun", "--show-sdk-path"], ["xcrun", "clang++", "--version"]])
    payload = {"schema": 1, "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
               "probeSha256": sha256(Path(__file__)), "python": sys.version,
               "platform": platform.platform(), "cwd": str(Path.cwd()),
               "wslStorage": wsl_storage(), "commands": [capture(cmd) for cmd in commands],
               "macos": {"verified": platform.system() == "Darwin",
                         "scope": "Only this local host is probed; no remote Mac is configured or inferred."}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "sha256": sha256(args.output),
                      "commands": len(payload["commands"]), "wslStorage": payload["wslStorage"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
