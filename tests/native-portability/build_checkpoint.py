#!/usr/bin/env python3
"""Resume a D-owned build with the same verified source snapshot, never historical C objects."""
from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import subprocess
import sys

from audit_support import OWNED, AuditSafetyError, owned_output, plain_path, single_worker
import build_provenance
from run_audit import build_capacity, digest, execute, inventory, save, wsl_path
from verify_snapshot import verify


def validate_build(build: Path, root: Path, distribution: str,
                   source_sha256: str, manifest_sha256: str) -> dict:
    return build_provenance.validate(build, root, source_sha256, manifest_sha256,
                                     wsl_path(root, distribution), wsl_path(OWNED, distribution))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--native-root", type=Path, required=True)
    parser.add_argument("--snapshot-manifest", type=Path, required=True)
    parser.add_argument("--expected-manifest-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--distribution", default="Ubuntu-24.04")
    parser.add_argument("--seconds", type=int, default=1800)
    args = parser.parse_args()
    payload = None
    try:
        if args.seconds < 1:
            raise AuditSafetyError("Build timeout must be positive")
        output = owned_output(args.output)
        if output.exists():
            raise AuditSafetyError("Refusing to overwrite an existing build checkpoint")
        build_capacity()
        root = plain_path(args.native_root, must_exist=True)
        build = owned_output(args.build_dir)
        with single_worker():
            before = verify(args.snapshot_manifest, args.expected_manifest_sha256)
            if not before["verified"] or root != plain_path(args.snapshot_manifest).parent / "native":
                raise AuditSafetyError("Snapshot does not identify the exact verified native root")
            source = inventory(root)
            provenance = validate_build(build, root, args.distribution,
                                        source["contentSha256"], before["manifestSha256"])
            inputs = build_provenance.audit_inputs()
            build_capacity()
            payload = {"schema": 2, "state": "RUNNING", "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
                       "snapshotBefore": before, "sourceBeforeSha256": source["contentSha256"],
                       "buildPhysicalPath": str(build), "buildProvenance": provenance,
                       "auditInputsBefore": inputs, "runnerSha256": digest(Path(__file__)),
                       "scope": "Actual-code build resume only; NOT complete launcher or runtime acceptance",
                       "passed": False, "completeProductVerified": False}
            save(output, payload)
            result = execute(["wsl.exe", "-d", args.distribution, "--exec", "cmake", "--build",
                              wsl_path(build, args.distribution), "--parallel", "1", "--verbose"],
                             OWNED, args.seconds, headroom=1024 * 1024 * 1024)
            payload.update(command=result, state=result["state"], exitCode=result["exitCode"],
                           sourceAfterSha256=inventory(root)["contentSha256"],
                           snapshotAfter=verify(args.snapshot_manifest, args.expected_manifest_sha256),
                           auditInputsAfter=build_provenance.audit_inputs())
            payload["sourceUnchanged"] = (payload["sourceBeforeSha256"] == payload["sourceAfterSha256"]
                                          and payload["snapshotAfter"]["verified"])
            payload["auditInputsUnchanged"] = inputs == payload["auditInputsAfter"]
            payload["passed"] = result["passed"] and payload["sourceUnchanged"] and payload["auditInputsUnchanged"]
            payload["artifacts"] = []
            for name in ("compile_commands.json", "CMakeCache.txt", "build.ninja", ".ninja_log"):
                path = plain_path(build / name, root=build)
                if path.is_file():
                    payload["artifacts"].append({"path": str(path.relative_to(OWNED)),
                                                 "bytes": path.stat().st_size, "sha256": digest(path),
                                                 "preservation": "In-place owner build metadata; not copied or deleted"})
            payload["finishedUtc"] = dt.datetime.now(dt.timezone.utc).isoformat()
            save(output, payload)
        print(json.dumps({"state": payload["state"], "exitCode": payload["exitCode"],
                          "sourceUnchanged": payload["sourceUnchanged"], "passed": payload["passed"]}, indent=2))
        return 0 if payload["passed"] else 1
    except (OSError, ValueError, AuditSafetyError, subprocess.SubprocessError) as error:
        if payload is not None:
            payload.update(state="FAILED_OR_BLOCKED", error=str(error), passed=False,
                           completeProductVerified=False, finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
            try:
                save(output, payload)
            except (OSError, ValueError, AuditSafetyError):
                pass
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
