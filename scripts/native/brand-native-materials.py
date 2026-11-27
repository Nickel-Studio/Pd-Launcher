#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Read-only inspection of installed native dependencies; never a release bundle."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / ".snow/brand-tools"))
SPEC = importlib.util.spec_from_file_location("nickel_native_material_legal", ROOT / "scripts/native/brand-legal.py")
LEGAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LEGAL)
from spdx_tools.spdx.parser.parse_anything import parse_file
from spdx_tools.spdx.validation.document_validator import validate_full_spdx_document

PREFIX = "install/windows-x64-static"
BUILD = "build/windows-x64-static"
LOCK = ".snow/native-shared/scripts/native/deps-lock.json"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
LICENSE_NAMES = re.compile(r"(?i)^(?:copying|licen[cs]e|notice)(?:$|[._-])")
CACHE_KEYS = re.compile(r"^(?:BUILD_SHARED_LIBS|CMAKE_(?:BUILD_TYPE|MSVC_RUNTIME_LIBRARY|INSTALL_PREFIX|HOME_DIRECTORY)|QT_FEATURE_.*|CURL_USE_.*|ENABLE_.*)$")
CATEGORIES = {"curl": "http", "libarchive": "archive", "zlib": "compression", "xz": "compression", "zstd": "compression", "bzip2": "compression"}
WRAPPERS = {"bzip2": "bzip2", "gumbo": "gumbo", "yaml": "yaml"}
HEADER_TREES = {"json": "include/nlohmann", "tomlplusplus": "include/toml++"}


class InspectionError(ValueError):
    pass


def safe_relative(value: str) -> PurePosixPath:
    if not isinstance(value, str) or not value.strip():
        raise InspectionError("An exact non-empty relative material path is required")
    normalized = value.replace("\\", "/")
    path = PurePosixPath(normalized)
    if path.is_absolute() or ".." in path.parts or re.match(r"^[A-Za-z]:", normalized):
        raise InspectionError(f"Unsafe material path: {value}")
    return path


class ReadOnlyTools:
    """Only the lead-authorized local tools portal may cross the checkout boundary."""
    def __init__(self, checkout: Path):
        self.checkout = checkout.resolve()
        self.portal = self.checkout / ".snow/native-tools"
        if not self.portal.is_dir():
            raise InspectionError("Missing authorized .snow/native-tools read-only entry")
        self.base = self.portal.resolve(strict=True)
        self.records = {}

    def path(self, relative: str) -> Path:
        parts = safe_relative(relative).parts
        cursor = self.portal
        for part in parts:
            cursor = cursor / part
            if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
                raise InspectionError(f"Nested material symlinks/junctions are not followed: {relative}")
        if not cursor.resolve().is_relative_to(self.base):
            raise InspectionError(f"Material escaped the authorized tools entry: {relative}")
        return cursor

    def record(self, relative: str, kind: str) -> dict:
        path = self.path(relative)
        if not path.is_file() or not path.stat().st_size:
            raise InspectionError(f"Missing or empty actual material: {relative}")
        before = path.stat()
        stamp = (before.st_size, before.st_mtime_ns)
        if relative in self.records:
            previous_stamp, previous = self.records[relative]
            if stamp != previous_stamp:
                raise InspectionError(f"Material changed while inspecting: {relative}")
            return dict(previous, kind=kind)
        digest = LEGAL.hash_file(path)
        after = path.stat()
        if stamp != (after.st_size, after.st_mtime_ns):
            raise InspectionError(f"Material changed while inspecting: {relative}")
        result = {"path": ".snow/native-tools/" + safe_relative(relative).as_posix(), "kind": kind,
                  "sha256": digest, "bytes": after.st_size, "available": True}
        self.records[relative] = (stamp, result)
        return dict(result)

    def text(self, relative: str) -> str:
        return self.path(relative).read_text(encoding="utf-8-sig")


def coff_archive(path: Path) -> dict:
    try:
        return LEGAL.library_artifact(path)
    except LEGAL.MaterialError as error:
        raise InspectionError(str(error)) from error


def pe_artifact(path: Path) -> dict:
    with path.open("rb") as stream:
        head = stream.read(64)
        if len(head) != 64 or head[:2] != b"MZ":
            raise InspectionError(f"Not an actual PE image: {path.name}")
        stream.seek(struct.unpack_from("<I", head, 60)[0])
        pe = stream.read(24)
    if len(pe) != 24 or pe[:4] != b"PE\0\0":
        raise InspectionError(f"Invalid PE signature: {path.name}")
    machine = struct.unpack_from("<H", pe, 4)[0]
    characteristics = struct.unpack_from("<H", pe, 22)[0]
    return {"kind": "shared-library" if characteristics & 0x2000 else "executable", "machine": machine}


def cache_options(text: str) -> dict:
    values = {}
    for line in text.splitlines():
        match = re.match(r"^([^#/:][^:]*):[^=]*=(.*)$", line)
        if match and CACHE_KEYS.fullmatch(match[1]):
            values[match[1]] = match[2]
    return values


def install_paths(text: str, prefixes: list[str]) -> set[str]:
    result = set()
    normalized = [prefix.replace("\\", "/").rstrip("/") + "/" for prefix in prefixes]
    for line in text.splitlines():
        line = line.strip().replace("\\", "/")
        if not line:
            continue
        matches = [prefix for prefix in normalized if line.casefold().startswith(prefix.casefold())]
        if not matches:
            raise InspectionError("Install manifest path is not under its actual recorded installation prefix")
        relative = line[len(max(matches, key=len)):]
        result.add(safe_relative(relative).as_posix())
    return result


def read_attributions(tools: ReadOnlyTools, source: str) -> tuple[dict, list[str]]:
    entries, relaxed = {}, []
    root = tools.path(source)
    for path in sorted(root.rglob("qt_attribution.json")):
        relative = path.relative_to(tools.portal).as_posix()
        text = tools.text(relative)
        try:
            document = json.loads(text)
        except json.JSONDecodeError as error:
            # Only literal JSON whitespace controls in upstream attribution strings are tolerated.
            if (not error.msg.startswith("Invalid control character") or text[error.pos] not in "\r\n\t"
                    or any(ord(char) < 0x20 and char not in "\r\n\t" for char in text)):
                raise InspectionError(f"Unsupported upstream attribution JSON: {relative}") from error
            try:
                document = json.loads(text, strict=False)
            except json.JSONDecodeError as relaxed_error:
                raise InspectionError(f"Unsupported upstream attribution JSON: {relative}") from relaxed_error
            relaxed.append(relative)
        rows = document if isinstance(document, list) else [document]
        if not rows or any(not isinstance(row, dict) for row in rows):
            raise InspectionError(f"Invalid upstream attribution structure: {relative}")
        for row in rows:
            identifier = row.get("Id")
            if not isinstance(identifier, str) or not identifier or identifier in entries:
                raise InspectionError(f"Missing/duplicate upstream attribution: {relative}")
            entries[identifier] = {"data": row, "metadata": tools.record(relative, "qt-attribution")}
    return entries, relaxed


def attribution_materials(tools: ReadOnlyTools, source: str, entry: dict) -> list[dict]:
    row = entry["data"]
    base = PurePosixPath(entry["metadata"]["path"]).relative_to(".snow/native-tools").parent
    filenames = row.get("LicenseFile", [])
    filenames = [filenames] if isinstance(filenames, str) else filenames
    result = []
    for name in filenames:
        candidate = tools.path(base.as_posix()) / name
        resolved = candidate.resolve()
        source_root = tools.path(source).resolve()
        if not resolved.is_relative_to(source_root):
            raise InspectionError(f"Upstream license reference escaped its source archive: {name}")
        relative = (PurePosixPath(source) / resolved.relative_to(source_root).as_posix()).as_posix()
        result.append(tools.record(relative, "license"))
    if not result:
        for identifier in LEGAL.spdx_ids(row["LicenseId"]):
            candidate = f"{source}/LICENSES/{identifier}.txt"
            if tools.path(candidate).is_file():
                result.append(tools.record(candidate, "license"))
        if not result and isinstance(row.get("License"), str) and len(row["License"].strip()) >= 100:
            result.append(dict(entry["metadata"], kind="license-in-attribution"))
    if not result:
        raise InspectionError(f"Actual license text missing for Qt attribution {row['Id']}")
    return result


def entrypoint_target_matches(text: str) -> bool:
    target = re.search(r"^[ \t]*set_target_properties\([ \t]*Qt6::EntryPointImplementation[ \t]+PROPERTIES\b(.*?)^[ \t]*\)",
                       text, re.MULTILINE | re.DOTALL)
    return bool(target and re.search(r'^[ \t]*IMPORTED_LOCATION_RELEASE[ \t]+"\$\{_IMPORT_PREFIX\}/lib/Qt6EntryPoint\.lib"[ \t]*\r?$',
                                     target.group(1), re.MULTILINE))


def qt_inspection(tools: ReadOnlyTools, locked: dict) -> dict:
    name, version = locked["name"], locked["version"]
    source = "sources/" + locked["sourceDirectory"]
    relative = f"{PREFIX}/sbom/{name}-{version}.spdx"
    sbom = tools.record(relative, "upstream-spdx")
    document = parse_file(str(tools.path(relative)))
    errors = [str(message) for message in validate_full_spdx_document(document)]
    if errors:
        raise InspectionError(f"Upstream {name} SPDX validation failed: {'; '.join(errors)}")
    metadata, relaxed = read_attributions(tools, source)
    packages = {row.spdx_id: row for row in document.packages}
    attribution_rows, library_rows = [], []
    for package in document.packages:
        if "_Attribution_" not in package.name:
            continue
        # SPDX element IDs normalize underscores; the package name retains the source attribution ID.
        identifier = package.name.split("_Attribution_", 1)[1]
        if identifier not in metadata:
            raise InspectionError(f"Upstream SBOM attribution has no actual source metadata: {identifier}")
        entry = metadata[identifier]
        source_expression = entry["data"]["LicenseId"]
        if str(package.license_concluded) != source_expression:
            raise InspectionError(f"Qt attribution license drift: {identifier}")
        attribution_rows.append({"upstreamSpdxId": package.spdx_id, "id": identifier, "name": package.name,
                                 "licenseConcluded": str(package.license_concluded),
                                 "copyrightText": str(package.copyright_text), "metadata": entry["metadata"],
                                 "licenseFiles": attribution_materials(tools, source, entry)})
    for file in document.files:
        if not file.name.lower().endswith(".lib"):
            continue
        installed = safe_relative(file.name.removeprefix("./")).as_posix()
        material = tools.record(f"{PREFIX}/{installed}", "library")
        expected = next((row.value for row in file.checksums if row.algorithm.name == "SHA1"), None)
        with tools.path(f"{PREFIX}/{installed}").open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha1").hexdigest()
        if not expected or actual != expected:
            raise InspectionError(f"Installed Qt library differs from upstream SBOM: {installed}")
        owners = sorted({edge.spdx_element_id for edge in document.relationships
                         if edge.relationship_type.name == "CONTAINS" and edge.related_spdx_element_id == file.spdx_id
                         and edge.spdx_element_id in packages})
        if not owners:
            raise InspectionError(f"Qt installed library has no upstream owning package: {installed}")
        library_rows.append({"installedPath": installed, "material": material, "upstreamSpdxId": file.spdx_id,
                             "upstreamSha1": expected, "sha1Matches": True, "ownerPackages": owners,
                             "upstreamLicenseExpressions": sorted({str(packages[key].license_concluded) for key in owners})})
    extracted = []
    for row in document.extracted_licensing_info:
        license_path = f"{source}/LICENSES/{row.license_id}.txt"
        text = tools.text(license_path)
        if text.strip() != row.extracted_text.strip():
            raise InspectionError(f"Upstream extracted license differs from actual source text: {row.license_id}")
        extracted.append({"licenseId": row.license_id, "material": tools.record(license_path, "extracted-license-text"),
                          "upstreamExtractedTextMatches": True})
    supplemental = []
    if name == "qtbase":
        entry = packages.get("SPDXRef-Package-qtbase-qt-module-EntryPointPrivate")
        version_file = f"{PREFIX}/modules/EntryPointPrivate.json"
        module = json.loads(tools.text(version_file))
        source_file = source + "/src/entrypoint/qtentrypoint_win.cpp"
        target_file = f"{PREFIX}/lib/cmake/Qt6EntryPointPrivate/Qt6EntryPointPrivateTargets-release.cmake"
        if not entry or module.get("version") != version or entry.version != version:
            raise InspectionError("Qt EntryPoint source/install/SBOM package version mismatch")
        if "BSD-3-Clause" not in str(entry.license_concluded) or "SPDX-License-Identifier: LicenseRef-Qt-Commercial OR BSD-3-Clause" not in tools.text(source_file):
            raise InspectionError("Qt EntryPoint actual permissive source license is missing")
        if not entrypoint_target_matches(tools.text(target_file)):
            raise InspectionError("Qt EntryPoint installed target does not bind the actual library")
        supplemental.append({"installedPath": "lib/Qt6EntryPoint.lib", "upstreamFileEntryPresent": False,
                             "upstreamSpdxId": entry.spdx_id, "openSourceLicenseExpression": "BSD-3-Clause",
                             "installedTargetMatches": True,
                             "material": tools.record(f"{PREFIX}/lib/Qt6EntryPoint.lib", "static-library"),
                             "versionEvidence": tools.record(version_file, "installed-module-version"),
                             "targetEvidence": tools.record(target_file, "installed-cmake-target"),
                             "sourceEvidence": tools.record(source_file, "source-license-header"),
                             "licenseText": tools.record(source + "/LICENSES/BSD-3-Clause.txt", "license")})
    return {"upstreamSbom": sbom, "independentValidationErrorCount": 0, "packages": len(document.packages),
            "files": len(document.files), "libraryCoverage": library_rows, "attributions": attribution_rows,
            "supplementalLibraryCoverage": supplemental, "extractedLicenses": extracted,
            "upstreamRelaxedJsonFiles": relaxed,
            "scopeLimit": "Installed upstream SBOM coverage, not a final launcher link inventory."}


def inspect(checkout: Path, lock_path: str) -> dict:
    tools = ReadOnlyTools(checkout)
    lock_file = LEGAL.inside(checkout, lock_path)
    lock_bytes = lock_file.read_bytes()
    lock = json.loads(lock_bytes)
    sources = lock.get("sources", [])
    if not sources or len({row.get("name") for row in sources}) != len(sources):
        raise InspectionError("Dependency lock must have non-empty unique actual components")
    logs, prefixes = [], [str(tools.path(PREFIX).resolve())]
    for path in sorted(tools.path("logs").glob("bootstrap-windows-x64-static-*.json")):
        relative = path.relative_to(tools.portal).as_posix()
        document = json.loads(tools.text(relative))
        commands = document.get("commands", [])
        if not document.get("complete") or not commands or any(row.get("returnCode") != 0 for row in commands):
            raise InspectionError(f"Dependency bootstrap is incomplete or failed: {relative}")
        if not all(isinstance(row.get("argv"), list) and row["argv"] and all(isinstance(v, str) and v for v in row["argv"]) for row in commands):
            raise InspectionError(f"Dependency bootstrap has no actual command argv: {relative}")
        prefixes.append(document["prefix"])
        logs.append({"material": tools.record(relative, "bootstrap-log"), "document": document})
    if not logs:
        raise InspectionError("Actual dependency bootstrap logs are required")
    components, claimed = [], {}
    for locked in sources:
        name = locked["name"]
        print(f"Read-only dependency inspection: {name} {locked['version']}", file=sys.stderr, flush=True)
        if not SHA256.fullmatch(locked.get("sha256", "")):
            raise InspectionError(f"Missing locked archive SHA-256: {name}")
        archive = tools.record("downloads/" + locked["archive"], "source-archive")
        if archive["sha256"] != locked["sha256"]:
            raise InspectionError(f"Actual source archive hash differs from lock: {name}")
        LEGAL.verify_source_archive(tools.path("downloads/" + locked["archive"]))
        source = "sources/" + locked["sourceDirectory"]
        if not tools.path(source).is_dir():
            raise InspectionError(f"Missing actual source tree: {name}")
        source_logs = []
        for log in logs:
            for row in log["document"].get("sources", []):
                if row.get("name") != name:
                    continue
                if row.get("version") != locked["version"] or row.get("verifiedSha256") != locked["sha256"]:
                    raise InspectionError(f"Bootstrap source differs from dependency lock: {name}")
                source_logs.append(log["material"])
        if not source_logs:
            raise InspectionError(f"Dependency has no successful bootstrap source evidence: {name}")
        build_name = "yaml-native" if name == "yaml" else name
        build = f"{BUILD}/{build_name}"
        options = cache_options(tools.text(build + "/CMakeCache.txt"))
        install = tools.text(build + "/install_manifest.txt")
        allowed_prefixes = prefixes + [options.get("CMAKE_INSTALL_PREFIX", "")]
        installed = install_paths(install, [value for value in allowed_prefixes if value])
        artifacts = []
        for relative in sorted(installed):
            library = (relative.startswith("lib/") or relative.startswith("plugins/")) and relative.lower().endswith(".lib")
            shared = relative.startswith("bin/") and relative.lower().endswith(".dll")
            header = name in HEADER_TREES and relative.startswith(HEADER_TREES[name] + "/") and PurePosixPath(relative).suffix in {".h", ".hpp"}
            if not (library or shared or header):
                continue
            path = tools.path(PREFIX + "/" + relative)
            details = coff_archive(path) if library else pe_artifact(path) if shared else {}
            kind = details["kind"] if details else "installed-header"
            if relative in claimed and claimed[relative] != name:
                raise InspectionError(f"Installed artifact claimed by multiple locked components: {relative}")
            claimed[relative] = name
            artifacts.append({**tools.record(PREFIX + "/" + relative, kind), "binary": details})
        if not artifacts:
            raise InspectionError(f"No actual installed library/header artifacts: {name}")
        licenses = [tools.record(source + "/" + path.name, "upstream-license-or-notice")
                    for path in sorted(tools.path(source).iterdir()) if path.is_file() and LICENSE_NAMES.match(path.name)]
        if name.startswith("qt"):
            licenses += [tools.record(path.relative_to(tools.portal).as_posix(), "upstream-license")
                         for path in sorted(tools.path(source + "/LICENSES").glob("*.txt"))]
        if not licenses:
            raise InspectionError(f"No actual upstream license/notice: {name}")
        materials = [tools.record(build + "/" + file, kind) for file, kind in
                     [("CMakeCache.txt", "build-cache"), ("install_manifest.txt", "install-manifest"), ("build.ninja", "build-commands")]]
        wrappers = []
        if name in WRAPPERS:
            wrapper = f".snow/native-shared/scripts/native/deps/{WRAPPERS[name]}"
            for file in sorted((checkout / wrapper).glob("*.cmake")) + sorted((checkout / wrapper).glob("CMakeLists.txt")):
                relative = file.relative_to(checkout).as_posix()
                actual = LEGAL.inside(checkout, relative)
                wrappers.append({"path": relative, "kind": "build-wrapper", "sha256": LEGAL.hash_file(actual),
                                 "bytes": actual.stat().st_size, "available": True})
            if not wrappers:
                raise InspectionError(f"Missing actual project build wrapper: {name}")
        component = {"id": name, "version": locked["version"], "category": "qt" if name.startswith("qt") else CATEGORIES.get(name, "other"),
                     "origin": locked["url"], "lockedLicenseExpression": locked["license"],
                     "sourceArchive": dict(archive, lockedSha256Matches=True, readablePreferredFormArchive=True),
                     "sourceDirectory": ".snow/native-tools/" + source, "artifacts": artifacts,
                     "licenseAndNoticeFiles": licenses, "buildMaterials": materials + wrappers,
                     "successfulBootstrapLogs": source_logs, "buildOptions": options,
                     "finalExecutableUseVerified": False}
        if name.startswith("qt"):
            component["qt"] = qt_inspection(tools, locked)
            qt_covered = {row["installedPath"] for row in component["qt"]["libraryCoverage"] + component["qt"]["supplementalLibraryCoverage"]}
            qt_installed = {row["path"].removeprefix(".snow/native-tools/" + PREFIX + "/") for row in artifacts if row["kind"] == "static-library"}
            if qt_installed != qt_covered:
                raise InspectionError("Installed Qt library license coverage gap: " + ", ".join(sorted(qt_installed ^ qt_covered)))
            resource_objects = []
            for relative in sorted(installed):
                if relative.startswith("lib/objects-") and relative.lower().endswith(".obj"):
                    if LEGAL.binary_kind(tools.path(PREFIX + "/" + relative)) != "object":
                        raise InspectionError(f"Qt resource is not a real object: {relative}")
                    resource_objects.append(tools.record(PREFIX + "/" + relative, "dependency-resource-object"))
            component["dependencyResourceObjects"] = resource_objects
        if name == "libarchive":
            component["perFileLicenseEvidence"] = [tools.record(source + "/libarchive/" + filename, "source-license-header") for filename in
                                                   ["archive_read_support_filter_compress.c", "archive_write_add_filter_compress.c",
                                                    "archive_parse_date.c", "archive_blake2.h", "archive_blake2_impl.h",
                                                    "archive_blake2s_ref.c", "archive_blake2sp_ref.c"]]
            component["licenseScopeLimit"] = "Root BSD-2-Clause is not the complete distribution license: COPYING identifies BSD-3-Clause compress files, public-domain date parser, triple-licensed BLAKE2 and separately licensed build scripts. Per-file/source and final-link review remain required."
        if name == "xz":
            component["licenseScopeLimit"] = "liblzma is 0BSD; complete archive also contains LGPL/GPL tools and build files. The archive is not uniformly 0BSD."
        if name == "zlib":
            component["licenseScopeLimit"] = "Both zlibstatic.lib and the separate import-library/DLL are installed; installation does not prove the final launcher is entirely static."
        components.append(component)
    installed_libs = set()
    for directory in ["lib", "plugins"]:
        for path in tools.path(PREFIX + "/" + directory).rglob("*.lib"):
            installed_libs.add(path.relative_to(tools.path(PREFIX)).as_posix())
    unclaimed = sorted(installed_libs - set(claimed))
    if unclaimed:
        raise InspectionError("Installed libraries omitted from dependency inspection: " + ", ".join(unclaimed))
    return {"schemaVersion": 1, "kind": "native-dependency-inspection", "scope": "native-dependency-snapshot",
            "observedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "inspectionPassed": True, "materialBundleCollected": False, "dependencySnapshotValidationPassed": False,
            "nativeLegalMaterialValidationPassed": False, "nativeReleaseReady": False,
            "lock": {"path": lock_path, "sha256": hashlib.sha256(lock_bytes).hexdigest()},
            "components": components, "installedLibraryCount": len(installed_libs), "unclaimedLibraries": unclaimed,
            "bootstrapLogs": [{"material": row["material"], "complete": True,
                               "successfulCommands": len(row["document"]["commands"]),
                               "recordedLockSha256": row["document"].get("lockSha256"),
                               "recordedLockMatchesCurrent": row["document"].get("lockSha256") == hashlib.sha256(lock_bytes).hexdigest()}
                              for row in logs],
            "unverifiedGates": ["matching-final-executable-and-link-inventory", "complete-application-source-and-objects",
                                "release-license-selection-and-third-party-closure", "actual-replacement-library-relink-probe",
                                "copied-and-independently-verified-native-release-bundle"],
            "notes": ["Read-only hash/source availability inspection; no source archives, libraries or objects are copied.",
                      "The authorized local .snow/native-tools entry is read-only. Ordinary bundle path confinement is not relaxed.",
                      "Installed Qt object resources are dependency objects, not Corresponding Application Code.",
                      "An upstream SPDX with zero validation errors is not proof of the final executable dependency closure."]}

def observation_schema_errors(document: dict) -> list[str]:
    schema = json.loads((ROOT / "docs/native/licenses/native-dependency-evidence.schema.json").read_text(encoding="utf-8"))
    return ["Inspection schema: " + error.message for error in LEGAL.Draft202012Validator(schema).iter_errors(document)]


def verify_observation(checkout: Path, document: dict) -> list[str]:
    if not isinstance(document, dict) or document.get("kind") != "native-dependency-inspection" or document.get("scope") != "native-dependency-snapshot":
        return ["Not a native dependency inspection"]
    errors = observation_schema_errors(document)
    if errors:
        return errors
    if any(document.get(key) is not False for key in ["materialBundleCollected", "dependencySnapshotValidationPassed", "nativeLegalMaterialValidationPassed", "nativeReleaseReady"]):
        errors.append("Read-only inspection must not claim bundle validation or release authorization")
    try:
        tools = ReadOnlyTools(checkout)
    except (InspectionError, OSError) as error:
        return errors + [str(error)]

    def walk(value):
        if isinstance(value, dict):
            if "path" in value and "sha256" in value:
                try:
                    path = value["path"]
                    if not isinstance(path, str) or not isinstance(value["sha256"], str) or not SHA256.fullmatch(value["sha256"]):
                        raise InspectionError("Observed material needs an exact path and SHA-256")
                    if path.startswith(".snow/native-tools/"):
                        actual = tools.record(path.removeprefix(".snow/native-tools/"), value.get("kind", "file"))
                        if actual["sha256"] != value["sha256"] or ("bytes" in value and actual["bytes"] != value["bytes"]):
                            raise InspectionError(f"Observed material drift: {path}")
                    else:
                        actual = LEGAL.inside(checkout, path)
                        if LEGAL.hash_file(actual) != value["sha256"] or ("bytes" in value and actual.stat().st_size != value["bytes"]):
                            raise InspectionError(f"Observed local material drift: {path}")
                except (InspectionError, LEGAL.MaterialError, OSError) as error:
                    errors.append(str(error))
            for child in value.values():
                walk(child)
        elif isinstance(value, list):
            for child in value:
                walk(child)
    walk(document)
    if errors:
        return errors
    try:
        # Rehashing listed rows alone cannot detect omitted artifacts or fabricated counts/licenses.
        current = inspect(checkout, document["lock"]["path"])
        recorded = dict(document)
        current.pop("observedAt", None)
        recorded.pop("observedAt", None)
        for key in sorted(set(current) | set(recorded)):
            if current.get(key) != recorded.get(key):
                errors.append(f"Inspection differs from current actual materials: {key}")
    except (InspectionError, LEGAL.MaterialError, OSError, ValueError, KeyError, TypeError) as error:
        errors.append(str(error))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    read = commands.add_parser("inspect", help="Read/hash actual dependency files without copying them")
    read.add_argument("--lock", default=LOCK)
    read.add_argument("--output", default="docs/native/licenses/native-dependency-evidence.json")
    verify = commands.add_parser("verify-observation", help="Rehash read-only evidence against its authorized local tools entry")
    verify.add_argument("--evidence", required=True)
    args = parser.parse_args()
    try:
        if args.command == "inspect":
            output = LEGAL.inside(ROOT, args.output)
            if output.exists():
                raise InspectionError("Refusing to overwrite evidence; select an unused exact output path")
            document = inspect(ROOT, args.lock)
            schema_errors = observation_schema_errors(document)
            if schema_errors:
                raise InspectionError("; ".join(schema_errors[:4]))
            raw = (json.dumps(document, indent=2, ensure_ascii=True) + "\n").encode("utf-8")
            parent = output.parent
            while not parent.exists():
                parent = parent.parent
            if shutil.disk_usage(parent).free < len(raw) + 16 * 1024 * 1024:
                raise InspectionError("Insufficient workspace disk space before writing inspection evidence")
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("xb") as stream:
                stream.write(raw)
            result = {"output": args.output, "sha256": LEGAL.hash_file(output), "inspectionPassed": True,
                      "components": len(document["components"]), "installedLibraryCount": document["installedLibraryCount"],
                      "materialBundleCollected": False, "nativeLegalMaterialValidationPassed": False, "nativeReleaseReady": False}
        else:
            document = json.loads(LEGAL.inside(ROOT, args.evidence).read_text(encoding="utf-8"))
            errors = verify_observation(ROOT, document)
            result = {"inspectionPassed": not errors, "errors": errors, "materialBundleCollected": False,
                      "nativeLegalMaterialValidationPassed": False, "nativeReleaseReady": False}
        print(json.dumps(result, indent=2))
        return 0 if result["inspectionPassed"] else 1
    except (InspectionError, LEGAL.MaterialError, OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps({"inspectionPassed": False, "materialBundleCollected": False,
                          "nativeLegalMaterialValidationPassed": False, "nativeReleaseReady": False, "errors": [str(error)]}, indent=2))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
