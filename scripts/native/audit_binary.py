#!/usr/bin/env python3
"""Audit actual native artifacts using the independent Windows acceptance parser."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

FORBIDDEN = re.compile(r"(?:webview|webengine|qt[56](?:quick|qml)|javascriptcore|(?:^|[/\\\s])(?:lib)?(?:node|v8|rust|tauri|msedge|chrome|chromium)(?:[._-]|$))", re.I)
DYNAMIC_QT_CRT = re.compile(r"^(?:qt[56].*|vcruntime.*|msvcp.*|concrt.*|vcomp.*|ucrtbased?)\.dll$", re.I)
MAX_IMAGE = 512 * 1024 * 1024


class AuditError(RuntimeError):
    pass


def load_windows_auditor():
    path = Path(__file__).resolve().parents[2] / "tests" / "native-acceptance" / "audit_windows.py"
    spec = importlib.util.spec_from_file_location("nickel_windows_acceptance_auditor", path)
    if spec is None or spec.loader is None:
        raise AuditError("Independent Windows acceptance auditor is unavailable: " + str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


WINDOWS_AUDITOR = load_windows_auditor()
API_SET = WINDOWS_AUDITOR.API_SET


def sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def pe_imports(data: bytes) -> list[dict[str, str]]:
    if len(data) > MAX_IMAGE:
        raise AuditError("PE image exceeds the audit size bound")
    try:
        image = WINDOWS_AUDITOR.PeImage(data)
        return ([{"name": name, "kind": "import"} for name in image.imports()]
                + [{"name": name, "kind": "delay"} for name in image.imports(delay=True)])
    except WINDOWS_AUDITOR.PeFormatError as error:
        raise AuditError(str(error)) from error


def windows_audit(binary: Path, module_evidence: Path | None = None,
                  windows_root: str | None = None) -> dict:
    binary = binary.resolve()
    if binary.stat().st_size > MAX_IMAGE:
        raise AuditError("Executable exceeds the audit size bound")
    try:
        artifact = WINDOWS_AUDITOR.audit(binary, module_evidence, windows_root)
    except (WINDOWS_AUDITOR.PeFormatError, ValueError) as error:
        raise AuditError(str(error)) from error
    system = Path(windows_root or os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    pending = [binary]
    seen = set()
    nodes = []
    errors = list(artifact["failures"])
    while pending:
        image = pending.pop()
        key = str(image).casefold()
        if key in seen:
            continue
        seen.add(key)
        if len(seen) > 512:
            raise AuditError("Import closure exceeds 512 images")
        if image.stat().st_size > MAX_IMAGE:
            raise AuditError("Import closure image exceeds the audit size bound")
        imports = pe_imports(image.read_bytes())
        is_system = image.parent.resolve() == system.resolve()
        nodes.append({"path": str(image), "sha256": sha256(image),
                      "imports": imports, "isSystem": is_system})
        # Windows owns its optional internal imports; the release closure ends at this boundary.
        if is_system:
            for item in imports:
                item["resolution"] = "Windows system implementation"
            continue
        for item in imports:
            name = item["name"]
            if FORBIDDEN.search(name):
                errors.append("Browser/QML/sidecar runtime import: " + name)
            if DYNAMIC_QT_CRT.fullmatch(name):
                errors.append("Dynamic Qt/CRT dependency: " + name)
            if API_SET.fullmatch(name):
                item["resolution"] = "Windows API set"
                continue
            adjacent = image.parent / name
            installed = system / name
            if adjacent.is_file():
                item["resolution"] = str(adjacent)
                errors.append("Non-system adjacent DLL: " + name)
                pending.append(adjacent)
            elif installed.is_file():
                item["resolution"] = str(installed)
                if not WINDOWS_AUDITOR.system_import(name):
                    errors.append("Unregistered system import: " + name)
                pending.append(installed)
            else:
                item["resolution"] = "UNRESOLVED"
                errors.append("Unresolved import: " + name)
    limitations = artifact["limitations"] + ["The import closure ends at Windows system libraries; their optional internal loads are OS-owned."]
    return {"nodes": nodes, "errors": sorted(set(errors)), "staticRuntime": True,
            "importClosureComplete": not errors, "artifactAudit": artifact,
            "runtimeGate": artifact["runtimeGate"], "limitations": limitations}


def native_tool_audit(binary: Path) -> dict:
    arguments = ["otool", "-L", str(binary)] if sys.platform == "darwin" else ["ldd", str(binary)]
    tool = shutil.which(arguments[0])
    if tool is None:
        raise AuditError("Platform import tool is missing: " + arguments[0])
    arguments[0] = tool
    result = subprocess.run(arguments, check=True, capture_output=True, text=True)
    errors = [line.strip() for line in result.stdout.splitlines()
              if FORBIDDEN.search(line) or "not found" in line]
    return {"command": arguments, "output": result.stdout, "errors": errors,
            "importClosureComplete": not errors, "staticSystemRuntime": False,
            "runtimeGate": "unverified",
            "limitations": ["Import-tool output is not clean-machine or observed-runtime evidence."]}


def audit(binary: Path, module_evidence: Path | None = None,
          windows_root: str | None = None) -> dict:
    if binary.is_symlink() or not binary.is_file():
        raise AuditError("Executable must be an exact regular file: " + str(binary))
    result = {"schema": 1, "executedUtc": dt.datetime.now(dt.UTC).isoformat(),
              "binary": str(binary.resolve()), "sha256": sha256(binary),
              "bytes": binary.stat().st_size, "platform": sys.platform}
    if os.name == "nt":
        result.update(windows_audit(binary, module_evidence, windows_root))
    else:
        if module_evidence is not None or windows_root is not None:
            raise AuditError("Windows module evidence cannot audit another platform")
        result.update(native_tool_audit(binary))
    result["noForbiddenImports"] = not any(FORBIDDEN.search(error) for error in result["errors"])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--modules", type=Path)
    parser.add_argument("--windows-root")
    parser.add_argument("--require-runtime", action="store_true")
    options = parser.parse_args()
    try:
        result = audit(options.binary, options.modules, options.windows_root)
    except (AuditError, OSError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        return 2
    if options.require_runtime and result["runtimeGate"] != "passed-observed-scenarios":
        result["errors"].append("Observed runtime evidence is required and has not passed")
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"sha256": result["sha256"], "nodes": len(result.get("nodes", [])),
                      "runtimeGate": result["runtimeGate"], "errors": result["errors"]}, indent=2))
    return 0 if not result["errors"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
