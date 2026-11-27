#!/usr/bin/env python3
"""Run real, bounded CMake/Ninja generation cases without Qt or a compiler."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone


OWNED = Path(__file__).resolve().parent
ROOT = OWNED.parent.parent
LIMIT = 32 * 1024 * 1024
OUTPUT_LIMIT = 2 * 1024 * 1024


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def digest(path):
    with path.open("rb") as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(65536), b""):
            value.update(block)
    return value.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")


def owned_size():
    return sum(path.stat().st_size for path in OWNED.rglob("*") if path.is_file())


def resolve_tool(name, supplied):
    found = supplied or shutil.which(name)
    if not found and os.name == "nt":
        base = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
        for version in ("18", "2022", "2019"):
            for edition in ("BuildTools", "Community", "Professional", "Enterprise"):
                cmake_root = base / "Microsoft Visual Studio" / version / edition
                cmake_root /= "Common7/IDE/CommonExtensions/Microsoft/CMake"
                relative = "CMake/bin/cmake.exe" if name == "cmake" else "Ninja/ninja.exe"
                candidate = cmake_root / relative
                if candidate.is_file():
                    found = candidate
                    break
            if found:
                break
    if not found:
        raise RuntimeError(f"Missing {name}; pass --{name} with its existing executable")
    executable = Path(found).resolve(strict=True)
    if not executable.is_file():
        raise RuntimeError(f"Tool is not a file: {executable}")
    return executable


class Regression:
    def __init__(self, args):
        self.args = args
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", args.run_id):
            raise RuntimeError("run-id must be a bounded simple name, not a path")
        self.scratch = OWNED / ".scratch" / args.run_id
        self.records = OWNED / "records" / args.run_id
        if self.scratch.exists() or self.records.exists():
            raise RuntimeError("Run directories already exist; choose a new run-id to preserve history")
        self.guard_budget()
        self.scratch.mkdir(parents=True)
        self.records.mkdir(parents=True)
        (self.scratch / "tmp").mkdir()
        self.env = os.environ.copy()
        self.env.update({
            "TEMP": str(self.scratch / "tmp"),
            "TMP": str(self.scratch / "tmp"),
            "TMPDIR": str(self.scratch / "tmp"),
            "CMAKE_BUILD_PARALLEL_LEVEL": "1",
            "CTEST_PARALLEL_LEVEL": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
        })
        self.summary = {
            "schemaVersion": 1,
            "startedUtc": utc_now(),
            "runnerArgv": [sys.executable, *sys.argv],
            "phase": args.phase,
            "worktree": str(ROOT),
            "scratch": str(self.scratch),
            "records": str(self.records),
            "budgetBytes": LIMIT,
            "workerCount": 1,
            "platform": sys.platform,
            "python": sys.version,
            "commands": [],
            "checks": [],
            "sourceHashes": {},
            "result": "INCOMPLETE",
        }
        self.cmake = resolve_tool("cmake", args.cmake)
        self.ninja = resolve_tool("ninja", args.ninja)
        self.module = (ROOT / args.module).resolve(strict=True)
        if not self.module.is_relative_to(ROOT):
            raise RuntimeError("The original module must be inside this worktree")
        self.sources = [
            self.module,
            ROOT / "CMakeLists.txt",
            ROOT / "native/app/CMakeLists.txt",
            ROOT / "native/tests/brand/CMakeLists.txt",
            OWNED / "run_regression.py",
            *sorted((OWNED / "fixture").rglob("CMakeLists.txt")),
            OWNED / "fixture/native/assets/brand/functional-resources.qrc",
        ]
        self.summary["sourceHashes"] = self.source_hashes()
        self.summary["module"] = str(self.module)
        if args.expected_module_sha256:
            self.check("expected original module SHA256", digest(self.module) == args.expected_module_sha256)
        self.save()

    def guard_budget(self):
        size = owned_size()
        if size >= LIMIT:
            raise RuntimeError(f"Owned files exceed the 32-MiB budget: {size}")
        if shutil.disk_usage(OWNED).free < LIMIT:
            raise RuntimeError("Less than 32 MiB free on the owned volume")
        return size

    def source_hashes(self):
        return {path.relative_to(ROOT).as_posix(): digest(path) for path in self.sources}

    def save(self):
        self.summary["ownedBytesObserved"] = owned_size()
        write_json(self.records / "result.json", self.summary)

    def check(self, name, condition, details=None):
        self.summary["checks"].append({"name": name, "passed": bool(condition), "details": details})
        self.save()
        if not condition:
            raise RuntimeError(f"Check failed: {name}: {details}")

    def execute(self, label, argv, cwd=None):
        self.guard_budget()
        out_path = self.records / f"{label}.stdout.txt"
        err_path = self.records / f"{label}.stderr.txt"
        arguments = [str(item) for item in argv]
        record = {
            "label": label,
            "argv": arguments,
            "cwd": str(cwd or ROOT),
            "startedUtc": utc_now(),
            "environmentOverrides": {key: self.env[key] for key in (
                "TEMP", "TMP", "TMPDIR", "CMAKE_BUILD_PARALLEL_LEVEL",
                "CTEST_PARALLEL_LEVEL", "PYTHONDONTWRITEBYTECODE",
            )},
            "timeoutSeconds": self.args.timeout,
        }
        started = time.monotonic()
        with out_path.open("wb") as stdout, err_path.open("wb") as stderr:
            process = subprocess.Popen(arguments, cwd=cwd or ROOT, env=self.env,
                                       stdout=stdout, stderr=stderr, shell=False)
            try:
                record["returnCode"] = process.wait(timeout=self.args.timeout)
                record["timedOut"] = False
            except subprocess.TimeoutExpired:
                process.kill()
                record["returnCode"] = process.wait()
                record["timedOut"] = True
        record["elapsedSeconds"] = round(time.monotonic() - started, 6)
        record["finishedUtc"] = utc_now()
        record["stdout"] = out_path.relative_to(ROOT).as_posix()
        record["stderr"] = err_path.relative_to(ROOT).as_posix()
        record["stdoutSha256"] = digest(out_path)
        record["stderrSha256"] = digest(err_path)
        write_json(self.records / f"{label}.command.json", record)
        self.summary["commands"].append(record)
        self.save()
        self.guard_budget()
        self.check(f"{label}: no timeout", not record["timedOut"])
        self.check(f"{label}: bounded output", max(out_path.stat().st_size, err_path.stat().st_size) <= OUTPUT_LIMIT)
        return record, out_path.read_text(encoding="utf-8", errors="replace") + err_path.read_text(encoding="utf-8", errors="replace")

    def fixture(self, case):
        source = self.scratch / case / "source"
        shutil.copytree(OWNED / "fixture", source)
        return source, self.scratch / case / "build"

    def configure(self, case, source, build, mode):
        trace = self.records / f"{case}.trace.jsonl"
        argv = [self.cmake, "-S", source, "-B", build, "-G", "Ninja",
                f"-DCMAKE_MAKE_PROGRAM:FILEPATH={self.ninja.as_posix()}",
                f"-DPD_REGRESSION_MODE:STRING={mode}",
                f"-DPD_DOMAIN_MODULE:FILEPATH={self.module.as_posix()}",
                "-DNICKEL_SHARED_NATIVE_ROOT:PATH=",
                "--trace-expand", "--trace-format=json-v1", f"--trace-redirect={trace}",
                f"--trace-source={self.module}",
                f"--trace-source={source / 'native/app/CMakeLists.txt'}",
                f"--trace-source={source / 'native/tests/brand/CMakeLists.txt'}"]
        return self.execute(f"{case}.configure", argv)

    def archive_generation(self, case, build):
        files = []
        for path in sorted(build.rglob("*")):
            if path.is_file() and (path.name in ("build.ninja", "CMakeCache.txt", "rules.ninja")
                                   or path.name.endswith("-configure-inputs.txt")):
                destination = self.records / "generated" / case / path.relative_to(build)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, destination)
                files.append({"path": path.relative_to(build).as_posix(), "sha256": digest(path)})
        self.summary.setdefault("generated", {})[case] = files
        self.save()

    def baseline(self):
        source, build = self.fixture("raw")
        canonical = source / "native/assets/brand/functional-resources.qrc"
        alternate = source / "native/tests/brand/../../assets/brand/functional-resources.qrc"
        self.check("raw spellings differ but reference the same file",
                   str(canonical) != str(alternate) and os.path.samefile(canonical, alternate),
                   {"canonical": str(canonical), "alternate": str(alternate)})
        record, output = self.configure("raw", source, build, "raw")
        self.check("raw CMake configure/generate exits 0", record["returnCode"] == 0, output)
        self.archive_generation("raw", build)
        record, output = self.execute("raw.ninja-parse", [self.ninja, "-C", build, "-j", "1", "-n"])
        duplicate_diagnostics = ("multiple rules generate", "duplicate", "is defined as an output multiple times")
        self.check("raw Ninja rejects duplicate functional resource output",
                   record["returnCode"] != 0 and "functional-resources.qrc" in output
                   and any(message in output for message in duplicate_diagnostics), output)
        self.summary["baseline"] = "EXPECTED_REAL_NINJA_FAILURE_REPRODUCED"
        self.save()

    def verify(self):
        source, build = self.fixture("builder")
        record, output = self.configure("builder", source, build, "builder")
        self.check("builder CMake configure/generate exits 0", record["returnCode"] == 0, output)
        self.archive_generation("builder-initial", build)
        record, output = self.execute("builder.ninja-parse", [self.ninja, "-C", build, "-j", "1", "-n"])
        self.check("builder Ninja parses without duplicate-output escape", record["returnCode"] == 0, output)
        record, output = self.execute("builder.build", [self.cmake, "--build", build, "--parallel", "1"])
        self.check("builder real single-worker fixture build exits 0", record["returnCode"] == 0, output)
        self.check_outputs(source, build, "initial")
        record, output = self.configure("builder-repeat", source, build, "builder")
        self.check("builder repeated configure exits 0", record["returnCode"] == 0, output)
        record, output = self.execute("builder.repeat-build", [self.ninja, "-C", build, "-j", "1"])
        self.check("builder repeated build exits 0", record["returnCode"] == 0, output)
        resource = source / "native/assets/brand/functional-resources.qrc"
        before = digest(resource)
        previous_mtime = max(path.stat().st_mtime_ns for path in build.rglob("*.ninja"))
        with resource.open("ab") as stream:
            stream.write(b"<!-- owned-fixture-change: configure dependency must remain active -->\n")
        future = max(time.time_ns(), previous_mtime + 2_000_000_000)
        os.utime(resource, ns=(future, future))
        self.summary["resourceMutation"] = {
            "path": str(resource), "beforeSha256": before, "afterSha256": digest(resource),
            "mtimeNs": future, "appliesOnlyToOwnedScratch": True,
        }
        record, output = self.execute("builder.resource-regenerate", [self.ninja, "-C", build, "-j", "1"])
        self.check("resource change triggers real successful CMake regeneration",
                   record["returnCode"] == 0 and "Re-running CMake" in output, output)
        self.check_outputs(source, build, "resource-change")
        record, output = self.execute("builder.final-parse", [self.ninja, "-C", build, "-j", "1", "-n"])
        self.check("regenerated Ninja still parses", record["returnCode"] == 0, output)
        self.archive_generation("builder-final", build)
        trace = self.records / "builder.trace.jsonl"
        events = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        consumed = any(event.get("file", "").replace("\\", "/").lower() == self.module.as_posix().lower()
                       and event.get("cmd") for event in events)
        self.check("expanded CMake trace proves original builder module execution", consumed)
        self.summary["verification"] = "REAL_ORIGINAL_MODULE_CMAKE_NINJA_PASS"
        self.save()

    def check_outputs(self, source, build, label):
        original = source / "native/assets/brand/functional-resources.qrc"
        outputs = [build / "native/app/functional-resources.qrc",
                   build / "native/app/app-resource-copy.qrc",
                   build / "tests/brand/functional-resources.qrc",
                   build / "tests/brand/brand-resource-copy.qrc"]
        hashes = {str(path.relative_to(build)): digest(path) if path.is_file() else None for path in outputs}
        self.check(f"{label}: app and brand outputs contain the actual current resource",
                   all(value == digest(original) for value in hashes.values()), hashes)

    def run(self):
        try:
            record, output = self.execute("tool.cmake-version", [self.cmake, "--version"])
            self.check("CMake version command exits 0", record["returnCode"] == 0, output)
            self.summary["cmakeVersion"] = output.strip()
            record, output = self.execute("tool.ninja-version", [self.ninja, "--version"])
            self.check("Ninja version command exits 0", record["returnCode"] == 0, output)
            self.summary["ninjaVersion"] = output.strip()
            self.summary["toolHashes"] = {str(self.cmake): digest(self.cmake), str(self.ninja): digest(self.ninja)}
            record, output = self.execute("source.git-head", ["git", "rev-parse", "HEAD"])
            self.check("source commit query exits 0", record["returnCode"] == 0, output)
            self.summary["sourceCommit"] = output.strip()
            if self.args.phase in ("baseline", "all"):
                self.baseline()
            if self.args.phase in ("verify", "all"):
                self.verify()
            self.check("original module and all production/fixture inputs remain unchanged",
                       self.source_hashes() == self.summary["sourceHashes"])
            self.summary["result"] = "BASELINE_REPRODUCED" if self.args.phase == "baseline" else "PASS"
            return 0
        except Exception as error:
            self.summary["result"] = "FAIL"
            self.summary["error"] = f"{type(error).__name__}: {error}"
            print(self.summary["error"], file=sys.stderr)
            return 1
        finally:
            self.summary["finishedUtc"] = utc_now()
            self.summary["sourceHashesAfter"] = self.source_hashes()
            self.save()
            print(json.dumps({"result": self.summary["result"], "records": str(self.records),
                              "ownedBytes": owned_size()}, ensure_ascii=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=("baseline", "verify", "all"), default="all")
    parser.add_argument("--run-id", default=datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    parser.add_argument("--module", default="cmake/NickelDomains.cmake")
    parser.add_argument("--expected-module-sha256")
    parser.add_argument("--cmake")
    parser.add_argument("--ninja")
    parser.add_argument("--timeout", type=int, choices=range(1, 121), default=60, metavar="1..120")
    args = parser.parse_args()
    try:
        return Regression(args).run()
    except Exception as error:
        print(f"Setup failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
