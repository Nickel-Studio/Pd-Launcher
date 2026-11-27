#!/usr/bin/env python3
"""Read-only PE/module audit using the frozen acceptance parser, never a release claim."""
from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path, PureWindowsPath
import sys
from types import ModuleType

from audit_support import AuditSafetyError, atomic_json, owned_output, plain_path, sha256
from verify_snapshot import HASH

MAX_IMAGE = 512 * 1024 * 1024
MAX_MODULE_EVIDENCE = 8 * 1024 * 1024


def exact_file(path: Path, expected: str, limit: int) -> dict:
    path = plain_path(path, must_exist=True)
    if not HASH.fullmatch(expected) or not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise AuditSafetyError("An exact hash and bounded regular audit input are required: " + str(path))
    actual = sha256(path)
    if actual != expected:
        raise AuditSafetyError("Audit input SHA-256 differs from its supplied identity: " + str(path))
    return {"path": str(path), "sha256": actual, "bytes": path.stat().st_size}


def load_parser(path: Path, expected: str):
    reference = exact_file(path, expected, 1024 * 1024)
    source = Path(reference["path"]).read_bytes()
    if __import__("hashlib").sha256(source).hexdigest() != expected:
        raise AuditSafetyError("Acceptance parser changed before loading")
    module = ModuleType("nickel_frozen_windows_acceptance")
    module.__file__ = reference["path"]
    exec(compile(source, reference["path"], "exec"), module.__dict__)
    if not all(callable(getattr(module, name, None)) for name in ("PeImage", "check_imports", "audit_modules", "audit")):
        raise AuditSafetyError("Supplied acceptance parser does not expose the inspected audit contract")
    return module, reference


def validate_module_shape(path: Path) -> None:
    payload = json.loads(path.read_bytes().decode("utf-8-sig"))
    if not isinstance(payload, dict):
        raise AuditSafetyError("Observed-module evidence must be a JSON object")
    snapshots = payload.get("snapshots", [])
    if not isinstance(snapshots, list) or len(snapshots) > 128:
        raise AuditSafetyError("Observed-module evidence must contain a bounded snapshot list")
    for item in snapshots:
        if not isinstance(item, dict):
            raise AuditSafetyError("Observed module snapshot must be a JSON object")
        scenario, pid = item.get("scenario"), item.get("pid")
        if not isinstance(scenario, str) or not 1 <= len(scenario) <= 256 or type(pid) is not int or pid <= 0:
            raise AuditSafetyError("Observed module snapshot needs a concrete scenario and positive PID")
        modules, children = item.get("modules", []), item.get("children", [])
        if not isinstance(modules, list) or len(modules) > 2048 or not isinstance(children, list) or len(children) > 256:
            raise AuditSafetyError("Observed module and child lists exceed their bounded contract")
        if any(not isinstance(raw, str) or not 1 <= len(raw) <= 32767 for raw in modules):
            raise AuditSafetyError("Observed module paths must be bounded strings")
        if any(not isinstance(child, dict) or not isinstance(child.get("executable", ""), str)
               or not isinstance(child.get("purpose"), str) for child in children):
            raise AuditSafetyError("Observed children must contain typed executable/purpose fields")


def audit(executable: Path, expected_executable: str, parser: Path, expected_parser: str,
          modules: Path | None = None, expected_modules: str | None = None,
          windows_root: str | None = None) -> dict:
    if (modules is None) != (expected_modules is None):
        raise AuditSafetyError("Observed-module path and exact SHA-256 must be supplied together")
    if modules is not None:
        root = PureWindowsPath(windows_root or "")
        if not root.is_absolute() or ".." in root.parts:
            raise AuditSafetyError("Observed-module audit needs the actual absolute Windows system root")
    binary_ref = exact_file(executable, expected_executable, MAX_IMAGE)
    auditor, parser_ref = load_parser(parser, expected_parser)
    module_ref = exact_file(modules, expected_modules, MAX_MODULE_EVIDENCE) if modules is not None else None
    if module_ref:
        validate_module_shape(Path(module_ref["path"]))
    result = auditor.audit(Path(binary_ref["path"]), Path(module_ref["path"]) if module_ref else None, windows_root)
    for reference, limit in ((binary_ref, MAX_IMAGE), (parser_ref, 1024 * 1024),
                             (module_ref, MAX_MODULE_EVIDENCE)):
        if reference:
            exact_file(Path(reference["path"]), reference["sha256"], limit)
    result.update(auditOwner="portability-audit", executedUtc=dt.datetime.now(dt.timezone.utc).isoformat(),
                  auditInputs={"executable": binary_ref, "parser": parser_ref, "modules": module_ref},
                  staticCrtGate="unverified", staticPluginResourceGate="unverified",
                  cleanDirectoryGate="unverified", noExtractionGate="unverified",
                  systemModuleIdentityGate="unverified", finalRelinkLegalGate="unverified",
                  deploymentGate="failed" if result["failures"] else "incomplete",
                  completeProductVerified=False, passed=False,
                  scope="Existing Windows acceptance schema; hash-bound read-only imports/modules only. "
                        "No executable launch, DLL copy, extraction monitoring or complete-product acceptance.")
    result["limitations"].append("Observed module JSON is supplied evidence; capture provenance and OS module identity need separate verification.")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--expected-executable-sha256", required=True)
    parser.add_argument("--parser", type=Path, required=True)
    parser.add_argument("--expected-parser-sha256", required=True)
    parser.add_argument("--modules", type=Path)
    parser.add_argument("--expected-modules-sha256")
    parser.add_argument("--windows-root")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        output = owned_output(args.output)
        if output.exists():
            raise AuditSafetyError("Refusing to replace existing Windows audit evidence")
        result = audit(args.executable, args.expected_executable_sha256, args.parser,
                       args.expected_parser_sha256, args.modules, args.expected_modules_sha256,
                       args.windows_root)
        atomic_json(output, result)
        print(json.dumps({"sha256": result["sha256"], "importGate": result["importGate"],
                          "runtimeGate": result["runtimeGate"], "deploymentGate": result["deploymentGate"],
                          "completeProductVerified": False, "failures": result["failures"]}, indent=2))
        return 1
    except (OSError, ValueError, TypeError, AuditSafetyError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
