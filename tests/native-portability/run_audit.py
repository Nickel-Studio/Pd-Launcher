#!/usr/bin/env python3
"""Run actual owner code on a real host with durable, strictly scoped evidence."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import uuid

from audit_support import (OWNED, AuditSafetyError, BUILD_HEADROOM, WRITE_HEADROOM,
                           atomic_bytes, atomic_json, capacity, create_scratch,
                           owned_output, plain_path, sha256, single_worker)
import build_provenance
from verify_snapshot import verify

QT_COMPONENTS = ("Widgets", "Svg", "Test")


def digest(path: Path) -> str:
    return sha256(path)


def inventory(root: Path) -> dict:
    files = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and (path.suffix.lower() in {".cpp", ".hpp", ".h", ".cmake", ".json", ".qrc", ".rc", ".manifest"}
                               or path.name == "CMakeLists.txt"):
            plain_path(path, root=root, must_exist=True)
            files.append({"path": path.relative_to(root).as_posix(), "sha256": digest(path),
                          "bytes": path.stat().st_size})
    packed = json.dumps(files, sort_keys=True, separators=(",", ":")).encode("ascii")
    return {"root": str(root), "files": files, "contentSha256": hashlib.sha256(packed).hexdigest()}


def save(path: Path, payload: dict) -> None:
    atomic_json(path, payload)


def qt_gate_source() -> str:
    return ("cmake_minimum_required(VERSION 3.25)\n"
            "project(IndependentQtVersionGate LANGUAGES CXX)\n"
            "find_package(Qt6 6.8.3 EXACT CONFIG REQUIRED COMPONENTS "
            + " ".join(QT_COMPONENTS) + ")\n")


def build_capacity(required: int = 64 * 1024 * 1024) -> dict:
    result = {"owner": capacity(OWNED, required, BUILD_HEADROOM)}
    if os.name == "nt":
        result["system"] = capacity(Path("C:/"), 0, BUILD_HEADROOM)
    return result


def wsl_path(path: Path, distribution: str) -> str:
    result = subprocess.run(["wsl.exe", "-d", distribution, "--exec", "wslpath", "-u", str(path.absolute())],
                            capture_output=True, check=True, text=True, encoding="utf-8", timeout=30)
    value = result.stdout.strip()
    if not value.startswith("/") or "\n" in value:
        raise AuditSafetyError("wslpath did not return an exact absolute guest path")
    return value


def execute(argv: list[str], cwd: Path, timeout: int = 600, env: dict | None = None,
            headroom: int = WRITE_HEADROOM, *, ram_fixtures: bool = False) -> dict:
    guest = len(argv) >= 5 and Path(argv[0]).name.casefold() == "wsl.exe" and argv[1] == "-d" and argv[3] == "--exec"
    if ram_fixtures and not guest:
        raise AuditSafetyError("RAM fixtures require the actual WSL guest supervisor")
    raw = OWNED / "evidence" / ("command-" + uuid.uuid4().hex)
    capacity(raw, 1024 * 1024, headroom)
    raw.parent.mkdir(parents=True, exist_ok=True)
    stdout_path, stderr_path = raw.with_suffix(".stdout.log"), raw.with_suffix(".stderr.log")
    temporary = OWNED / "scratch" / raw.name
    owned_output(temporary)
    capacity(temporary, 1024 * 1024, headroom)
    temporary.mkdir(parents=True, exist_ok=False)
    local_env = dict(os.environ if env is None else env)
    local_env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary),
                     PYTHONDONTWRITEBYTECODE="1")
    pending = {"argv": argv, "cwd": str(cwd), "startedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
               "exitCode": None, "state": "STARTING", "temp": str(temporary),
               "stdoutLog": str(stdout_path.relative_to(OWNED)),
               "stderrLog": str(stderr_path.relative_to(OWNED)), "passed": False,
               "ramFixturesRequested": ram_fixtures, "completeProductVerified": False}
    save(raw.with_suffix(".json"), pending)
    process = None
    try:
        if guest:
            distribution = argv[2]
            token = uuid.uuid4().hex
            guest_status, control = raw.with_suffix(".guest.json"), raw.with_suffix(".control.json")
            wrapper = argv[:4] + ["env", "PYTHONDONTWRITEBYTECODE=1", "python3",
                       wsl_path(OWNED / "guest_command.py", distribution),
                       "--cwd", wsl_path(cwd, distribution), "--temp", wsl_path(temporary, distribution),
                       "--status", wsl_path(guest_status, distribution),
                       "--control", wsl_path(control, distribution),
                       "--stdout", wsl_path(stdout_path, distribution),
                       "--stderr", wsl_path(stderr_path, distribution),
                       "--token", token, "--seconds", str(timeout), "--headroom", str(headroom)]
            if ram_fixtures:
                wrapper.append("--ram-fixtures")
            wrapper.extend(["--", *argv[4:]])
            pending.update(wrapperArgv=wrapper, guestStatus=str(guest_status.relative_to(OWNED)))
            with raw.with_suffix(".supervisor.log").open("xb") as supervisor:
                process = subprocess.Popen(wrapper, cwd=cwd, stdout=supervisor, stderr=supervisor,
                                           stdin=subprocess.DEVNULL, env=local_env)
                pending.update(state="RUNNING", commandPid=process.pid)
                save(raw.with_suffix(".json"), pending)
                try:
                    process.wait(timeout=timeout + 60)
                except (subprocess.TimeoutExpired, KeyboardInterrupt):
                    save(control, {"token": token, "action": "cancel", "reason": "CANCELLED_BY_OUTER_SUPERVISOR"})
                    process.wait(timeout=30)
            if not guest_status.is_file():
                raise AuditSafetyError("Guest supervisor did not save a durable final status")
            status = json.loads(guest_status.read_text())
            if status.get("token") != token or status.get("state") in {"STARTING", "RUNNING"}:
                raise AuditSafetyError("Guest supervisor identity or final status is invalid")
            pending.update(state=status["state"], exitCode=status["exitCode"],
                           guestStatusSha256=digest(guest_status), guestSupervisor=status)
            if ram_fixtures and not status.get("fixtureCleanup", {}).get("removedExactRoot", False):
                raise AuditSafetyError("Per-run marked RAM fixture was not safely removed; command cannot pass")
        else:
            with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                process = subprocess.Popen(argv, cwd=cwd, stdout=stdout, stderr=stderr,
                                           stdin=subprocess.DEVNULL, env=local_env,
                                           start_new_session=os.name != "nt")
                pending.update(state="RUNNING", commandPid=process.pid)
                save(raw.with_suffix(".json"), pending)
                try:
                    pending.update(exitCode=process.wait(timeout=timeout), state="FINISHED")
                except subprocess.TimeoutExpired:
                    if os.name == "nt":
                        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                       stdout=stderr, stderr=stderr, env=local_env, check=False, timeout=15)
                    else:
                        os.killpg(process.pid, __import__("signal").SIGKILL)
                    process.wait(timeout=15)
                    pending.update(state="TIMED_OUT", exitCode=process.returncode)
    except (OSError, ValueError, AuditSafetyError, subprocess.SubprocessError) as error:
        pending.update(state="SUPERVISION_FAILED", error=str(error))
    pending.update(finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
    for name, path in (("stdout", stdout_path), ("stderr", stderr_path)):
        pending[name] = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
        pending[name + "Sha256"] = digest(path) if path.is_file() else None
    pending["passed"] = pending["state"] == "FINISHED" and pending["exitCode"] == 0
    save(raw.with_suffix(".json"), pending)
    return pending


def run(args) -> int:
    args._owns_output = False
    root = plain_path(args.native_root, must_exist=True)
    output = owned_output(args.output)
    if output.exists():
        raise AuditSafetyError("Refusing to replace an existing audit report")
    if not (root / "core/config/CMakeLists.txt").is_file():
        raise AuditSafetyError("native-root must identify actual owner Foundation source")
    if args.mode == "wsl" and os.name != "nt":
        raise AuditSafetyError("WSL mode requires a Windows host")
    if args.mode == "wsl" and not args.skip_runtime and not args.posix_ram_fixtures:
        raise AuditSafetyError("Use explicit POSIX RAM fixtures or --skip-runtime; DrvFS is not POSIX permission evidence")
    preflight = build_capacity(128 * 1024 * 1024 if args.all_core else 32 * 1024 * 1024)
    snapshot_before = verify(args.snapshot_manifest, args.expected_manifest_sha256) if args.snapshot_manifest else None
    if snapshot_before and not snapshot_before["verified"]:
        raise AuditSafetyError("Owner snapshot failed before-build verification")
    if root.parent.name == "native-shared" and snapshot_before is None:
        raise AuditSafetyError("The owner mirror requires its exact --snapshot-manifest")
    if snapshot_before and root != plain_path(args.snapshot_manifest).parent / "native":
        raise AuditSafetyError("The manifest must identify this exact native source root")
    before = inventory(root)
    inputs = build_provenance.audit_inputs()
    scratch = create_scratch(128 * 1024 * 1024 if args.all_core else 32 * 1024 * 1024)
    build = scratch / "build"
    build.mkdir()
    if args.mode == "wsl":
        prefix = ["wsl.exe", "-d", args.distribution, "--exec"]
        source_arg, native_arg, build_arg = (wsl_path(p, args.distribution) for p in (OWNED, root, build))
    else:
        prefix = []
        source_arg, native_arg, build_arg = str(OWNED), str(root), str(build)
    manifest = output.with_name(output.stem + ".source-manifest.json")
    save(manifest, before)
    commands, artifacts, runs = [], [], []
    payload = {"schema": 2, "state": "STARTING", "workerPid": os.getpid(),
               "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
               "runnerSha256": digest(Path(__file__)), "probeSourceSha256": digest(OWNED / "portability_probe.cpp"),
               "auditCmakeSha256": digest(OWNED / "CMakeLists.txt"), "auditInputsBefore": inputs,
               "mode": args.mode, "host": platform.platform(),
               "sourceRoot": str(root), "sourceBeforeSha256": before["contentSha256"],
               "sourceManifest": {"path": str(manifest.relative_to(OWNED)), "sha256": digest(manifest)},
               "snapshotBefore": snapshot_before, "preflight": preflight, "buildPhysicalPath": str(build),
               "commands": commands, "artifacts": artifacts, "runs": runs,
               "allCoreCompileRequested": args.all_core, "runtimeRequested": not args.skip_runtime,
               "qtVersionGate": "unrequested", "completeProductVerified": False, "passed": False,
               "scope": "Independent actual-code compilation and strictly named fixtures; NOT complete launcher/release/performance evidence"}

    def checkpoint(state: str) -> None:
        payload["state"] = state
        save(output, payload)
        args._owns_output = True

    def command(argv: list[str], timeout: int = 120, env: dict | None = None,
                build_step: bool = False, ram_fixtures: bool = False) -> dict:
        if build_step:
            build_capacity()
        result = execute(argv, OWNED, timeout, env, BUILD_HEADROOM if build_step else WRITE_HEADROOM,
                         ram_fixtures=ram_fixtures)
        commands.append(result)
        checkpoint(payload["state"])
        return result

    checkpoint("CONFIGURING")
    configured = command(prefix + [args.cmake, "-S", source_arg, "-B", build_arg, "-G", "Ninja",
                         "-DCMAKE_BUILD_TYPE=Release", "-DNICKEL_NATIVE_ROOT=" + native_arg,
                         "-DNICKEL_PORTABILITY_ALL_CORE=" + ("ON" if args.all_core else "OFF")]
                         + (["-DCMAKE_PREFIX_PATH=" + (wsl_path(args.prefix, args.distribution) if args.mode == "wsl" else str(args.prefix.absolute()))]
                            if args.prefix else []), build_step=True)
    built = None
    if configured["passed"]:
        if inventory(root) != before or build_provenance.audit_inputs() != inputs:
            raise AuditSafetyError("Source or audit inputs changed during configuration; refusing to build mixed objects")
        if snapshot_before and not verify(args.snapshot_manifest, snapshot_before["manifestSha256"])["verified"]:
            raise AuditSafetyError("Frozen snapshot changed during configuration")
        provenance = build_provenance.record(build, root, before["contentSha256"],
                                             snapshot_before["manifestSha256"] if snapshot_before else None,
                                             native_arg, source_arg)
        payload["buildProvenance"] = {"path": str(provenance.relative_to(OWNED)), "sha256": digest(provenance)}
        checkpoint("BUILDING")
        built = command(prefix + [args.cmake, "--build", build_arg, "--parallel", "1", "--verbose"],
                        args.build_timeout, build_step=True)
    suffix = ".exe" if args.mode == "local" and os.name == "nt" else ""
    binary = build / ("nickel_portability_probe" + suffix)
    core = build / ("nickel_portability_core_link" + suffix)
    for path in (binary, core):
        if path.is_file():
            artifacts.append({"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size,
                              "scope": "Actual-code-linked independent probe, not the product executable"})
    if built and built["passed"] and not args.skip_runtime and binary.is_file():
        checkpoint("RUNNING_FOUNDATION_FIXTURES")
        payload["fixtureScope"] = ("One marked /dev/shm root per run, <=1 MiB captured no-follow identities; guest runner finally removes only its root; temp/build/logs D"
                                   if args.mode == "wsl" else "Unique owner worktree fixtures on actual local host")
        for index in range(args.repeat):
            if args.mode == "wsl":
                result = command(prefix + [wsl_path(binary, args.distribution)], ram_fixtures=True)
            else:
                fixture = str(scratch / "fixtures" / f"run-{index + 1:02d}")
                env = {"PATH": os.environ.get("PATH", ""), "SystemRoot": os.environ.get("SystemRoot", ""),
                       "HOME": fixture + os.sep + "home", "XDG_CONFIG_HOME": fixture + os.sep + "xdg-config",
                       "XDG_DATA_HOME": fixture + os.sep + "xdg-data"}
                result = command([str(binary), "--fixture-root", fixture], env=env)
            run_path = output.with_name(output.stem + f".run-{index + 1:02d}.json")
            save(run_path, result)
            case_report = json.loads(result["stdout"]) if result["stdout"].strip().startswith("{") else {}
            runs.append({"path": str(run_path.relative_to(OWNED)), "sha256": digest(run_path),
                         "caseCount": case_report.get("caseCount"), "failedCases": case_report.get("failed"),
                         "exitCode": result["exitCode"], "passed": result["passed"],
                         "fixtureRemoved": result.get("guestSupervisor", {}).get("fixtureCleanup", {}).get("removedExactRoot")})
            checkpoint("RUNNING_FOUNDATION_FIXTURES")
    core_run = None
    if built and built["passed"] and args.all_core and core.is_file() and not args.skip_runtime:
        checkpoint("RUNNING_CORE_INITIALIZATION")
        core_run = command(prefix + [wsl_path(core, args.distribution)] if args.mode == "wsl" else [str(core)])
    if args.qt_gate:
        checkpoint("CHECKING_EXACT_QT")
        qt_source = scratch / "qt-gate"
        qt_source.mkdir()
        atomic_bytes(qt_source / "CMakeLists.txt", qt_gate_source().encode("ascii"))
        qt_result = command(prefix + [args.cmake, "-S", wsl_path(qt_source, args.distribution) if args.mode == "wsl" else str(qt_source),
                            "-B", wsl_path(scratch / "qt-gate-build", args.distribution) if args.mode == "wsl" else str(scratch / "qt-gate-build"),
                            "-G", "Ninja"], build_step=True)
        payload.update(qtComponents=list(QT_COMPONENTS), qtVersionGate="passed-exact-configuration" if qt_result["passed"] else "failed-exact-configuration")
    for name in ("compile_commands.json", "CMakeCache.txt", "build.ninja", ".ninja_log"):
        path = plain_path(build / name, root=build)
        if path.is_file():
            artifacts.append({"path": str(path.relative_to(OWNED)), "sha256": digest(path), "bytes": path.stat().st_size,
                              "preservation": "In-place owner build metadata; not copied or deleted"})
    after = inventory(root)
    inputs_after = build_provenance.audit_inputs()
    snapshot_after = verify(args.snapshot_manifest, snapshot_before["manifestSha256"]) if snapshot_before else None
    source_unchanged = before == after and (not snapshot_after or snapshot_after["verified"])
    payload.update(sourceAfterSha256=after["contentSha256"], sourceUnchanged=source_unchanged,
                   auditInputsAfter=inputs_after, auditInputsUnchanged=inputs == inputs_after,
                   snapshotAfter=snapshot_after, finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat(),
                   foundationCompileGate="passed" if built and built["passed"] else "failed",
                   foundationRuntimeGate="unverified" if args.skip_runtime else ("passed-named-fixtures" if len(runs) == args.repeat and all(r["passed"] for r in runs) else "failed-or-incomplete"),
                   stdCoreCompileGate=("passed-whole-archive-link" if built and built["passed"] and core.is_file() else "failed-or-incomplete") if args.all_core else "unrequested",
                   coreInitialization=core_run)
    payload["passed"] = bool(source_unchanged and inputs == inputs_after and built and built["passed"]
                             and (args.skip_runtime or (len(runs) == args.repeat and all(r["passed"] for r in runs)))
                             and (not args.all_core or (core.is_file() and (args.skip_runtime or bool(core_run and core_run["passed"]))))
                             and (not args.qt_gate or payload["qtVersionGate"] == "passed-exact-configuration"))
    checkpoint("FINISHED")
    print(json.dumps({"output": str(output), "sha256": digest(output), "passed": payload["passed"],
                      "sourceUnchanged": source_unchanged, "completeProductVerified": False,
                      "buildPhysicalPath": str(build), "runs": len(runs)}, indent=2))
    return 0 if payload["passed"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=["wsl", "local"], default="wsl" if os.name == "nt" else "local")
    parser.add_argument("--distribution", default="Ubuntu-24.04")
    parser.add_argument("--all-core", action="store_true")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--qt-gate", action="store_true")
    parser.add_argument("--snapshot-manifest", type=Path)
    parser.add_argument("--expected-manifest-sha256")
    parser.add_argument("--build-timeout", type=int, default=1800)
    parser.add_argument("--skip-runtime", action="store_true")
    parser.add_argument("--posix-ram-fixtures", action="store_true")
    parser.add_argument("--detach", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.repeat <= 20 or args.build_timeout < 1:
        parser.error("repeat must be 1..20 and build-timeout must be positive")
    try:
        if args.detach:
            output = owned_output(args.output)
            if output.exists() or output.with_suffix(".runner.log").exists():
                raise AuditSafetyError("Detached evidence paths already exist")
            build_capacity()
            capacity(output, 1024 * 1024)
            output.parent.mkdir(parents=True, exist_ok=True)
            argv = [sys.executable, str(Path(__file__).absolute()), *[a for a in sys.argv[1:] if a != "--detach"]]
            flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS if os.name == "nt" else 0
            with output.with_suffix(".runner.log").open("xb") as log:
                process = subprocess.Popen(argv, cwd=OWNED.parents[1], stdout=log, stderr=log,
                                           stdin=subprocess.DEVNULL, creationflags=flags,
                                           env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}, start_new_session=os.name != "nt")
            print(json.dumps({"workerPid": process.pid, "output": str(output)}))
            return 0
        with single_worker():
            return run(args)
    except (AuditSafetyError, OSError, ValueError, subprocess.SubprocessError) as error:
        if getattr(args, "_owns_output", False):
            try:
                output = owned_output(args.output)
                payload = json.loads(output.read_text())
                payload.update(state="FAILED_OR_BLOCKED", error=str(error), passed=False,
                               completeProductVerified=False,
                               finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
                save(output, payload)
            except (AuditSafetyError, OSError, ValueError):
                pass
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
