"""Run real task publication controls against one verified mirror and approved read-only JSON prefix."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import uuid

sys.dont_write_bytecode = True
import compile_controls as controls

SOURCES = (
    "native/core/config/atomic_file.cpp",
    "native/core/config/crypto.cpp",
    "native/core/config/detail.cpp",
    "native/core/config/path_guard.cpp",
    "native/core/network/control.cpp",
    "native/core/network/hash.cpp",
    "native/core/tasks/secure_fs.cpp",
    "native/core/tasks/tasks.cpp",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--readonly-prefix", type=Path, required=True)
    parser.add_argument("--source-overlay", type=Path, action="append", default=[])
    parser.add_argument("--vs-root", type=Path)
    parser.add_argument("--syntax-only", action="store_true")
    options = parser.parse_args()
    output = None
    manifest_sha = None
    inputs = {}
    records = []
    overlays = []
    overlay_inputs = {}
    try:
        controls.check_budget()
        manifest_sha = controls.verify_mirror()
        overlays, overlay_inputs = controls.verify_overlays(options.source_overlay)
        inputs.update(overlay_inputs)
        prefix = options.readonly_prefix
        if not prefix.is_absolute():
            raise RuntimeError("An explicitly approved absolute read-only prefix is required")
        sources = [next((root / name for root in overlays if (root / name).is_file()), controls.SHARED / name)
                   for name in SOURCES] + [controls.OWNED / "commit_controls.cpp"]
        for source in [Path(__file__).resolve(), controls.OWNED / "compile_controls.py", *sources]:
            inputs[str(source)] = digest(source)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        output = controls.OWNED / (".build-commit-" + stamp + "-" + uuid.uuid4().hex[:8])
        output.mkdir()
        temporary = output / "tmp"
        temporary.mkdir()
        os.environ["TEMP"] = str(temporary)
        os.environ["TMP"] = str(temporary)
        environment = controls.developer_environment(options.vs_root)
        environment["TEMP"] = str(temporary)
        environment["TMP"] = str(temporary)
        compiler = shutil.which("cl.exe", path=environment["PATH"])
        if compiler is None:
            raise RuntimeError("No MSVC compiler in the supplied developer environment")
        flags = ["/nologo", "/std:c++20", "/EHsc", "/MT", "/W4", "/utf-8", "/permissive-",
                 "/Zc:__cplusplus", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/DUNICODE", "/D_UNICODE",
                 "/DNICKEL_TRANSPORT_TEST_HOOKS", *["/I" + str(root / "native") for root in [*overlays, controls.SHARED]],
                 "/I" + str(prefix / "include")]
        (output / "inputs.json").write_text(json.dumps({"manifestSha256": manifest_sha,
            "inputSha256": inputs, "sourceOverlays": list(map(str, overlays)),
            "authorizedReadonlyPrefix": str(prefix)}, indent=2) + "\n", encoding="ascii")
        if options.syntax_only:
            controls.run([compiler, *flags, "/WX", "/Zs", str(sources[-1])], environment,
                         output, "commit-syntax", records)
        else:
            objects = []
            for index, source in enumerate(sources):
                obj = output / f"{index:02d}-{source.stem}.obj"
                objects.append(obj)
                controls.run([compiler, *flags, "/O1", "/Gy", "/c", "/Fo" + str(obj), str(source)],
                             environment, output, f"compile-{index:02d}-{source.stem}", records)
            executable = output / "commit_controls.exe"
            controls.run([compiler, "/nologo", "/MT", *map(str, objects), "/Fe" + str(executable),
                          "/link", "/INCREMENTAL:NO", "/OPT:REF", "bcrypt.lib", "advapi32.lib",
                          "shell32.lib", "ole32.lib", "user32.lib", "uuid.lib"],
                         environment, output, "commit-link", records)
            fixture = output / ("fixture-" + uuid.uuid4().hex)
            controls.run([str(executable), str(fixture)], environment, output, "commit-test", records, 30)
        after = controls.verify_mirror()
        if after != manifest_sha or controls.verify_overlays(overlays)[1] != overlay_inputs:
            raise RuntimeError("Dependency generation changed during controls")
        for name, expected in inputs.items():
            if digest(Path(name)) != expected:
                raise RuntimeError("Control input changed: " + name)
        evidence = {"kind": "real-task-publication-controls", "status": "PASSED",
                    "owner": "app-instance-actions", "syntaxOnly": options.syntax_only,
                    "manifestSha256": manifest_sha, "observedManifestSha256After": after,
                    "sourceOverlays": list(map(str, overlays)), "inputSha256": inputs, "commands": records,
                    "authorizedReadonlyPrefix": str(prefix), "prefixHeaderSha256": None, "productionAcceptance": False}
        (output / "result.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="ascii")
        print("PASS task publication controls only; evidence=" + str(output.relative_to(controls.ROOT) / "result.json"))
        return 0
    except Exception as failure:
        print("BLOCKED/FAIL " + str(failure), file=sys.stderr)
        if output is not None:
            evidence = {"kind": "real-task-publication-controls", "status": "BLOCKED_OR_FAILED",
                        "sourceOverlays": list(map(str, overlays)), "manifestSha256": manifest_sha,
                        "inputSha256": inputs, "commands": records, "error": str(failure), "productionAcceptance": False}
            (output / "result.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="ascii")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
