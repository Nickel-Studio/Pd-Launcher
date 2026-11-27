"""Verify the owned helper fixture itself, never the production launcher process layer."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evidence


def write_text_new(path: Path, text: str) -> None:
    path = evidence.checked_local(path, writable=True)
    evidence.capacity(path, len(text.encode("utf-8")), 256 * 1024 * 1024)
    with path.open("x", encoding="utf-8", newline="\n") as output:
        output.write(text)


def wait_marker(process: subprocess.Popen, path: Path, seconds: float = 5) -> None:
    deadline = time.monotonic() + seconds
    while not path.is_file():
        if process.poll() is not None:
            raise RuntimeError("helper exited before controlled barrier")
        if time.monotonic() >= deadline:
            raise RuntimeError("helper barrier timed out")
        time.sleep(0.005)


def run(executable: str, output: str) -> dict:
    exe = evidence.checked_local(executable)
    out = evidence.checked_local(output, writable=True)
    evidence.capacity(out, 1024 * 1024, 256 * 1024 * 1024)
    out.mkdir(parents=True, exist_ok=False)
    version = subprocess.run([str(exe), "-version"], capture_output=True, timeout=5, check=False)
    if version.returncode != 0 or b"17.0.14" not in version.stderr:
        raise RuntimeError("helper Java version fixture failed")
    rows = []
    for mode in ("exit-success", "exit-failure", "stdout-first", "hold-running"):
        control = out / mode
        evidence.capacity(control, 65536, 256 * 1024 * 1024)
        control.mkdir(exist_ok=False)
        write_text_new(control / "helper-control.owner", "production-smoke-helper-v1\n")
        write_text_new(control / "mode.txt", mode + "\n")
        environment = os.environ.copy()
        environment["NICKEL_SMOKE_CONTROL_DIRECTORY"] = str(control)
        command = [str(exe), "-cp", "synthetic classpath with spaces", "SyntheticMain", "--gameDir", str(control)]
        started = evidence.utc_now()
        with (control / "stdout.txt").open("xb") as stdout, (control / "stderr.txt").open("xb") as stderr:
            process = subprocess.Popen(command, cwd=control, env=environment, stdout=stdout, stderr=stderr)
            pid = process.pid
            running_before_release = False
            stdout_eof_before_exit = False
            try:
                if mode in ("stdout-first", "hold-running"):
                    wait_marker(process, control / "running.marker")
                    if mode == "stdout-first":
                        wait_marker(process, control / "stdout-closed.marker")
                        stdout_eof_before_exit = True
                    running_before_release = process.poll() is None
                    if not running_before_release or (control / "exit.marker").exists():
                        raise RuntimeError("helper did not remain running across control barrier")
                    write_text_new(control / "release.marker", "owned-test-release\n")
                code = process.wait(timeout=5)
            except BaseException:
                # This Popen owns the exact child handle; no by-name process termination.
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                raise
        expected = 0 if mode == "exit-success" else 23
        if code != expected or not (control / "exit.marker").is_file():
            raise RuntimeError("helper fixture returned an unexpected exit")
        argv = (control / "argv.txt").read_text(encoding="utf-8")
        if "synthetic classpath with spaces" not in argv or str(control) not in argv:
            raise RuntimeError("helper fixture argv lost path or spacing")
        if b"SMOKE_ACCESS_0123456789_PUBLIC_TEST" not in (control / "stdout.txt").read_bytes():
            raise RuntimeError("helper did not emit the split synthetic redaction input")
        if b"stderr-tail-after-stdout" not in (control / "stderr.txt").read_bytes():
            raise RuntimeError("helper stderr tail missing")
        rows.append({"mode": mode, "argv": command, "pid": pid, "pidBirthBoundBy": "owned-Popen-handle",
            "startedAt": started, "exitCode": code, "expectedExitCode": expected,
            "runningBeforeRelease": running_before_release, "stdoutEofBeforeExit": stdout_eof_before_exit,
            "stdoutSha256": evidence.sha256(control / "stdout.txt"),
            "stderrSha256": evidence.sha256(control / "stderr.txt"), "argvSha256": evidence.sha256(control / "argv.txt")})
    result = {"schema": 1, "kind": "owned-helper-selftest", "verdict": "PASS", "productionPass": False,
              "executable": str(exe), "executableSha256": evidence.sha256(exe), "modes": rows,
              "doesNotVerify": ["production-ProcessService", "AppServices", "GameSession-wait", "CancelLaunch"]}
    evidence.write_json_new(out / "helper-selftest.json", result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(run(args.executable, args.output), ensure_ascii=True, indent=2))
        return 0
    except (OSError, RuntimeError, evidence.EvidenceError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
