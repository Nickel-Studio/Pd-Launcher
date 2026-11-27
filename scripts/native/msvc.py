"""Import a trusted MSVC developer environment without invoking a shell for builds."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess

DEFAULT_VS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools")


def developer_environment(vs_root: Path = DEFAULT_VS) -> dict[str, str]:
    if os.name != "nt":
        return dict(os.environ)
    script = vs_root.resolve() / "Common7" / "Tools" / "VsDevCmd.bat"
    if not script.is_file():
        raise FileNotFoundError(script)
    # cmd is required only to load this installed, trusted batch environment.
    if any(character in str(script) for character in '\r\n"&|<>^%!'):
        raise ValueError("Unsafe developer environment path")
    command = f'call "{script}" -no_logo -arch=x64 -host_arch=x64 >nul && set'
    interpreter = Path(os.environ.get("COMSPEC", r"C:\Windows\System32\cmd.exe"))
    if not interpreter.is_file() or any(c in str(interpreter) for c in '\r\n"&|<>^%!'):
        raise ValueError("Unsafe command interpreter")
    # list2cmdline escapes embedded quotes for C runtimes, not cmd /s /c.
    result = subprocess.run(
        f'"{interpreter}" /d /s /c "{command}"',
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if result.returncode:
        raise RuntimeError("MSVC environment failed: " + result.stderr.strip())
    environment = {name.upper(): value for name, value in os.environ.items()}
    for line in result.stdout.splitlines():
        name, separator, value = line.partition("=")
        if separator and name:
            environment[name.upper()] = value
    cmake_root = vs_root / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake"
    extra = [cmake_root / "CMake" / "bin", cmake_root / "Ninja"]
    # An inherited initialized VS shell can have its PATH scrubbed by a CLI host.
    compiler_root = environment.get("VCTOOLSINSTALLDIR")
    if not compiler_root:
        raise RuntimeError("VS developer environment did not identify VCToolsInstallDir")
    compiler_bin = Path(compiler_root) / "bin" / "Hostx64" / "x64"
    if not (compiler_bin / "cl.exe").is_file():
        raise FileNotFoundError(compiler_bin / "cl.exe")
    extra.insert(0, compiler_bin)
    sdk_root = environment.get("WINDOWSSDKDIR")
    sdk_version = environment.get("WINDOWSSDKVERSION", "").rstrip("\\/")
    if sdk_root and sdk_version:
        sdk_bin = Path(sdk_root) / "bin" / sdk_version / "x64"
        if sdk_bin.is_dir():
            extra.append(sdk_bin)
    environment["PATH"] = os.pathsep.join(map(str, extra)) + os.pathsep + environment["PATH"]
    return environment


def input_inventory(path: Path) -> dict:
    import json
    from build_inputs import Records, absolute, file_hash

    path = path.resolve()
    if path.stat().st_size > 32 * 1024 * 1024:
        raise ValueError("Build-input inventory exceeds its size bound")
    digest, size = file_hash(path)
    document = json.loads(path.read_bytes())
    rows = document.get("records", document.get("files"))
    if not isinstance(rows, list) or not rows or len(rows) > 100000:
        raise ValueError("Build-input inventory must contain bounded actual files")
    base = Path(document.get("sourceRoot", path.parent)).resolve()
    records = Records()
    for row in rows:
        records.add(absolute(row["path"], base), row.get("roles", ["frozen-build-source"]), row)
    if file_hash(path) != (digest, size):
        raise ValueError("Build-input inventory changed during verification")
    return {"path": str(path), "sha256": digest, "bytes": size,
            "fileCount": len(records.rows()),
            "sourceCommit": document.get("sourceCommit"),
            "sourceTree": document.get("sourceTree"), "verified": True}


def run_logged(command: list[str], environment: dict[str, str], log: Path,
               evidence: Path, inventory: Path | None = None) -> int:
    import datetime as dt
    import hashlib
    import json

    log = log.resolve()
    evidence = evidence.resolve()
    if log == evidence or os.path.lexists(log) or os.path.lexists(evidence):
        raise ValueError("Command evidence paths must be distinct and unused")
    binding = input_inventory(inventory) if inventory is not None else None
    log.parent.mkdir(parents=True, exist_ok=True)
    evidence.parent.mkdir(parents=True, exist_ok=True)
    record = {"schemaVersion": 1, "argv": command, "cwd": str(Path.cwd()),
              "startedUtc": dt.datetime.now(dt.UTC).isoformat(), "complete": False,
              "log": str(log), "buildInputInventory": binding,
              "toolchainEnvironment": {name: environment[name] for name in
                  ("PATH", "LIB", "LIBPATH", "INCLUDE", "VCTOOLSINSTALLDIR", "VCTOOLSVERSION",
                   "WINDOWSSDKDIR", "WINDOWSSDKVERSION", "TEMP", "TMP") if name in environment}}
    with evidence.open("x", encoding="utf-8") as report:
        json.dump(record, report, indent=2)
        report.write("\n")
    with log.open("xb") as output:
        result = subprocess.run(command, env=environment, stdout=output, stderr=subprocess.STDOUT)
    with log.open("rb") as output:
        record["logSha256"] = hashlib.file_digest(output, "sha256").hexdigest()
    record.update(returnCode=result.returncode, complete=True,
                  finishedUtc=dt.datetime.now(dt.UTC).isoformat())
    if binding is not None:
        try:
            if input_inventory(inventory) != binding:
                raise ValueError("Build-input inventory changed during command execution")
            record["buildInputGate"] = "passed"
        except (OSError, ValueError, KeyError, TypeError) as error:
            record.update(buildInputGate="failed", buildInputError=str(error))
    evidence.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"returnCode": result.returncode, "log": str(log),
                      "evidence": str(evidence), "logSha256": record["logSha256"],
                      "buildInputGate": record.get("buildInputGate", "not-requested")}))
    # A native exit is preserved in the receipt even when source integrity fails.
    return result.returncode or (2 if record.get("buildInputGate") == "failed" else 0)


def main() -> int:
    import argparse
    import shutil

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vs-root", type=Path, default=DEFAULT_VS)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--input-inventory", type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    command = options.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("A program and its arguments are required")
    if (options.log is None) != (options.evidence is None):
        parser.error("--log and --evidence must be supplied together")
    if options.input_inventory is not None and options.log is None:
        parser.error("--input-inventory requires --log and --evidence")
    environment = developer_environment(options.vs_root)
    executable = shutil.which(command[0], path=environment["PATH"])
    if executable is None:
        parser.error("Program not found in developer environment: " + command[0])
    command[0] = executable
    if options.log is not None:
        return run_logged(command, environment, options.log, options.evidence, options.input_inventory)
    return subprocess.run(command, env=environment).returncode


if __name__ == "__main__":
    raise SystemExit(main())
