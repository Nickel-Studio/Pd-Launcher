"""Compile owned production objects with CMake's actual commands, without rebuilding dependencies."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

WORKTREE = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(WORKTREE / "scripts" / "native"))
from msvc import developer_environment


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("names", nargs="*")
    options = parser.parse_args()
    build = WORKTREE / "native" / "tests" / "app" / "build-composed"
    temporary = WORKTREE / "native" / "tests" / "app" / ".temp"
    temporary.mkdir(exist_ok=True)
    system_drive = Path(os.environ.get("SystemDrive", "C:")) / "\\"
    environment = developer_environment()
    environment["TEMP"] = environment["TMP"] = str(temporary)
    environment["VSLANG"] = "1033"
    entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    selected = [entry for entry in entries if ("nickel_application_composed.dir" in entry["command"]
        or "nickel_application_production_tests.dir" in entry["command"])
        and (not options.names or Path(entry["file"]).name in options.names)]
    if not selected:
        raise RuntimeError("No production compilation commands matched")
    missing = set(options.names) - {Path(entry["file"]).name for entry in selected}
    if missing:
        raise RuntimeError(f"Requested production sources are missing: {sorted(missing)}")
    failures = 0
    for entry in selected:
        if shutil.disk_usage(system_drive).free < 1024 * 1024 * 1024 or shutil.disk_usage(build).free < 256 * 1024 * 1024:
            raise RuntimeError("Compilation requires 1 GiB on the system drive and 256 MiB in the existing build tree")
        source = Path(entry["file"])
        command = entry["command"].replace(" /showIncludes", "")
        directory = Path(entry["directory"])
        directory.mkdir(exist_ok=True)
        output = entry.get("output")
        if output:
            (directory / output).parent.mkdir(parents=True, exist_ok=True)
        log = build / (source.stem + ".object.log")
        with log.open("wb") as stream:
            result = subprocess.run(command, cwd=directory, env=environment,
                                    stdout=stream, stderr=subprocess.STDOUT, timeout=120)
        content = log.read_bytes().decode("utf-8", errors="replace")
        print(f"{source.name}: exit {result.returncode}", flush=True)
        if result.returncode:
            failures += 1
            for line in content.splitlines():
                if "error" in line.lower() or "fatal" in line.lower():
                    print(line, flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
