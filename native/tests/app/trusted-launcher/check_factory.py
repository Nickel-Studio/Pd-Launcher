"""Build real factory negatives from immutable headers; never mint a production receipt."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys

WORKTREE = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(WORKTREE / "scripts" / "native"))
from msvc import developer_environment


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-commit", required=True)
    options = parser.parse_args()
    if os.name != "nt" or not re.fullmatch(r"[0-9a-f]{40}", options.input_commit):
        parser.error("A Windows host and a complete immutable input commit are required")
    build_base = WORKTREE / "native" / "tests" / "app" / "build-trusted-launcher"
    if shutil.disk_usage(Path(os.environ.get("SystemDrive", "C:")) / "\\").free < 1024**3:
        raise RuntimeError("Factory checks require 1 GiB on the system drive")
    if shutil.disk_usage(WORKTREE).free < 256 * 1024**2:
        raise RuntimeError("Factory checks require 256 MiB on the worktree volume")
    output = build_base / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    inputs = output / "inputs"
    inputs.mkdir()
    sources: dict[str, bytes] = {}

    def git_blob(path: str) -> bytes | None:
        result = subprocess.run(["git", "show", options.input_commit + ":" + path],
                                cwd=WORKTREE, capture_output=True, timeout=30)
        return result.stdout if result.returncode == 0 else None

    def capture(path: str) -> bytes:
        if path in sources:
            return sources[path]
        data = git_blob(path)
        if data is None:
            raise FileNotFoundError(options.input_commit + ":" + path)
        sources[path] = data
        destination = inputs / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            raise FileExistsError(destination)
        destination.write_bytes(data)
        for include in re.findall(r'^\s*#\s*include\s*"([^"\r\n]+)"', data.decode("utf-8"), re.MULTILINE):
            candidates = [str(PurePosixPath(path).parent / include), "native/" + include]
            for candidate in candidates:
                if candidate in sources or git_blob(candidate) is not None:
                    capture(candidate)
                    break
            else:
                raise FileNotFoundError("Unresolved immutable include " + path + " -> " + include)
        return data

    capture("native/core/packs/packs.hpp")
    capture("native/core/tasks/secure_fs.hpp")
    capture("native/core/network/hash.hpp")
    capture("native/platform/process.hpp")
    metadata_bytes = capture("native/metadata.json")
    metadata = json.loads(metadata_bytes)
    environment = developer_environment()
    temporary = WORKTREE / "native" / "tests" / "app" / ".temp"
    temporary.mkdir(exist_ok=True)
    environment["TEMP"] = environment["TMP"] = str(temporary)
    environment["VSLANG"] = "1033"
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    compiler = Path(environment["VCToolsInstallDir"]) / "bin" / "Hostx64" / "x64" / "cl.exe"
    linker = compiler.with_name("link.exe")
    prefix = Path(r"D:\workon\nickel-native-tools\install\windows-x64-static")
    if not compiler.is_file() or not linker.is_file() or not (prefix / "include").is_dir():
        raise RuntimeError("The real MSVC compiler and installed dependencies are required")
    owned = [WORKTREE / "native/app/service/trusted_launcher.cpp",
             WORKTREE / "native/app/service/trusted_launcher.hpp",
             Path(__file__).with_name("runtime-tests.cpp"), Path(__file__)]
    frozen_owned = {str(path): digest(path.read_bytes()) for path in owned}
    checks = []
    failures = 0

    def execute(argv: list[str], directory: Path, log_name: str) -> int:
        result = subprocess.run(argv, cwd=directory, env=environment, capture_output=True, timeout=120)
        log = directory / log_name
        content = result.stdout + result.stderr
        log.write_bytes(content)
        checks.append({"argv": argv, "cwd": str(directory), "returnCode": result.returncode,
                       "log": str(log), "logSha256": digest(content)})
        print(log_name + ": exit " + str(result.returncode), flush=True)
        if result.returncode or log_name == "runtime.log":
            print(content.decode("utf-8", errors="replace"), flush=True)
        return result.returncode

    for variant in ("no-generated-capability", "compiled-ready-false"):
        directory = output / variant
        directory.mkdir()
        generated = directory / "generated"
        generated.mkdir()
        build_info = "#pragma once\n#include <string_view>\nnamespace nickel::app::build {\n"
        for key, field in (("version", "version"), ("displayName", "display_name"),
                           ("applicationId", "application_id"), ("executableName", "executable_name"),
                           ("license", "license")):
            build_info += "inline constexpr std::string_view " + field + " = " + json.dumps(metadata[key]) + ";\n"
        (generated / "BuildInfo.hpp").write_text(build_info + "}\n", encoding="ascii")
        if variant == "compiled-ready-false":
            # This exercises the disabled compiled interface, with no stamp and no legal material.
            (generated / "TrustedLauncherBuild.hpp").write_text(
                '#pragma once\n#include "app/service/trusted_launcher.hpp"\n#include <array>\n'
                'namespace nickel::app::build {\ninline constexpr bool trusted_launcher_ready = false;\n'
                'inline constexpr std::string_view trusted_launcher_source_set_sha256 = "";\n'
                'inline constexpr std::string_view trusted_launcher_notice_set_sha256 = "";\n'
                'inline constexpr std::string_view trusted_launcher_repository = "";\n'
                'inline constexpr std::string_view trusted_launcher_stamp_resource = "";\n'
                'inline constexpr std::array<EmbeddedLauncherNotice, 0> trusted_launcher_notices{};\n}\n',
                encoding="ascii")
        objects = []
        variant_failed = False
        for source in (owned[0], owned[2]):
            obj = directory / (source.stem + ".obj")
            command = [str(compiler), "/nologo", "/TP", "/std:c++20", "/O2", "/MT", "/W4", "/WX",
                       "/utf-8", "/permissive-", "/EHsc", "/Gy", "/Gw", "/DNOMINMAX",
                       "/DWIN32_LEAN_AND_MEAN", "/D_WIN32_WINNT=0x0A00", "/I" + str(generated),
                       "/I" + str(inputs / "native"), "/I" + str(WORKTREE / "native"),
                       "/external:I" + str(prefix / "include"), "/external:W0", "/c", str(source),
                       "/Fo" + str(obj)]
            if execute(command, directory, source.stem + ".compile.log"):
                variant_failed = True
                break
            objects.append(str(obj))
        if not variant_failed:
            executable = directory / "trusted-launcher-negatives.exe"
            command = [str(linker), "/nologo", "/INCREMENTAL:NO", "/OPT:REF", "/OPT:ICF",
                       "/MACHINE:X64", "/SUBSYSTEM:CONSOLE", "/OUT:" + str(executable),
                       *objects, "kernel32.lib", "version.lib"]
            variant_failed = execute(command, directory, "link.log") != 0
            if not variant_failed:
                variant_failed = execute([str(executable)], directory, "runtime.log") != 0
        failures += int(variant_failed)
    if any(digest(Path(path).read_bytes()) != sha for path, sha in frozen_owned.items()):
        raise RuntimeError("Owned source changed during factory checks")
    receipt = {"schemaVersion": 1, "type": "trusted-launcher-factory-primitive-negatives",
               "status": "passed" if failures == 0 else "failed", "inputCommit": options.input_commit,
               "ownedSources": frozen_owned,
               "immutableInputs": [{"path": path, "bytes": len(data), "sha256": digest(data)}
                                   for path, data in sorted(sources.items())],
               "checks": checks, "actualProductionEntry": "NOT_RUN", "productionExport": "NOT_RUN",
               "tamperWithGenuineReceipt": "NOT_RUN", "publishedArchiveCancellation": "NOT_RUN",
               "syntheticProductionStamp": False, "historicalApplicationLibrary": False}
    path = output / "receipt.json"
    path.write_text(json.dumps(receipt, indent=2, ensure_ascii=True) + "\n", encoding="ascii")
    print("receipt: " + str(path), flush=True)
    print("sha256: " + digest(path.read_bytes()), flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
