"""Compile owned C++ sources against supplied headers, with one worker and bounded output."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
OWNED = ROOT / "native/tests/app_instance_actions"
SHARED = ROOT / ".snow/native-shared"
BUDGET = 256 * 1024 * 1024
DOMAIN_SOURCES = (
    "native/core/config/atomic_file.cpp",
    "native/core/config/crypto.cpp",
    "native/core/config/detail.cpp",
    "native/core/config/path_guard.cpp",
    "native/core/config/repository.cpp",
    "native/core/instances/domain_io.cpp",
    "native/core/instances/instances.cpp",
    "native/core/instances/settings.cpp",
    "native/core/instances/startup.cpp",
    "native/app/service/mutation_gate.cpp",
    "native/app/WorkerQueue.cpp",
)


def owned_bytes() -> int:
    return sum(path.stat().st_size for path in OWNED.rglob("*") if path.is_file())


def verify_mirror() -> str:
    manifest = SHARED / "snapshot.json"
    raw = manifest.read_bytes()
    parsed = json.loads(raw.decode("utf-8-sig"))
    if parsed.get("member") != "app-instance-actions":
        raise RuntimeError("Wrong dependency snapshot member")
    for name, record in parsed["files"].items():
        path = SHARED / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]:
            raise RuntimeError("Supplied manifest hash mismatch: " + name)
    return hashlib.sha256(raw).hexdigest()


def verify_overlays(paths: list[Path]) -> tuple[list[Path], dict[str, str]]:
    roots = []
    inputs: dict[str, str] = {}
    incoming = (ROOT / ".snow/app-inputs/incoming").resolve()
    for selected in paths:
        root = selected.resolve()
        if not root.is_relative_to(incoming) or root in roots:
            raise RuntimeError("Overlay must be a distinct published incoming directory")
        marker_path = root / "publication-complete.json"
        manifest_path = root / "snapshot.json"
        if (root / ".publisher.lock").exists():
            raise RuntimeError("Overlay publisher is still active: " + str(root))
        marker_raw = marker_path.read_bytes()
        raw = manifest_path.read_bytes()
        marker = json.loads(marker_raw.decode("utf-8-sig"))
        manifest = json.loads(raw.decode("utf-8-sig"))
        if not marker.get("publicationComplete") or marker.get("snapshotFileSha256") != hashlib.sha256(raw).hexdigest():
            raise RuntimeError("Overlay has no matching completed publication: " + str(root))
        inputs[str(marker_path)] = hashlib.sha256(marker_raw).hexdigest()
        inputs[str(manifest_path)] = hashlib.sha256(raw).hexdigest()
        for name, expected in manifest["files"].items():
            path = (root / name).resolve()
            if not path.is_relative_to(root):
                raise RuntimeError("Overlay path escapes its publication: " + name)
            data = path.read_bytes()
            actual = hashlib.sha256(data).hexdigest()
            if actual != expected["sha256"] or len(data) != expected["bytes"]:
                raise RuntimeError("Overlay input does not match its manifest: " + name)
            inputs[str(path)] = actual
        if (root / ".publisher.lock").exists() or marker_path.read_bytes() != marker_raw or manifest_path.read_bytes() != raw:
            raise RuntimeError("Overlay changed while checking publication: " + str(root))
        roots.append(root)
    return roots, inputs


def check_budget() -> None:
    if owned_bytes() > BUDGET:
        raise RuntimeError("Owned incremental storage exceeds 256 MiB")
    for drive, reserve in (("C:/", 1024 * 1024 * 1024), ("D:/", BUDGET)):
        free = shutil.disk_usage(drive).free
        if free < reserve:
            raise RuntimeError(f"Insufficient compile reserve on {drive}: {free} bytes; require {reserve}")


def developer_environment(vs_root: Path | None) -> dict[str, str]:
    path = SHARED / "scripts/native/msvc.py"
    spec = importlib.util.spec_from_file_location("supplied_msvc", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("MSVC environment helper is not supplied")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.developer_environment(vs_root) if vs_root else module.developer_environment()


def run(command: list[str], environment: dict[str, str], output: Path, label: str,
        records: list[dict[str, object]], timeout: int = 180) -> None:
    check_budget()
    log = output / (label + ".log")
    with log.open("wb") as stream:
        result = subprocess.run(command, cwd=output, env=environment, stdin=subprocess.DEVNULL,
                                stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
    if log.stat().st_size > 4 * 1024 * 1024:
        raise RuntimeError("Compiler log exceeds its quota: " + str(log))
    text = log.read_text(encoding="utf-8", errors="replace")
    record = {"label": label, "command": command, "returncode": result.returncode,
              "log": str(log.relative_to(ROOT)), "logSha256": hashlib.sha256(log.read_bytes()).hexdigest()}
    records.append(record)
    (output / "commands.json").write_text(json.dumps(records, indent=2) + "\n", encoding="ascii")
    print(f"{label}: exit={result.returncode}; log={log.relative_to(ROOT)}")
    if result.returncode:
        print(text[-12000:])
        raise RuntimeError("Command failed: " + label)
    if label == "domain-test":
        print(text.strip())
    check_budget()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("probe", "syntax", "prefix-syntax", "domain-syntax", "domain"), required=True)
    parser.add_argument("--json-include", type=Path)
    parser.add_argument("--qt-include", type=Path)
    parser.add_argument("--readonly-prefix", type=Path)
    parser.add_argument("--source-overlay", type=Path, action="append", default=[])
    parser.add_argument("--vs-root", type=Path)
    options = parser.parse_args()
    records: list[dict[str, object]] = []
    source_hashes: dict[str, str] = {}
    inputs: dict[str, str] = {}
    overlays: list[Path] = []
    overlay_inputs: dict[str, str] = {}
    manifest_sha: str | None = None
    observed_manifest_after: str | None = None
    prefix: Path | None = None
    output: Path | None = None
    try:
        check_budget()
        manifest_sha = verify_mirror()
        overlays, overlay_inputs = verify_overlays(options.source_overlay)
        inputs.update(overlay_inputs)
        inputs[str(Path(__file__).resolve())] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        include = ["/I" + str(root / "native") for root in [*overlays, SHARED]]
        qt_flags: list[str] = []
        if options.mode == "prefix-syntax":
            prefix = options.readonly_prefix
            if prefix is None or not prefix.is_absolute():
                raise RuntimeError("An explicitly authorized absolute read-only prefix is required")
            # Only the approved compiler reads the external prefix; no copy or file probing here.
            qt = prefix / "include"
            include.append("/I" + str(qt))
            qt_flags = ["/DQT_STATIC", "/I" + str(prefix / "mkspecs/win32-msvc")]
            qt_flags.extend("/I" + str(qt / module) for module in
                            ("QtCore", "QtGui", "QtWidgets", "QtNetwork", "QtConcurrent", "QtSvg"))
        elif options.mode != "probe":
            if options.json_include is None or not (options.json_include / "nlohmann/json.hpp").is_file():
                raise RuntimeError("An exact verified nlohmann include directory is required")
            json_header = (options.json_include / "nlohmann/json.hpp").resolve()
            inputs[str(json_header)] = hashlib.sha256(json_header.read_bytes()).hexdigest()
            include.append("/I" + str(options.json_include.resolve()))
        if options.mode == "syntax":
            if options.qt_include is None:
                raise RuntimeError("Production syntax checking requires an exact Qt 6.8.3 include directory")
            qt = options.qt_include.resolve()
            for module in ("QtCore", "QtGui"):
                if not (qt / module).is_dir():
                    raise RuntimeError("Qt include module is absent: " + module)
            qt_flags = ["/DQT_STATIC", "/I" + str(qt)]
            qt_flags.extend("/I" + str(qt / module) for module in
                            ("QtCore", "QtGui", "QtWidgets", "QtNetwork", "QtConcurrent", "QtSvg")
                            if (qt / module).is_dir())
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        output = OWNED / (".build-controls-" + stamp + "-" + uuid.uuid4().hex[:8])
        output.mkdir()
        temporary = output / "tmp"
        temporary.mkdir()
        os.environ["TEMP"] = str(temporary)
        os.environ["TMP"] = str(temporary)
        environment = developer_environment(options.vs_root)
        environment["TEMP"] = str(temporary)
        environment["TMP"] = str(temporary)
        compiler = shutil.which("cl.exe", path=environment["PATH"])
        if compiler is None:
            raise RuntimeError("No MSVC compiler in the supplied developer environment")
        flags = ["/nologo", "/std:c++20", "/EHsc", "/MT", "/W4", "/utf-8", "/permissive-",
                 "/Zc:__cplusplus", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/DUNICODE", "/D_UNICODE"]
        if options.mode == "probe":
            run([compiler, "/Bv", "/?"], environment, output, "compiler-probe", records, 30)
        elif options.mode in ("syntax", "prefix-syntax", "domain-syntax"):
            sources = [("domain-syntax", OWNED / "domain_tests.cpp", [])]
            if options.mode in ("syntax", "prefix-syntax"):
                sources.insert(0, ("production-syntax", ROOT / "native/app/service/instance_actions.cpp", qt_flags))
            if prefix is not None:
                probe = output / "prefix_probe.cpp"
                probe_text = (
                    '#include <QtCore/qglobal.h>\n#include <nlohmann/json.hpp>\n'
                    '#define INSTANCE_STRINGIFY_INNER(value) #value\n'
                    '#define INSTANCE_STRINGIFY(value) INSTANCE_STRINGIFY_INNER(value)\n'
                    '#pragma message("READONLY_PREFIX_QT " QT_VERSION_STR)\n'
                    '#pragma message("READONLY_PREFIX_JSON " INSTANCE_STRINGIFY(NLOHMANN_JSON_VERSION_MAJOR) "." '
                    'INSTANCE_STRINGIFY(NLOHMANN_JSON_VERSION_MINOR) "." INSTANCE_STRINGIFY(NLOHMANN_JSON_VERSION_PATCH))\n'
                    'static_assert(QT_VERSION == QT_VERSION_CHECK(6, 8, 3), "Approved prefix must provide Qt 6.8.3");\n'
                )
                with probe.open("x", encoding="ascii", newline="\n") as stream:
                    stream.write(probe_text)
                sources.insert(0, ("prefix-header-syntax", probe, qt_flags))
            for label, source, extra in sources:
                sha = hashlib.sha256(source.read_bytes()).hexdigest()
                source_hashes[str(source.relative_to(ROOT))] = sha
                inputs[str(source)] = sha
                (output / "inputs.json").write_text(json.dumps({"manifestSha256": manifest_sha,
                    "sourceSha256": source_hashes, "inputSha256": inputs, "sourceOverlays": list(map(str, overlays)),
                    "authorizedReadonlyPrefix": str(prefix) if prefix else None,
                    "prefixHeaderSha256": None}, indent=2) + "\n", encoding="ascii")
                run([compiler, *flags, *include, *extra, "/Zs", str(source)], environment,
                    output, label, records)
        else:
            sources = [next((root / name for root in overlays if (root / name).is_file()), SHARED / name)
                       for name in DOMAIN_SOURCES] + [OWNED / "domain_tests.cpp"]
            for source in sources:
                sha = hashlib.sha256(source.read_bytes()).hexdigest()
                source_hashes[str(source.relative_to(ROOT))] = sha
                inputs[str(source)] = sha
            (output / "inputs.json").write_text(json.dumps({"manifestSha256": manifest_sha,
                "sourceSha256": source_hashes, "inputSha256": inputs, "sourceOverlays": list(map(str, overlays))},
                indent=2) + "\n", encoding="ascii")
            objects = []
            for index, source in enumerate(sources):
                obj = output / f"{index:02d}-{source.stem}.obj"
                objects.append(obj)
                run([compiler, *flags, *include, "/O1", "/Gy", "/c", "/Fo" + str(obj), str(source)],
                    environment, output, f"compile-{index:02d}-{source.stem}", records)
            executable = output / "domain_controls.exe"
            run([compiler, "/nologo", "/MT", *map(str, objects), "/Fe" + str(executable), "/link",
                 "/INCREMENTAL:NO", "/OPT:REF", "bcrypt.lib", "advapi32.lib", "shell32.lib", "ole32.lib",
                 "user32.lib", "uuid.lib"], environment, output, "domain-link", records)
            fixture = output / ("fixture-" + uuid.uuid4().hex)
            run([str(executable), str(fixture)], environment, output, "domain-test", records, 45)
        observed_manifest_after = verify_mirror()
        if observed_manifest_after != manifest_sha or verify_overlays(overlays)[1] != overlay_inputs:
            raise RuntimeError("Dependency generation changed during compilation; result is not frozen")
        for name, expected in inputs.items():
            if hashlib.sha256(Path(name).read_bytes()).hexdigest() != expected:
                raise RuntimeError("Compilation input changed during verification: " + name)
        evidence = {"kind": "owned-native-controls", "owner": "app-instance-actions", "mode": options.mode,
                    "manifestSha256": manifest_sha, "observedManifestSha256After": observed_manifest_after,
                    "sourceOverlays": list(map(str, overlays)), "sourceSha256": source_hashes, "inputSha256": inputs,
                    "authorizedReadonlyPrefix": str(prefix) if prefix else None, "prefixHeaderSha256": None,
                    "commands": records, "ownedBytes": owned_bytes(), "status": "PASSED",
                    "productionAcceptance": False}
        (output / "result.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="ascii")
        if options.mode == "probe":
            print("READY compiler environment only; no C++ acceptance result")
        else:
            print(f"PASS {options.mode} controls only; evidence={output.relative_to(ROOT)}/result.json")
        return 0
    except Exception as failure:
        print("BLOCKED/FAIL " + str(failure), file=sys.stderr)
        if output is not None:
            manifest_path = SHARED / "snapshot.json"
            observed_manifest_after = hashlib.sha256(manifest_path.read_bytes()).hexdigest() if manifest_path.is_file() else None
            evidence = {"kind": "owned-native-controls", "owner": "app-instance-actions", "mode": options.mode,
                        "manifestSha256": manifest_sha, "observedManifestSha256After": observed_manifest_after,
                        "sourceOverlays": list(map(str, overlays)), "sourceSha256": source_hashes, "inputSha256": inputs,
                        "authorizedReadonlyPrefix": str(prefix) if prefix else None, "prefixHeaderSha256": None,
                        "productionAcceptance": False, "status": "BLOCKED_OR_FAILED",
                        "error": str(failure), "commands": records}
            (output / "result.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="ascii")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
