#!/usr/bin/env python3
"""Manual, native-architecture CI driver; no dispatch, pushes, secrets, or system dependency reuse."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
from types import SimpleNamespace
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SPEC = importlib.util.spec_from_file_location("macos_delivery_api", HERE / "macos-delivery.py")
api = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(api)
TOOL_SPEC = importlib.util.spec_from_file_location("macos_fixture_tools", HERE / "macos-delivery.test-tools.py")
fixture_tools = importlib.util.module_from_spec(TOOL_SPEC)
TOOL_SPEC.loader.exec_module(fixture_tools)
PHASES = ("preflight", "inventory", "tools", "core", "qt", "app", "probes", "deploy", "materials", "collect")
EXCLUDED = {".git", ".snow", ".tools", ".build", ".deps", ".temp", "build", "builds", "cmakefiles", "scratch", "cache", "caches", "evidence", "records", "reports", "dist", "target", "node_modules", "__pycache__",
            "build-composed", "build-registry"}
# Build/source extensions of the canonical staged roots, each tied to actual tracked inputs:
# .inc: native/core/config/conditional_file_impl.inc (#included by core/config/atomic_file.cpp and
#       required to exist by native/tests/identity/CMakeLists.txt); omitting it broke the hosted
#       app phase at the atomic_file.cpp compile step.
# .cjs: native/tests/identity/generate_builtin_heads.cjs and generate_offline_uuid_golden.cjs are
#       registered as ctest commands whenever node exists (hosted macOS runners provide node).
# .rs:  offline_uuid_legacy_oracle.rs is read byte-exactly by generate_offline_uuid_golden.cjs --check.
# .mjs/.lock: reviewed scripts/native text tooling kept whole in corresponding source
#       (release_assets(.test).mjs, test-fixture-python.lock read by test-fixture-python-verify.py).
# .manifest: tests/native-portability/probe.manifest, referenced by the staged probe.rc; the four
#       .build-policy embed/intermediate manifests stay excluded as tracked build outputs.
EXTENSIONS = {".cpp", ".hpp", ".h", ".c", ".inc", ".cmake", ".txt", ".json", ".py", ".ps1", ".sh", ".cmd", ".in",
              ".qrc", ".rc", ".svg", ".png", ".ico", ".icns", ".ttf", ".woff2", ".md", ".cjs", ".rs", ".mjs", ".lock",
              ".manifest"}
STAGED_SOURCE_ROOTS = ("cmake/", "native/", "scripts/native/", "tests/native-")
# Windows host capacity-pathfinder probe workspace and its committed run artifacts (WSL syntax
# probes, node compile caches, java/node path probes): never consumed by the Apple source build.
EXCLUDED_STAGING_PREFIXES = ("tests/native-capacity-pathfinder/",)


def excluded_source_path(path: PurePosixPath) -> bool:
    return any(part.lower() in EXCLUDED or part.lower().startswith((".build-", ".tools-", ".deps-", ".temp-"))
               or part.lower().endswith((".app", ".dsym")) for part in path.parts)


def excluded_staging_input(value: str) -> bool:
    path = api.safe_relative(value)
    if excluded_source_path(path) or any(part.lower().startswith(".git") for part in path.parts):
        return True
    return value.startswith(EXCLUDED_STAGING_PREFIXES)


def staging_inventory_gaps(tracked) -> list[str]:
    """Tracked staged-root inputs that are neither includable nor explicitly excluded; staging fails closed on any."""
    return sorted(value for value in tracked if value.startswith(STAGED_SOURCE_ROOTS)
                  and not staged_source_path(value) and not excluded_staging_input(value))


def staged_source_path(value: str) -> bool:
    """Includable staged-root input: canonical source AND not explicitly excluded."""
    return canonical_path(value) and not excluded_staging_input(value)


def canonical_path(value: str) -> bool:
    path = api.safe_relative(value)
    if excluded_source_path(path) or value.startswith("native/user/"):
        return False
    if path.name.lower() in {"cmakecache.txt", "credentials.key", "accounts.json", "active_account.json", "secrets.local.json"} or path.name.startswith(".env"):
        return False
    if value in {"CMakeLists.txt", "CMakePresets.json", "CHANGELOG.json", "LICENSE", "LICENSE.txt", "COPYING", "NOTICE"}:
        return True
    if value.startswith("tests/native-macos/inputs/") and path.suffix == ".zip":
        return True
    if path.suffix not in EXTENSIONS:
        return False
    return value.startswith(STAGED_SOURCE_ROOTS)


def qrc_reference_path(parent: PurePosixPath, reference: str) -> str:
    if not reference or reference.startswith("/") or "\\" in reference or ":" in reference:
        raise api.DeliveryError("QRC file reference must be a relative source path")
    parts = list(parent.parts)
    for part in reference.split("/"):
        if part == "..":
            if not parts:
                raise api.DeliveryError("QRC file reference escapes the selected source root")
            parts.pop()
        elif part not in ("", "."):
            parts.append(part)
    return str(api.safe_relative("/".join(parts)))


def verified_historical_source(root: Path, tracked: set[str], base: PurePosixPath, record: dict) -> str:
    if not isinstance(record, dict) or type(record.get("bytes")) is not int or not 0 <= record["bytes"] <= 16 * 1024 * 1024:
        raise api.DeliveryError("Historical QRC source needs a bounded exact byte count")
    relative = str(base / api.safe_relative(record.get("path")))
    expected = api.require_sha(record.get("sha256"))
    if relative not in tracked or not canonical_path(relative):
        raise api.DeliveryError("Historical QRC source is not tracked canonical material: " + relative)
    path = api.checked_file(root, relative)
    if path.stat().st_size != record["bytes"] or api.digest(path) != expected:
        raise api.DeliveryError("Historical QRC source bytes differ from the declared original: " + relative)
    return relative


def historical_qrc_relocations(root: Path, tracked: set[str], selected: set[str]) -> dict:
    relocations = {}
    for relative in sorted(path for path in selected if PurePosixPath(path).name == "asset-manifest.json"):
        manifest = api.small_json(api.checked_file(root, relative))
        if "historicalFiles" not in manifest:
            continue
        rows = manifest["historicalFiles"]
        if manifest.get("schemaVersion") != 1 or not isinstance(rows, list) or not 0 < len(rows) <= api.MAX_FILES:
            raise api.DeliveryError("Historical QRC relocation requires the declared bounded schema")
        base = PurePosixPath(relative).parent
        records = {}
        destinations = set()
        for record in rows:
            if not isinstance(record, dict):
                raise api.DeliveryError("Historical QRC relocation record must be an object")
            original = str(api.safe_relative(record.get("originalPath")))
            destination = str(api.safe_relative(record.get("path")))
            if original in records or destination in destinations:
                raise api.DeliveryError("Historical QRC relocation has duplicate source or destination paths")
            if type(record.get("stillActiveUnchanged")) is not bool or type(record.get("bytes")) is not int or not 0 <= record["bytes"] <= 16 * 1024 * 1024:
                raise api.DeliveryError("Historical QRC relocation needs explicit activity and bounded byte counts")
            api.require_sha(record.get("sha256"))
            if not canonical_path(str(base / destination)):
                raise api.DeliveryError("Historical QRC relocation names forbidden source material")
            records[original] = record
            destinations.add(destination)
        archived = {original: record for original, record in records.items()
                    if original.endswith(".qrc") and original != record["path"] and not record["stillActiveUnchanged"]
                    and str(base / record["path"]) in selected}
        if not archived:
            continue
        historical = verified_historical_source(root, tracked, base, manifest.get("historicalManifest"))
        previous = api.small_json(api.checked_file(root, historical))
        if previous.get("schemaVersion") != 1:
            raise api.DeliveryError("Historical QRC original manifest schema differs")
        originals = api.manifest_entries({"schema": 1, "files": previous.get("assets")}, "Historical QRC original manifest")
        for original in originals:
            record = records.get(original["path"])
            if record is None or (record["sha256"], record["bytes"]) != (original["sha256"], original.get("bytes")):
                raise api.DeliveryError("Historical QRC relocation differs from its bound original manifest")
        original_records = {record["path"]: records[record["path"]] for record in originals}
        for original, record in archived.items():
            if original not in original_records:
                raise api.DeliveryError("Archived QRC is absent from its bound original manifest")
            destination = verified_historical_source(root, tracked, base, record)
            if destination in relocations:
                raise api.DeliveryError("Archived QRC has ambiguous relocation manifests")
            relocations[destination] = (base, PurePosixPath(original).parent, original_records)
    return relocations


def resource_source_paths(root: Path, tracked: set[str], selected: set[str]) -> set[str]:
    result = set(selected)
    queue = sorted(path for path in selected if path.endswith(".qrc"))
    relocations = historical_qrc_relocations(root, tracked, selected)
    oracle_formats = {".svg", ".png", ".jpg", ".jpeg", ".webp", ".gif", ".ico", ".icns", ".ttf", ".otf", ".woff2"}
    for relative in queue:
        document = api.checked_file(root, relative).read_bytes()
        if len(document) > 1024 * 1024 or b"<!DOCTYPE" in document.upper() or b"<!ENTITY" in document.upper():
            raise api.DeliveryError("QRC source must be bounded XML without entity declarations")
        try:
            tree = ET.fromstring(document)
        except ET.ParseError as exception:
            raise api.DeliveryError("Malformed actual QRC source: " + relative) from exception
        if tree.tag != "RCC":
            raise api.DeliveryError("QRC source must use the RCC root")
        for node in tree.iter("file"):
            reference = (node.text or "").strip()
            if relative in relocations:
                # An unchanged archived QRC still names the original, not relocated, resource paths.
                base, original_parent, records = relocations[relative]
                original = qrc_reference_path(original_parent, reference)
                if original not in records:
                    raise api.DeliveryError("Archived QRC reference lacks a bound original relocation: " + original)
                resource = verified_historical_source(root, tracked, base, records[original])
            else:
                resource = qrc_reference_path(PurePosixPath(relative).parent, reference)
            path = api.safe_relative(resource)
            oracle = resource.startswith("src/assets/") and path.suffix.lower() in oracle_formats and not excluded_source_path(path)
            if resource not in tracked or not (canonical_path(resource) or oracle):
                raise api.DeliveryError("QRC resource is not a tracked canonical source or read-only artwork: " + resource)
            api.checked_file(root, resource)
            if resource not in result:
                result.add(resource)
                if resource.endswith(".qrc"):
                    queue.append(resource)
    return result


def inventory(root: Path, tracked: list[str], resolved_commit: str) -> dict:
    if not re.fullmatch(r"[0-9a-f]{40}", resolved_commit):
        raise api.DeliveryError("A full resolved lowercase source commit is required")
    if len(tracked) != len(set(tracked)):
        raise api.DeliveryError("Tracked source inventory has duplicate paths")
    selected = {path for path in tracked if staged_source_path(path)}
    gaps = staging_inventory_gaps(tracked)
    if gaps:
        raise api.DeliveryError("Staged-root tracked inputs are neither includable nor explicitly excluded; classify each before staging: " + "; ".join(gaps[:8]))
    selected = resource_source_paths(root, set(tracked), selected)
    if "native/tests/brand/CMakeLists.txt" in selected:
        # The root brand tests read the trusted ASSET_ORACLE_ROOT contract from the
        # staged source; without these files CMake fatal-errors at configure time.
        tracked_set = set(tracked)
        for relative in api.BRAND_ORACLE_REQUIRED:
            if relative not in tracked_set:
                raise api.DeliveryError("Brand oracle contract file is not tracked in the selected source commit: " + relative)
            selected.add(relative)
    brand_oracle = sorted(relative for relative in api.BRAND_ORACLE_REQUIRED if relative in selected)
    files = []
    for relative in sorted(selected):
        path = api.checked_file(root, relative)
        if path.stat().st_size > 16 * 1024 * 1024:
            raise api.DeliveryError("Canonical source file exceeds the bounded material size")
        files.append({"path": relative, "sha256": api.digest(path), "bytes": path.stat().st_size})
    required = {"CMakeLists.txt", "native/app/CMakeLists.txt", "native/app/main.cpp", "native/app/AppServices.cpp", "native/ui/MainWindow.cpp", "native/ui/CMakeLists.txt", "native/tests/ui/CMakeLists.txt", "scripts/native/deps-lock.json"}
    if not required.issubset({item["path"] for item in files}):
        raise api.DeliveryError("Complete current root/AppServices/Widgets/UI tests/lock are not tracked in the selected source commit")
    if sum(item["bytes"] for item in files) > 64 * 1024 * 1024 or len(files) > api.MAX_FILES:
        raise api.DeliveryError("Canonical source inventory exceeds its 64 MiB bound")
    return {"schema": 1, "kind": "actual-canonical-tracked-source-inventory", "resolvedSourceCommit": resolved_commit,
            "scope": "canonical tracked root/cmake/native/source-tests/tooling plus actual QRC artwork closure; no generated caches, tracked build outputs, VCS metadata, capacity-probe run artifacts, user data, or old 604-file inheritance",
            "qrcReferencedOracleAssets": sorted(path for path in selected if path.startswith("src/assets/")),
            "brandOracleFilesStaged": brand_oracle,
            "files": files, "appleBuildExecuted": False, "productDelivered": False}


def stage(root: Path, manifest: Path, expected: str, destination: Path) -> None:
    verified = api.verify_file_manifest(root, manifest, expected, "Actual selected-commit source")
    if destination.exists():
        raise api.DeliveryError("Production snapshot stage must be a new external root")
    destination.mkdir(parents=True)
    for item in verified["data"]["files"]:
        original = api.checked_file(root, item["path"])
        target = destination / item["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        with original.open("rb") as source, target.open("xb") as output:
            shutil.copyfileobj(source, output)
        target.chmod(original.stat().st_mode & 0o777)
    api.verify_file_manifest(destination, manifest, expected, "Staged selected-commit source")


def preflight(options, inputs: dict, evidence: Path) -> None:
    api.require_sha(options.lock_sha256)
    if options.lock_sha256 != inputs["frozenInputs"]["dependencyLockSha256"]:
        raise api.DeliveryError("Source CI dependency lock SHA must equal the frozen 13-source pin")
    for optional in (options.source_manifest_sha256, options.expected_executable_sha256):
        if optional:
            api.require_sha(optional)
    if not re.fullmatch(r"[0-9a-f]{40}", options.source_ref):
        raise api.DeliveryError("source_ref must be a resolved lowercase 40-character commit, not a branch or shell expression")
    if os.environ.get("GITHUB_REPOSITORY") not in (None, "Nickel-Studio/Pd-Launcher"):
        raise api.DeliveryError("Source CI must run in the authorized Palladium repository")
    environment, host = api.apple_environment(inputs["policy"], options.arch)
    if sys.version_info < (3, 12):
        raise api.DeliveryError("Python >= 3.12 required; record the actual official runner interpreter")
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True, timeout=15)
    if result.stdout.strip() != options.source_ref:
        raise api.DeliveryError("Checkout HEAD does not equal the exact selected source commit")
    if shutil.disk_usage(options.work_root.parent).free < 16 * 1024 ** 3:
        raise api.DeliveryError("Native full source build needs at least 16 GiB runner space")
    api.write_json(evidence / "host.json", {"schema": 1, "kind": "actual-native-apple-host", "host": host,
                                           "discovery": api.discover(), "resolvedSourceCommit": options.source_ref,
                                           "dependencyLockSha256": options.lock_sha256, "productDelivered": False})


def corresponding_materials(options, evidence: Path, source: Path, contract: dict) -> dict:
    deployment_path = evidence / "deployment.json"
    deployment = api.small_json(deployment_path)
    if not deployment.get("complete"):
        raise api.DeliveryError("Corresponding materials require actual completed deployment")
    root = options.work_root / "materials"
    if root.exists():
        raise api.DeliveryError("Materials output must be new")
    root.mkdir()
    manifest = api.small_json(evidence / "production-manifest.json")
    source_archive = root / "application-corresponding-source.tar.gz"
    with tarfile.open(source_archive, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        for item in manifest["files"]:
            path = api.checked_file(source, item["path"])
            if api.digest(path) != item["sha256"] or path.stat().st_size != item["bytes"]:
                raise api.DeliveryError("Application source changed before corresponding-source packaging")
            archive.add(path, arcname="source/" + item["path"], recursive=False)
    roles = {source_archive.name: "application-corresponding-source"}
    downloads = options.work_root / "downloads"
    for entry in contract["sourceRecords"]:
        path = downloads / entry["archive"]
        if api.digest(path) != entry["sha256"]:
            raise api.DeliveryError("Corresponding dependency archive hash mismatch")
        shutil.copyfile(path, root / entry["archive"])
        roles[entry["archive"]] = "dependency-source"
    notices = root / "notices"
    notices.mkdir()
    qt_sources = {item["name"]: options.work_root / "qt" / ("source-" + item["name"]) / item["sourceDirectory"] for item in contract["sourceRecords"] if item["name"] in api.QT_MODULES}
    core_sources = {name: Path(path) for name, path in api.small_json(evidence / "core.json")["sourceDirectories"].items()}
    for name, directory in {**qt_sources, **core_sources}.items():
        copied = 0
        for path in directory.rglob("*"):
            if not path.is_file() or path.is_symlink() or not re.match(r"(?i)^(LICENSE|LICENCE|COPYING|NOTICE|PATENTS|COPYRIGHT|AUTHORS|OFL)", path.name):
                continue
            if path.stat().st_size > 1024 * 1024:
                raise api.DeliveryError("Dependency notice exceeds bound")
            relative = name + "/" + path.relative_to(directory).as_posix()
            target = notices / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            roles["notices/" + relative] = "license-notices"
            copied += 1
        if not copied:
            raise api.DeliveryError("Extracted dependency source has no recorded license notices: " + name)
    for item in manifest["files"]:
        relative = item["path"]
        if relative.startswith("native/assets/") and re.search(r"(?i)(license|licence|notice|ofl|cc-by)", Path(relative).name):
            target = notices / "application" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / relative, target)
            roles[target.relative_to(root).as_posix()] = "license-notices"
    sbom = {"schema": 1, "kind": "corresponding-source-inventory-not-legal-opinion", "resolvedSourceCommit": options.source_ref,
            "deploymentEvidenceSha256": api.digest(deployment_path), "sourceManifestSha256": api.digest(evidence / "production-manifest.json"),
            "dependencyLockSha256": contract["lockSha256"], "inputSupplementSha256": contract["supplementSha256"],
            "sources": contract["sourceRecords"], "toolInputs": contract["tools"], "runtimeBundleInventory": deployment["bundleAudit"]["files"]}
    api.write_json(root / "sbom.json", sbom)
    roles["sbom.json"] = "sbom"
    instructions = ("Corresponding source and relink instructions\n"
                    "Use the included pinned scripts/native/macos-delivery.py, .core.py and .ci.py.\n"
                    "Use a matching native Apple arm64/x86_64 host, Xcode 16.4 build 16F6, SDK 15.5; deployment target 12.0.\n"
                    "Qt 6.8.3 is built as replaceable dynamic frameworks from the three included Qt archives; non-Qt libraries are source-built static.\n"
                    "CMake/Ninja wheel URL/size/hash pins and OpenSSL 3.5.9 LTS are recorded in scripts/native/macos-delivery.supplement.json.\n"
                    "Run phases preflight, inventory, tools, core, qt, app, probes, deploy, materials on a new isolated runner temp root.\n"
                    "The evidence commands.json/log files give exact actual configure/build/link/test commands; do not reuse an old prefix or generated cache.\n"
                    "Replace/rebuild compatible Qt frameworks/plugins, then rerun macdeployqt and codesign with your own identity or ad-hoc '-' for local testing.\n"
                    "This package uses ad-hoc test signing only, no Developer ID/notarization/Gatekeeper distribution claim.\n")
    (root / "build-and-relink.txt").write_text(instructions, encoding="ascii")
    roles["build-and-relink.txt"] = "build-and-relink-instructions"
    (root / "installation-information.txt").write_text("Install the per-architecture .app outside user data. Do not write data in Contents or weaken user protections.\nAfter a local rebuild/relink, re-sign your modified bundle; no release signing credential is required or included.\nAd-hoc test artifacts are not notarized public releases. Full LGPL/install/legal sufficiency requires independent review.\n", encoding="ascii")
    roles["installation-information.txt"] = "installation-information"
    files = [{"path": relative, "role": role, "sha256": api.digest(root / relative), "bytes": (root / relative).stat().st_size} for relative, role in sorted(roles.items())]
    material_manifest = evidence / "material-manifest.json"
    api.write_json(material_manifest, {"schema": 1, "deploymentEvidenceSha256": api.digest(deployment_path), "files": files})
    verified = api.material_evidence(SimpleNamespace(deployment_evidence=deployment_path, material_root=root,
                                                     material_manifest=material_manifest, material_sha256=api.digest(material_manifest)))
    return verified


def collect(options, evidence: Path) -> None:
    destination = options.work_root / "artifacts"
    destination.mkdir(exist_ok=True)
    for path in evidence.iterdir():
        if path.is_file() and not path.is_symlink():
            shutil.copyfile(path, destination / path.name)
    graph = evidence / "generated-install"
    if graph.is_dir() and not graph.is_symlink():
        for path in graph.rglob("*"):
            if path.is_symlink():
                raise api.DeliveryError("Linked raw generated install evidence rejected")
            if path.is_file():
                if path.stat().st_size > api.MAX_SMALL_INPUT:
                    raise api.DeliveryError("Raw generated install evidence exceeds bound")
                target = destination / "generated-install" / path.relative_to(graph)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
    for name in ("app-build", "probe-build"):
        root = options.work_root / name
        for path in root.rglob("*") if root.exists() else ():
            if not path.is_file() or path.is_symlink():
                continue
            if path.name in {"ctest.xml", "LastTest.log", "LastTestsFailed.log", "CMakeCache.txt", "compile_commands.json"} or path.suffix == ".png" or path.name.startswith("ui-probe-") or ".cmake/api/v1/reply" in path.as_posix():
                if path.stat().st_size > 16 * 1024 * 1024:
                    raise api.DeliveryError("Test/build artifact exceeds bounded raw-output size")
                target = destination / name / path.relative_to(root)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
    for name in ("packages", "symbols", "materials"):
        root = options.work_root / name
        if root.is_dir():
            target = destination / name
            if not target.exists():
                shutil.copytree(root, target, symlinks=True)
    api.write_json(destination / "artifact-status.json", {"schema": 1, "kind": "collected-native-ci-attempt-materials",
                                                         "resolvedSourceCommit": options.source_ref, "architecture": options.arch,
                                                         "completePhases": sorted(path.stem for path in evidence.glob("*.json") if api.small_json(path).get("complete")),
                                                         "productDelivered": False, "fullSevenRouteSemanticReviewVerified": False,
                                                         "notarizationVerified": False, "minimumMacos12RuntimeVerified": False})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", required=True, choices=PHASES)
    parser.add_argument("--arch", required=True, choices=api.ARCHITECTURES)
    parser.add_argument("--source-ref", required=True)
    parser.add_argument("--source-manifest-sha256", default="")
    parser.add_argument("--lock-sha256", default="b974a9bcd5b9f6fab32ffbf248ccd23ab9938ff731a91c9aff2c162d70dd136d")
    parser.add_argument("--expected-executable-sha256", default="")
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=2)
    options = parser.parse_args()
    evidence = options.work_root / "evidence"
    evidence.mkdir(parents=True, exist_ok=True)
    try:
        inputs = api.validate_inputs(HERE / "macos-delivery.inputs.json")
        pins = ROOT / inputs["frozenInputs"]["root"]
        supplement = HERE / "macos-delivery.supplement.json"
        contract = api.verify_source_contract(pins / "deps-lock.json", pins / "source-contract.json", inputs["frozenInputs"]["sourceContractSha256"],
                                              inputs, pins / "verification-rules.json", supplement, api.digest(supplement))
        common = dict(arch=options.arch, jobs=options.jobs, downloads=options.work_root / "downloads", source_root=options.work_root / "source",
                      production_manifest=evidence / "production-manifest.json", production_sha256=api.digest(evidence / "production-manifest.json") if (evidence / "production-manifest.json").is_file() else None,
                      contract_sha256=contract["contractSha256"], output=evidence / (options.phase + ".json"))
        if options.phase == "preflight":
            preflight(options, inputs, evidence)
        elif options.phase == "inventory":
            resolved = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True, timeout=15).stdout.strip()
            if resolved != options.source_ref:
                raise api.DeliveryError("Inventory selected commit mismatch")
            tracked = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True, timeout=15).stdout.decode("utf-8").split("\x00")
            value = inventory(ROOT, [path for path in tracked if path], resolved)
            api.write_json(evidence / "production-manifest.json", value)
            actual = api.digest(evidence / "production-manifest.json")
            if options.source_manifest_sha256 and actual != api.require_sha(options.source_manifest_sha256):
                raise api.DeliveryError("Dispatch source manifest SHA256 differs from actual canonical tracked source bytes")
            if api.digest(ROOT / "scripts/native/deps-lock.json") != options.lock_sha256:
                raise api.DeliveryError("Actual selected-commit dependency lock differs from the 13-pin contract")
            stage(ROOT, evidence / "production-manifest.json", actual, options.work_root / "source")
            api.write_json(evidence / "inventory.json", {"schema": 1, "complete": True, "resolvedSourceCommit": resolved, "manifestSha256": actual,
                                                       "filesVerified": len(value["files"]), "classification": "source-byte-verification-not-build-or-runtime-PASS"})
        elif options.phase == "tools":
            api.fetch_sources(SimpleNamespace(**common, include_core=True, include_tools=True), inputs, contract)
            installed = api.setup_tools(SimpleNamespace(**common, tools_root=options.work_root / "tools"), inputs, contract)
            api.write_json(evidence / "tools.json", installed)
            fixture_tools.install(options.arch, options.work_root / "test-python", options.work_root / "test-downloads",
                                  evidence / "test-tools.json", inputs["policy"], api)
            if "GITHUB_PATH" in os.environ:
                with Path(os.environ["GITHUB_PATH"]).open("a", encoding="utf-8") as stream:
                    stream.write("\n".join(installed["pathDirectories"]) + "\n")
        elif options.phase == "core":
            api.build_core(SimpleNamespace(**common, tools_root=options.work_root / "core", prefix_manifest=evidence / "core-prefix-manifest.json"), inputs, contract)
        elif options.phase == "qt":
            api.build_qt(SimpleNamespace(**common, tools_root=options.work_root / "qt"), inputs, contract)
        elif options.phase in ("app", "probes"):
            core = api.small_json(evidence / "core.json")
            test_python = fixture_tools.verify_environment(evidence / "test-tools.json", options.arch, api)
            arguments = SimpleNamespace(**common, target=None, core_prefix=Path(core["prefix"]), core_manifest=evidence / "core-prefix-manifest.json",
                                        core_sha256=api.digest(evidence / "core-prefix-manifest.json"), qt_evidence=evidence / "qt.json",
                                        test_python=test_python, build_root=options.work_root / ("app-build" if options.phase == "app" else "probe-build"))
            (api.build_app if options.phase == "app" else api.run_probes)(arguments, inputs, contract)
        elif options.phase == "deploy":
            packages = options.work_root / "packages"
            packages.mkdir()
            api.deploy(SimpleNamespace(arch=options.arch, build_evidence=evidence / "app.json", qt_evidence=evidence / "qt.json",
                                       install_root=options.work_root / "product-install", symbols_root=options.work_root / "symbols",
                                       archive=packages / ("palladium-macos-" + options.arch + ".zip"), output=evidence / "deployment.json"), inputs)
            deployment = api.small_json(evidence / "deployment.json")
            if options.expected_executable_sha256 and deployment["executableSha256"] != api.require_sha(options.expected_executable_sha256):
                raise api.DeliveryError("Deployed executable does not match the independently pinned artifact SHA256")
        elif options.phase == "materials":
            api.write_json(evidence / "materials.json", corresponding_materials(options, evidence, options.work_root / "source", contract))
        elif options.phase == "collect":
            collect(options, evidence)
        return 0
    except Exception as exception:
        failure = evidence / (options.phase + "-failure.json")
        if not failure.exists():
            api.write_json(failure, {"schema": 1, "complete": False, "phase": options.phase, "failure": str(exception),
                                     "architecture": options.arch, "productDelivered": False, "appleRuntimeExecuted": False})
        print(str(exception), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
