"""CTest entry for real production-linked controls; preserve every fixture and log."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

OWNED = Path(__file__).resolve().parent
BUDGET = 256 * 1024 * 1024
LOG_LIMIT = 4 * 1024 * 1024


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_budget() -> None:
    used = sum(path.stat().st_size for path in OWNED.rglob("*") if path.is_file())
    if used > BUDGET:
        raise RuntimeError("Owned instance test storage exceeds 256 MiB")
    reserves = (("C:/", 1024 * 1024 * 1024), ("D:/", BUDGET)) if os.name == "nt" else ((str(OWNED), BUDGET),)
    for drive, reserve in reserves:
        if shutil.disk_usage(drive).free < reserve:
            raise RuntimeError("Insufficient instance test reserve on " + drive)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--kind", choices=("domain", "queue", "commit", "icon"), required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    options = parser.parse_args()
    output = None
    result = {"kind": "production-linked-instance-control", "fixture": options.kind,
              "productionAcceptance": False, "status": "BLOCKED_OR_FAILED"}
    try:
        executable = options.executable.resolve(strict=True)
        if not executable.is_file():
            raise RuntimeError("Control executable is not a regular file")
        workspace = options.workspace.resolve()
        if not options.workspace.is_absolute() or not workspace.is_relative_to(OWNED) or workspace == OWNED:
            raise RuntimeError("CTest workspace must remain below the owned test directory")
        check_budget()
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        output = workspace / (options.kind + "-" + stamp + "-" + uuid.uuid4().hex[:8])
        output.mkdir(parents=True, exist_ok=False)
        temporary = output / "tmp"
        temporary.mkdir()
        fixture = output / ("fixture-" + uuid.uuid4().hex)
        command = [str(executable)]
        if options.kind != "queue":
            command.append(str(fixture))
        inputs = {str(executable): digest(executable), str(Path(__file__).resolve()): digest(Path(__file__).resolve())}
        source_names = {"domain": "domain_tests.cpp", "queue": "queue_controls.cpp",
                        "commit": "commit_controls.cpp", "icon": "icon_actions.cpp"}
        source = OWNED / source_names[options.kind]
        inputs[str(source)] = digest(source)
        result.update({"command": command, "inputSha256": inputs,
                       "fixturePath": str(fixture) if options.kind != "queue" else None})
        environment = os.environ.copy()
        environment["TEMP"] = str(temporary)
        environment["TMP"] = str(temporary)
        environment["TMPDIR"] = str(temporary)
        log = output / "runtime.log"
        check_budget()
        with log.open("xb") as stream:
            completed = subprocess.run(command, cwd=output, env=environment,
                                       stdin=subprocess.DEVNULL, stdout=stream,
                                       stderr=subprocess.STDOUT, timeout=60)
        result["returncode"] = completed.returncode
        result["log"] = str(log)
        result["logSha256"] = digest(log)
        if log.stat().st_size > LOG_LIMIT:
            raise RuntimeError("Runtime log exceeds 4 MiB")
        text = log.read_text(encoding="utf-8", errors="replace")
        print(text, end="" if text.endswith("\n") else "\n")
        if completed.returncode:
            raise RuntimeError("Real instance control returned " + str(completed.returncode))
        for name, expected in inputs.items():
            if digest(Path(name)) != expected:
                raise RuntimeError("Instance control input changed while running: " + name)
        check_budget()
        result["status"] = "PASSED"
        return 0
    except Exception as failure:
        result["error"] = str(failure)
        print("FAIL instance CTest entry: " + str(failure), file=sys.stderr)
        return 1
    finally:
        if output is not None:
            log = output / "runtime.log"
            if log.is_file():
                result["log"] = str(log)
                result["logSha256"] = digest(log)
            with (output / "result.json").open("x", encoding="ascii", newline="\n") as stream:
                json.dump(result, stream, ensure_ascii=True, indent=2)
                stream.write("\n")
            print("EVIDENCE " + str(output / "result.json"))


if __name__ == "__main__":
    raise SystemExit(main())
