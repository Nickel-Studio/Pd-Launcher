"""Run serial CTest with immutable-source hashes and explicit executed-test evidence."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid
import xml.etree.ElementTree as ET

SOURCE_SUFFIXES = frozenset({".cpp", ".hpp", ".h", ".c", ".cmake", ".py", ".cjs", ".mjs", ".in", ".ps1", ".cmd", ".qrc", ".svg"})
GENERATED_NAMES = frozenset({".git", ".snow", ".deps", "__pycache__", "CMakeFiles", "Testing"})


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def reparse(path: Path) -> bool:
    info = path.lstat()
    return path.is_symlink() or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def source_manifest(root: Path) -> dict:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("Source snapshot must be an exact directory")
    files = {}
    has_sources = False
    for directory, names, filenames in os.walk(root, followlinks=False):
        names[:] = sorted(name for name in names if name not in GENERATED_NAMES and
                          not name.startswith((".build", ".foundation-build")))
        for name in names:
            if reparse(Path(directory) / name):
                raise ValueError("Source snapshot contains a linked directory")
        for name in sorted(filenames):
            path = Path(directory) / name
            if reparse(path) or not path.is_file():
                raise ValueError("Source snapshot contains a linked/non-regular input file")
            has_sources |= name == "CMakeLists.txt" or path.suffix.lower() in SOURCE_SUFFIXES
            # Fixtures, metadata and templates can change behavior without changing C++.
            files[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    if not has_sources:
        raise ValueError("Source snapshot has no compilable/test source files")
    ordered = dict(sorted(files.items()))
    encoded = json.dumps(ordered, sort_keys=True, separators=(",", ":")).encode("ascii")
    return {"root": str(root), "scope": "all-regular-snapshot-inputs", "files": ordered,
            "sha256": hashlib.sha256(encoded).hexdigest()}


def junit_evidence(path: Path, expected_names: list[str]) -> dict:
    document = ET.parse(path).getroot()
    cases = list(document.iter("testcase"))
    failures = []
    actual = [case.get("name", "") for case in cases]
    if sorted(actual) != sorted(expected_names):
        failures.append("JUnit tests do not match the exact CTest inventory")
    if not actual:
        failures.append("No tests were actually reported")
    scenarios = []
    for case in cases:
        unsuccessful = (any(case.find(tag) is not None for tag in ("failure", "error", "skipped")) or
                        case.get("status", "run") not in {"run", "passed"})
        if unsuccessful:
            failures.append("Test did not pass an executed scenario: " + case.get("name", ""))
        scenarios.append({"name": case.get("name", ""), "seconds": case.get("time"),
                          "status": "failed-or-unexecuted" if unsuccessful else "passed"})
    return {"scenarios": scenarios, "failures": failures}


def record_command(command: list[str], cwd: Path, destination: Path) -> tuple[dict, bytes]:
    started = utc_now()
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHON_COLORS="0", NO_COLOR="1")
    stdout_path, stderr_path = destination / "stdout.log", destination / "stderr.log"
    pending_path, command_path = destination / "pending.json", destination / "command.json"
    if any(path.exists() or path.is_symlink()
           for path in (stdout_path, stderr_path, pending_path, command_path)):
        raise FileExistsError("Command evidence must use a fresh destination")
    pending = {"command": command, "cwd": str(cwd), "startedUtc": started, "status": "running-or-interrupted"}
    with pending_path.open("x", encoding="utf-8") as pending_file:
        pending_file.write(json.dumps(pending, indent=2) + "\n")
    # Keep streamed bytes even if the outer CLI times out before this process finishes.
    with stdout_path.open("xb") as stdout_file, stderr_path.open("xb") as stderr_file:
        completed = subprocess.run(command, cwd=cwd, env=environment,
                                   stdout=stdout_file, stderr=stderr_file, shell=False)
    stdout, stderr = stdout_path.read_bytes(), stderr_path.read_bytes()
    record = {"command": command, "cwd": str(cwd), "startedUtc": started, "finishedUtc": utc_now(),
              "exitCode": completed.returncode,
              "stdoutSha256": hashlib.sha256(stdout).hexdigest(),
              "stderrSha256": hashlib.sha256(stderr).hexdigest()}
    with command_path.open("x", encoding="utf-8") as command_file:
        command_file.write(json.dumps(record, indent=2) + "\n")
    pending_path.unlink()
    return record, stdout


def inventory_names(raw: bytes) -> list[str]:
    inventory = json.loads(raw)
    tests = inventory.get("tests", [])
    names = [test.get("name", "") for test in tests]
    if not names or len(set(names)) != len(names) or not all(names):
        raise ValueError("CTest inventory must contain distinct, concrete tests")
    for test in tests:
        if not test.get("command"):
            raise ValueError("CTest inventory has an unavailable test executable")
        for item in test.get("properties", []):
            if item.get("name") == "DISABLED" and item.get("value"):
                raise ValueError("Disabled tests cannot constitute acceptance evidence")
    return names


def execution_manifest(workspace: Path, build: Path) -> dict:
    harness = source_manifest(workspace / "tests" / "native-acceptance")
    paths = {"python": Path(sys.executable), "wrapper": workspace / "scripts" / "native" / "msvc.py"}
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="strict")
    cache_names = {"CMAKE_COMMAND", "CMAKE_CTEST_COMMAND", "CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM"}
    for line in cache.splitlines():
        if ":" in line and "=" in line and line.split(":", 1)[0] in cache_names:
            name, value = line.split("=", 1)
            paths[name.split(":", 1)[0]] = Path(value)
    tools = {}
    for name, path in paths.items():
        path = path.resolve(strict=True)
        if not path.is_file() or reparse(path):
            raise ValueError("Execution tool must be a regular file: " + name)
        tools[name] = {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    artifacts = {}
    for path in sorted(build.rglob("*")):
        if path.is_file() and (path.suffix.lower() in {".exe", ".lib"} or
                               path.name in {"CMakeCache.txt", "build.ninja", "compile_commands.json", "CTestTestfile.cmake"}):
            if reparse(path):
                raise ValueError("Build evidence contains a linked artifact")
            artifacts[path.relative_to(build).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"harness": harness, "tools": tools, "buildFiles": artifacts, "pythonVersion": sys.version}


def preserve_round_output(build: Path, directory: Path, names: list[str]) -> dict:
    source = build / "Testing" / "Temporary" / "LastTest.log"
    if not source.is_file() or reparse(source):
        raise ValueError("Missing regular full CTest output")
    raw = source.read_bytes()
    target = directory / "ctest-full.log"
    with target.open("xb") as output:
        output.write(raw)
    recorded_names = []
    for line in raw.splitlines():
        index, separator, name = line.partition(b" Testing: ")
        if separator and index.replace(b"/", b"").isdigit():
            recorded_names.append(name.decode("utf-8"))
    if sorted(recorded_names) != sorted(names):
        raise ValueError("Full CTest output does not match the executed inventory")
    evidence = {"fullLog": {"path": str(target), "bytes": len(raw),
                            "sha256": hashlib.sha256(raw).hexdigest()}}
    if "native_foundation_golden" in names:
        marker = b" Testing: native_foundation_golden"
        start = raw.find(marker)
        end = raw.find(b"<end of output>", start)
        opening = raw.find(b"{", start, end)
        if start < 0 or end < 0 or opening < 0:
            raise ValueError("Full CTest output lacks complete golden JSON")
        payload = raw[opening:end].strip()
        golden = json.loads(payload)
        scenarios = golden.get("scenarios", [])
        passed, failed = golden.get("passed"), golden.get("failed")
        if (golden.get("kind") != "real-foundation-golden" or golden.get("schemaVersion") != 1 or
                type(passed) is not int or type(failed) is not int or failed != 0 or passed < 1 or
                not isinstance(scenarios, list) or passed != len(scenarios) or
                any(not isinstance(case, dict) or case.get("status") != "passed" for case in scenarios)):
            raise ValueError("Golden JSON does not prove its claimed executed scenarios")
        scenario_names = [case.get("name", "") for case in scenarios]
        if not all(isinstance(name, str) and name for name in scenario_names) or len(set(scenario_names)) != passed:
            raise ValueError("Golden scenarios must have concrete distinct names")
        golden_path = directory / "golden-result.json"
        with golden_path.open("xb") as output:
            output.write(payload)
        evidence["golden"] = {"path": str(golden_path), "bytes": len(payload),
                              "sha256": hashlib.sha256(payload).hexdigest(), "passed": passed, "failed": failed}
    return evidence



def run_suite(workspace: Path, source: Path, build: Path, output: Path,
              minimum_tests: int, repeat: int, test_filter: str | None = None) -> dict:
    workspace = workspace.resolve(strict=True)
    source = source.resolve(strict=True)
    build = build.resolve(strict=True)
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise ValueError("Build directory is not a configured CMake build")
    configured_sources = [line.split("=", 1)[1] for line in cache.read_text(encoding="utf-8").splitlines()
                          if line.startswith("NICKEL_SOURCE_ROOT:") and "=" in line]
    if len(configured_sources) != 1 or Path(configured_sources[0]).resolve(strict=True) != source:
        raise ValueError("Build cache is not bound to the exact requested source snapshot")
    before = source_manifest(source)
    execution_before = execution_manifest(workspace, build)
    output.mkdir(parents=True, exist_ok=False)
    (output / "source-before.json").write_text(json.dumps(before, indent=2) + "\n", encoding="utf-8")
    (output / "execution-before.json").write_text(json.dumps(execution_before, indent=2) + "\n", encoding="utf-8")
    wrapper = workspace / "scripts" / "native" / "msvc.py"
    if not wrapper.is_file():
        raise ValueError("The actual workspace MSVC command wrapper is required")
    base = [sys.executable, str(wrapper), "--", "ctest", "--test-dir", str(build)]
    if test_filter is not None:
        base += ["-R", test_filter]
    inventory_dir = output / "inventory"
    inventory_dir.mkdir()
    inventory_record, raw = record_command(base + ["--show-only=json-v1"], workspace, inventory_dir)
    failures = []
    rounds = []
    if inventory_record["exitCode"]:
        failures.append("CTest inventory command failed")
        names = []
    else:
        try:
            names = inventory_names(raw)
        except (ValueError, TypeError, KeyError) as error:
            names = []
            failures.append(str(error))
    if len(names) < minimum_tests:
        failures.append(f"Inventory has {len(names)} tests; required at least {minimum_tests}")
    if not failures:
        for index in range(repeat):
            directory = output / f"round-{index + 1:02d}"
            directory.mkdir()
            junit = directory / "ctest.xml"
            command = base + ["--verbose", "--output-on-failure", "--parallel", "1", "--output-junit", str(junit)]
            record, _ = record_command(command, workspace, directory)
            if record["exitCode"]:
                failures.append(f"CTest round {index + 1} exited {record['exitCode']}")
            try:
                observed = junit_evidence(junit, names)
                failures.extend(observed["failures"])
            except (OSError, ValueError, ET.ParseError) as error:
                observed = {"scenarios": [], "failures": [str(error)]}
                failures.append("Missing or invalid executed-test evidence")
            try:
                artifacts = preserve_round_output(build, directory, names)
            except (OSError, ValueError, TypeError, AttributeError) as error:
                artifacts = {"artifactFailures": [str(error)]}
                failures.append("Missing or invalid full-round output: " + str(error))
            rounds.append({"round": index + 1, "execution": record, **observed, **artifacts})
            if failures:
                break
    after = source_manifest(source)
    execution_after = execution_manifest(workspace, build)
    (output / "source-after.json").write_text(json.dumps(after, indent=2) + "\n", encoding="utf-8")
    (output / "execution-after.json").write_text(json.dumps(execution_after, indent=2) + "\n", encoding="utf-8")
    if before["files"] != after["files"]:
        failures.append("Source snapshot changed during acceptance; this run is not immutable-source evidence")
    if execution_before != execution_after:
        failures.append("Harness, tools or build artifacts changed during acceptance")
    if len(rounds) != repeat:
        failures.append("Not all requested independent serial rounds ran")
    result = {"schemaVersion": 1, "kind": "independent-production-ctest",
              "sourceSha256": before["sha256"], "sourceUnchanged": before["files"] == after["files"],
              "executionUnchanged": execution_before == execution_after,
              "build": str(build), "cacheSha256": hashlib.sha256(cache.read_bytes()).hexdigest(),
              "toolSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "inventory": inventory_record, "testNames": names, "requestedRounds": repeat,
              "rounds": rounds, "gate": "failed" if failures else "passed-executed-scenarios",
              "failures": failures,
              "limitations": ["Synthetic and loopback scenarios do not prove complete game launch or UI equivalence.",
                              "This evidence applies only to the exact recorded source snapshot and host.",
                              "Tool hashes do not fingerprint every installed SDK or dependency-prefix file."]}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--minimum-tests", type=int, required=True)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--filter")
    args = parser.parse_args()
    if args.minimum_tests < 1 or not 1 <= args.repeat <= 100:
        parser.error("A positive test minimum and 1..100 serial rounds are required")
    workspace = Path(__file__).resolve().parents[2]
    destination = workspace / ".snow" / "acceptance-evidence" / uuid.uuid4().hex
    result = run_suite(workspace, args.source, args.build, destination,
                       args.minimum_tests, args.repeat, args.filter)
    print(json.dumps({"evidence": str(destination), "gate": result["gate"],
                      "sourceSha256": result["sourceSha256"], "failures": result["failures"]}, indent=2))
    return 0 if not result["failures"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
