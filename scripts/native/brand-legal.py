#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Collect and verify real native dependency legal/relink materials, never templates."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / ".snow/brand-tools"))
from jsonschema import Draft202012Validator
from license_expression import get_spdx_licensing
from spdx_tools.spdx.parser.parse_anything import parse_file
from spdx_tools.spdx.validation.document_validator import validate_full_spdx_document

SCHEMA_VERSION = 1
SHA256 = re.compile(r"^[0-9a-f]{64}$")
IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
LINKAGES = {"static", "dynamic", "embedded", "source-only", "system", "build-only"}
SPDX_WORD = re.compile(r"(?:LicenseRef-[A-Za-z0-9.+-]+|[A-Za-z0-9][A-Za-z0-9.+-]*|\(|\))")
SOURCE_SUFFIXES = {".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".py", ".svg", ".qml", ".cmake", ".rs", ".ttf", ".otf", ".woff2"}
TEMPLATE_MARKERS = re.compile(r"(?im)^\s*(?:TODO\s*[:=-]|PLACEHOLDER\b|TEMPLATE ONLY\b|NOT A REAL LICENSE\b|REPLACE THIS\b)")
LICENSE_MARKERS = {
    "AGPL-3.0-only": ["GNU AFFERO GENERAL PUBLIC LICENSE", "Version 3, 19 November 2007"],
    "LGPL-3.0-only": ["GNU LESSER GENERAL PUBLIC LICENSE", "Version 3, 29 June 2007"],
    "GPL-3.0-only": ["GNU GENERAL PUBLIC LICENSE", "Version 3, 29 June 2007"],
    "LGPL-2.1-only": ["GNU LESSER GENERAL PUBLIC LICENSE", "Version 2.1"],
    "GPL-2.0-only": ["GNU GENERAL PUBLIC LICENSE", "Version 2, June 1991"],
    "Apache-2.0": ["Apache License", "Version 2.0"],
    "MIT": ["Permission is hereby granted, free of charge", "THE SOFTWARE IS PROVIDED"],
    "BSD-2-Clause": ["Redistribution and use in source and binary forms", "THIS SOFTWARE IS PROVIDED"],
    "BSD-3-Clause": ["Redistribution and use in source and binary forms", "THIS SOFTWARE IS PROVIDED"],
    "curl": ["Permission to use, copy, modify, and distribute", "THE SOFTWARE IS PROVIDED"],
    "OFL-1.1": ["SIL OPEN FONT LICENSE", "Version 1.1"],
    "CC-BY-SA-4.0": ["Creative Commons Attribution-ShareAlike 4.0", "Public License"],
    "Zlib": ["as-is", "altered source versions"],
    "BSL-1.0": ["Boost Software License", "Version 1.0"],
    "Unicode-3.0": ["UNICODE", "Permission"],
    "FTL": ["FreeType", "LICENSE"],
    "OpenSSL": ["OpenSSL", "Redistribution and use"],
    "Brotli": ["Permission is hereby granted, free of charge", "THE SOFTWARE IS PROVIDED"],
}


class MaterialError(ValueError):
    pass


def readiness(scope: str, *, passed: bool) -> dict:
    """Legal-material validation never authorizes a complete native release."""
    return {"nativeLegalMaterialValidationPassed": passed and scope == "native-release",
            "dependencySnapshotValidationPassed": passed and scope == "native-dependency-snapshot",
            "nativeReleaseReady": False}


def hash_file(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def inside(root: Path, value: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise MaterialError("Material paths must be exact non-empty strings")
    raw = Path(value)
    if raw.is_absolute() or ".." in raw.parts:
        raise MaterialError(f"Material path must be checkout-relative and traversal-free: {value}")
    target = (root / raw).resolve()
    if not target.is_relative_to(root.resolve()):
        raise MaterialError(f"Material escaped the checkout: {value}")
    cursor = root
    for part in raw.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise MaterialError(f"Material symlinks are not followed: {value}")
    return target


def reference(root: Path, row: dict, *, nonempty: bool = True) -> Path:
    if not isinstance(row, dict) or not SHA256.fullmatch(str(row.get("sha256", ""))):
        raise MaterialError("Every material needs its independently recorded SHA-256")
    path = inside(root, row.get("path"))
    if not path.is_file():
        raise MaterialError(f"Missing real material: {row.get('path')}")
    if nonempty and path.stat().st_size == 0:
        raise MaterialError(f"Empty material: {row['path']}")
    if hash_file(path) != row["sha256"]:
        raise MaterialError(f"Material hash mismatch: {row['path']}")
    return path


def spdx_ids(expression: str) -> list[str]:
    if not isinstance(expression, str) or not expression.strip() or expression in {"NONE", "NOASSERTION"}:
        raise MaterialError("A concluded SPDX license expression is required")
    try:
        validation = get_spdx_licensing().validate(expression, strict=True)
    except Exception as error:
        # Some malformed expressions raise inside the upstream validator itself.
        raise MaterialError(f"Malformed SPDX expression {expression}: {error}") from error
    unknown = [key for key in validation.invalid_symbols if not str(key).startswith("LicenseRef-")]
    if validation.errors and (unknown or not validation.invalid_symbols):
        raise MaterialError(f"Malformed or unknown SPDX expression {expression}: {validation.errors}")
    tokens = SPDX_WORD.findall(expression)
    if "".join(tokens) != re.sub(r"\s+", "", expression):
        raise MaterialError(f"Malformed SPDX expression: {expression}")
    return [token for token in tokens if token not in {"AND", "OR", "WITH", "(", ")"}]


def verify_license_text(component: dict, paths: list[Path]):
    text = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in paths)
    if len(text.strip()) < 100 or TEMPLATE_MARKERS.search(text):
        raise MaterialError(f"{component['id']}: license placeholder/short text is not release material")
    for path in paths:
        if re.search(r"(?i)(?:template|\.example|placeholder)", path.name):
            raise MaterialError(f"{component['id']}: template license input is rejected")
    for token in spdx_ids(component["licenseConcluded"]):
        normalized = token.replace("-or-later", "-only")
        markers = LICENSE_MARKERS.get(normalized)
        if markers and not all(marker.lower() in text.lower() for marker in markers):
            raise MaterialError(f"{component['id']}: actual text for {token} is missing")
    if any(token.startswith("LGPL-3.0") for token in spdx_ids(component["licenseConcluded"])):
        if not all(marker.lower() in text.lower() for marker in LICENSE_MARKERS["GPL-3.0-only"]):
            raise MaterialError(f"{component['id']}: LGPLv3 incorporates GPLv3; both complete texts must be conveyed")


def verify_source_archive(path: Path):
    names = []
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            if archive.testzip() is not None:
                raise MaterialError(f"Corrupt corresponding-source ZIP: {path.name}")
            names = [info.filename for info in archive.infolist() if not info.is_dir()]
    elif tarfile.is_tarfile(path):
        with tarfile.open(path, "r:*") as archive:
            names = [member.name for member in archive.getmembers() if member.isfile()]
    else:
        raise MaterialError(f"Corresponding source must be a real ZIP/TAR archive: {path.name}")
    if not names:
        raise MaterialError("Corresponding-source archive is empty")
    for name in names:
        normalized = name.replace("\\", "/")
        parts = PurePosixPath(normalized)
        if parts.is_absolute() or ".." in parts.parts or re.match(r"^[A-Za-z]:", normalized):
            raise MaterialError(f"Source archive contains unsafe path: {name}")
    if not any(PurePosixPath(name).suffix.lower() in SOURCE_SUFFIXES or PurePosixPath(name).name == "CMakeLists.txt" for name in names):
        raise MaterialError("Source archive has no preferred-form code/font/vector sources")


def library_artifact(path: Path) -> dict:
    """An import archive is not a static implementation library."""
    members, objects, imports, machines = 0, 0, 0, set()
    with path.open("rb") as stream:
        if stream.read(8) != b"!<arch>\n":
            raise MaterialError(f"Not an actual object archive: {path.name}")
        length = path.stat().st_size
        while stream.tell() < length:
            header = stream.read(60)
            if len(header) != 60 or header[58:] != b"`\n":
                raise MaterialError(f"Malformed library member: {path.name}")
            try:
                size = int(header[48:58].decode("ascii").strip())
                name = header[:16].decode("ascii").strip()
            except (UnicodeError, ValueError) as error:
                raise MaterialError(f"Malformed library member header: {path.name}") from error
            if size < 0 or stream.tell() + size > length:
                raise MaterialError(f"Truncated library member: {path.name}")
            start, payload_size = stream.tell(), size
            if name.startswith("#1/"):
                try:
                    name_length = int(name[3:])
                except ValueError as error:
                    raise MaterialError(f"Malformed BSD archive name: {path.name}") from error
                if not 0 <= name_length <= size:
                    raise MaterialError(f"Truncated BSD archive name: {path.name}")
                stream.seek(name_length, 1)
                payload_size -= name_length
            sample = stream.read(min(payload_size, 56))
            if name not in {"/", "//", "/SYM64/"}:
                members += 1
                recognized = False
                if len(sample) >= 20 and sample[:4] == b"\0\0\xff\xff":
                    version, machine = struct.unpack_from("<HH", sample, 4)
                    if machine in {0x14C, 0x8664, 0xAA64}:
                        if version == 0 and 20 + struct.unpack_from("<I", sample, 12)[0] <= payload_size:
                            imports += 1
                            recognized = True
                        elif version == 1 and len(sample) >= 32:
                            objects += 1
                            recognized = True
                        elif version == 2 and len(sample) >= 56 and sample[12:28] == bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8"):
                            sections = struct.unpack_from("<I", sample, 44)[0]
                            if sections and 56 + 40 * sections <= payload_size:
                                objects += 1
                                recognized = True
                        if recognized:
                            machines.add(machine)
                elif len(sample) >= 20 and struct.unpack_from("<H", sample)[0] in {0x14C, 0x8664, 0xAA64}:
                    machines.add(struct.unpack_from("<H", sample)[0])
                    objects += 1
                    recognized = True
                elif sample.startswith((b"\x7fELF", b"BC\xc0\xde", b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe")):
                    objects += 1
                    recognized = True
                if not recognized:
                    raise MaterialError(f"Unrecognized implementation/import member: {path.name}")
            stream.seek(start + size)
            if size % 2 and stream.read(1) != b"\n":
                raise MaterialError(f"Malformed library padding: {path.name}")
    if not members or not (objects or imports):
        raise MaterialError(f"No actual implementation/import members: {path.name}")
    return {"kind": "import-library" if imports else "static-library", "format": "object archive",
            "members": members, "implementationOrDescriptorObjects": objects, "shortImportObjects": imports,
            "machines": sorted(machines)}


def binary_kind(path: Path) -> str:
    with path.open("rb") as stream:
        head = stream.read(4096)
    if head.startswith(b"!<arch>\n"):
        try:
            return library_artifact(path)["kind"]
        except MaterialError:
            return "other"
    if head.startswith(b"\x7fELF") and len(head) >= 64:
        return "object" if head[16:18] in [b"\x01\x00", b"\x00\x01"] else "executable"
    if head[:4] in [b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"] and len(head) >= 32:
        return "object" if head[12:16] in [b"\x01\x00\x00\x00", b"\x00\x00\x00\x01"] else "executable"
    if head.startswith(b"MZ") and len(head) >= 64:
        offset = struct.unpack_from("<I", head, 60)[0]
        if offset >= 64:
            with path.open("rb") as stream:
                stream.seek(offset)
                pe = stream.read(24)
            if len(pe) == 24 and pe[:4] == b"PE\x00\x00":
                return "shared-library" if struct.unpack_from("<H", pe, 22)[0] & 0x2000 else "executable"
    if len(head) >= 56 and head[:8] in [b"\0\0\xff\xff\x02\0\x64\x86", b"\0\0\xff\xff\x02\0\x4c\x01", b"\0\0\xff\xff\x02\0\x64\xaa"]:
        # MSVC /bigobj uses the anonymous header, not the ordinary COFF machine prefix.
        sections = struct.unpack_from("<I", head, 44)[0]
        if head[12:28] == bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8") and sections and 56 + 40 * sections <= path.stat().st_size:
            return "object"
    if len(head) >= 20 and head[:2] in [b"\x64\x86", b"\x4c\x01", b"\x64\xaa"]:
        return "object"
    if head.startswith(b"BC\xc0\xde"):
        return "object"
    return "other"


def component_materials(component: dict):
    for role in ["artifacts", "licenseFiles", "noticeFiles", "versionEvidence", "buildMaterials", "licenseRefTexts"]:
        for row in component.get(role, []):
            yield role, row
    for role in ["sourceInventory"]:
        if component.get(role):
            yield role, component[role]
    for row in component.get("correspondingSource", []):
        yield "correspondingSource", row


def validate_palladium_artwork(component: dict, root: Path):
    license_id = "CC-BY-SA-4.0"
    if (component.get("category") != "artwork" or component.get("licenseConcluded") != license_id
            or component.get("licenseDeclared") != license_id or component.get("sourceLicenseDeclared") != license_id
            or component.get("linkage") != "embedded" or component.get("redistributed") is not True):
        raise MaterialError("Palladium artwork must be an embedded CC-BY-SA-4.0 component")
    inventory_path = reference(root, component.get("sourceInventory"))
    if hash_file(inventory_path) != "685dc8657abbd73d0bb458dd90ea7507bf52a40f5c6129028c3bea78a911a066":
        raise MaterialError("Palladium artwork needs the exact recorded source inventory")
    inventory = json.loads(inventory_path.read_bytes())
    source_hashes = {row["path"]: row["sha256"] for row in inventory["files"]}
    artifacts = {row["path"]: row["sha256"] for row in component["artifacts"]}
    roles = component.get("artifactRoles", [])
    if len(roles) != len(artifacts) or {row["path"] for row in roles} != set(artifacts):
        raise MaterialError("Palladium artwork needs every actual artifact role and source binding")
    originals = {row["sha256"] for row in component.get("correspondingSource", []) if row.get("kind") == "original-raster-art"}
    if not {row["sha256"] for row in inventory["selectedMappings"]}.issubset(originals):
        raise MaterialError("Palladium artwork source must include the actual supplied raster bytes, not only an inventory")
    for row in roles:
        if row["sha256"] != artifacts[row["path"]] or row.get("license") != license_id:
            raise MaterialError("Palladium artifact role/hash differs from the shipped artwork")
        if row.get("derivation") == "byte-identical-user-copy":
            if source_hashes.get(row.get("sourcePath")) != row["sha256"] or not row["path"].endswith((".png", ".ico")):
                raise MaterialError("Palladium artwork must remain the original supplied PNG/ICO bytes")
        elif row.get("derivation") == "container-only-original-png-payloads" and row.get("format") == "ICNS":
            data = reference(root, row).read_bytes()
            cursor = 8
            if data[:4] != b"icns" or struct.unpack_from(">I", data, 4)[0] != len(data):
                raise MaterialError("Palladium ICNS container is malformed")
            for frame in row["frameSources"]:
                kind, length = struct.unpack_from(">4sI", data, cursor)
                payload = data[cursor + 8:cursor + length]
                if (length <= 8 or cursor + length > len(data) or kind.decode("ascii") != frame["type"]
                        or hashlib.sha256(payload).hexdigest() != source_hashes.get(frame["sourcePath"])):
                    raise MaterialError("Palladium ICNS must package only exact original PNG bytes")
                cursor += length
            if cursor != len(data):
                raise MaterialError("Palladium ICNS contains unrecorded frames")
        else:
            raise MaterialError("Palladium artwork entries must be original bytes or container-only assembly")


def validate_dependency_snapshot(document: dict):
    """Installed dependency materials are separate from application/relink evidence."""
    if "build" in document or "relink" in document:
        raise MaterialError("A dependency snapshot must not masquerade as a built/relinked application release")
    for component in document["components"]:
        identifier = component["id"]
        if component["category"] in {"application", "artwork", "font"} or component["linkage"] in {"system", "build-only"}:
            raise MaterialError(f"{identifier}: a dependency snapshot is not an application or asset bundle")
        if not any(row.get("kind") == "source-archive" for row in component.get("correspondingSource", [])):
            raise MaterialError(f"{identifier}: installed dependency snapshot needs its full corresponding-source archive")
        if not component.get("buildMaterials"):
            raise MaterialError(f"{identifier}: installed dependency snapshot needs actual build/configuration material")


def validate_input(document: dict, root: Path) -> list[str]:
    errors = []
    try:
        schema = json.loads((ROOT / "docs/native/licenses/bundle-input.schema.json").read_text(encoding="utf-8"))
        schema_errors = list(Draft202012Validator(schema).iter_errors(document))
        if schema_errors:
            raise MaterialError("Input schema: " + "; ".join(error.message for error in schema_errors[:4]))
        if document.get("schemaVersion") != SCHEMA_VERSION:
            raise MaterialError("Unsupported native legal input schema")
        if document.get("scope") not in {"asset-only", "native-dependency-snapshot", "native-release"}:
            raise MaterialError("scope must explicitly identify asset-only, native-dependency-snapshot or native-release")
        if document["scope"] == "native-dependency-snapshot":
            validate_dependency_snapshot(document)
        if not IDENTIFIER.fullmatch(str(document.get("releaseId", ""))):
            raise MaterialError("releaseId must be a concrete identifier")
        components = document.get("components", [])
        if not components:
            raise MaterialError("Empty dependencies are not a license bundle")
        ids, license_ref_hashes = set(), {}
        for component in components:
            identifier = component.get("id")
            if not IDENTIFIER.fullmatch(str(identifier or "")) or identifier in ids:
                raise MaterialError("Component identifiers must be concrete and unique")
            ids.add(identifier)
            for field in ["name", "version", "copyrightText", "origin"]:
                value = component.get(field)
                if not isinstance(value, str) or not value.strip() or TEMPLATE_MARKERS.search(value):
                    raise MaterialError(f"{identifier}: actual {field} evidence is required")
            if component.get("linkage") not in LINKAGES:
                raise MaterialError(f"{identifier}: unknown linkage")
            concluded = spdx_ids(component.get("licenseConcluded"))
            declared = spdx_ids(component.get("licenseDeclared", component.get("licenseConcluded")))
            if component.get("category") == "artwork":
                validate_palladium_artwork(component, root)
            required_refs = {token for token in concluded + declared if token.startswith("LicenseRef-")}
            extracted_refs = set()
            for row in component.get("licenseRefTexts", []):
                text_id = row.get("licenseId")
                if not isinstance(text_id, str) or not re.fullmatch(r"LicenseRef-[A-Za-z0-9.+-]+", text_id):
                    raise MaterialError(f"{identifier}: extracted LicenseRef needs its actual licenseId")
                text = reference(root, row).read_text(encoding="utf-8", errors="strict")
                if len(text.strip()) < 100 or TEMPLATE_MARKERS.search(text):
                    raise MaterialError(f"{identifier}: LicenseRef text is empty, short or a template")
                if text_id in license_ref_hashes and license_ref_hashes[text_id] != row["sha256"]:
                    raise MaterialError(f"Conflicting extracted text for {text_id}")
                license_ref_hashes[text_id] = row["sha256"]
                extracted_refs.add(text_id)
            if required_refs - extracted_refs:
                raise MaterialError(f"{identifier}: LicenseRef requires matching extracted text, not a made-up SPDX label")
            if component["linkage"] in {"system", "build-only"}:
                if component.get("redistributed", False):
                    raise MaterialError(f"{identifier}: redistributed files cannot be classified as a system/build-only exception")
            elif not component.get("artifacts"):
                raise MaterialError(f"{identifier}: no actual shipped artifact")
            if not component.get("licenseFiles"):
                raise MaterialError(f"{identifier}: actual license files are required")
            if component.get("noticePolicy") not in {"copyright-in-license", "upstream-notice-conveyed", "copyright-in-notice"}:
                raise MaterialError(f"{identifier}: explicit upstream notice disposition is required")
            if component["noticePolicy"] in {"upstream-notice-conveyed", "copyright-in-notice"} and not component.get("noticeFiles"):
                raise MaterialError(f"{identifier}: upstream NOTICE/copyright files were not supplied")
            license_paths = []
            for role, row in component_materials(component):
                path = reference(root, row)
                if role == "licenseFiles":
                    license_paths.append(path)
                if role == "correspondingSource" and row.get("kind") == "source-archive":
                    verify_source_archive(path)
                if role == "artifacts" and component["linkage"] == "static" and binary_kind(path) != "static-library":
                    raise MaterialError(f"{identifier}: static artifact is not a real archive: {row['path']}")
            verify_license_text(component, license_paths)
            if not component.get("versionEvidence"):
                raise MaterialError(f"{identifier}: actual version evidence is required")
            version_text = "\n".join(reference(root, row).read_text(encoding="utf-8", errors="replace") for row in component["versionEvidence"])
            if component["version"] not in version_text:
                raise MaterialError(f"{identifier}: version is not present in the supplied actual evidence")
            copyleft = any(token.startswith(("AGPL-", "GPL-", "LGPL-", "MPL-", "EPL-")) for token in concluded)
            if component["linkage"] not in {"system", "build-only"} and (copyleft or component["linkage"] == "static"):
                source_rows = component.get("correspondingSource", [])
                if not source_rows:
                    raise MaterialError(f"{identifier}: link alone is not corresponding source")
                if not any(row.get("kind") in {"source-archive", "source-file", "font-original"} for row in source_rows):
                    raise MaterialError(f"{identifier}: a README is not corresponding source")
                if component["linkage"] == "static" and not any(row.get("kind") == "source-archive" for row in source_rows):
                    raise MaterialError(f"{identifier}: static dependency needs its full corresponding-source archive")
            if component["linkage"] == "static" and not component.get("buildMaterials"):
                raise MaterialError(f"{identifier}: static dependency build/patch configuration is missing")
            if component.get("category") == "qt" and component["linkage"] not in {"system", "build-only"}:
                if not component.get("qtModules") or not component.get("qtThirdPartyCoverage"):
                    raise MaterialError(f"{identifier}: list actual Qt modules and bundled third-party SBOM coverage")
                for row in component["qtThirdPartyCoverage"]:
                    reference(root, row)
        if document["scope"] == "native-release":
            validate_release_build(document, root, ids)
    except (MaterialError, OSError, ValueError, KeyError, TypeError, struct.error, UnicodeError) as error:
        errors.append(str(error))
    return errors


def validate_release_build(document: dict, root: Path, ids: set[str]):
    build = document.get("build", {})
    for field in ["platform", "architecture", "compiler", "compilerVersion"]:
        if not str(build.get(field, "")).strip():
            raise MaterialError(f"Native release missing actual build.{field}")
    executable = reference(root, build.get("executable", {}))
    if binary_kind(executable) != "executable":
        raise MaterialError("Native release executable is not a real PE/ELF/Mach-O binary")
    evidence = build.get("evidence", [])
    roles = {row.get("kind") for row in evidence}
    if not {"link-command", "link-map", "dependency-inventory", "build-log"}.issubset(roles):
        raise MaterialError("Native release needs actual link command, link map, dependency inventory and build log")
    link_text = ""
    inventory = None
    for row in evidence:
        path = reference(root, row)
        if row["kind"] in {"link-command", "link-map"}:
            link_text += path.read_text(encoding="utf-8", errors="replace") + "\n"
        if row["kind"] == "dependency-inventory":
            inventory = json.loads(path.read_text(encoding="utf-8"))
    linked = inventory.get("components", []) if inventory else []
    if not linked:
        raise MaterialError("Actual linked dependency inventory is empty")
    linked_ids = set()
    component_by_id = {component["id"]: component for component in document["components"]}
    relocation = document.get("_bundlePathMap", {})
    for entry in linked:
        if entry.get("id") not in ids:
            raise MaterialError(f"Uncovered linked dependency: {entry.get('id')}")
        if entry["id"] in linked_ids:
            raise MaterialError(f"Duplicate linked dependency: {entry['id']}")
        component = component_by_id[entry["id"]]
        if entry.get("version") != component["version"]:
            raise MaterialError(f"Linked dependency version drift: {entry['id']}")
        linked_ids.add(entry["id"])
        rows = entry.get("artifacts", [])
        if component["linkage"] in {"static", "dynamic"} and not rows:
            raise MaterialError(f"Linked dependency lacks actual artifact hashes: {entry['id']}")
        actual_hashes = set()
        for row in rows:
            relocated_row = dict(row)
            relocated_row["path"] = relocation.get(row["path"], row["path"])
            path = reference(root, relocated_row)
            if row["sha256"] not in {artifact["sha256"] for artifact in component["artifacts"]}:
                raise MaterialError(f"Linked artifact missing from legal materials: {entry['id']}")
            actual_hashes.add(row["sha256"])
            if component["linkage"] == "static" and Path(row["path"]).name.lower() not in link_text.lower():
                raise MaterialError(f"Static artifact absent from actual link evidence: {path.name}")
        if component["linkage"] == "static" and actual_hashes != {row["sha256"] for row in component["artifacts"]}:
            raise MaterialError(f"Static artifact inventory is incomplete: {entry['id']}")
    for component in document["components"]:
        if component["linkage"] == "static" and component["id"] not in linked_ids:
            raise MaterialError(f"Static component absent from actual dependency inventory: {component['id']}")
    applications = [component for component in document["components"] if component.get("category") == "application"]
    if len(applications) != 1 or "AGPL-3.0" not in applications[0]["licenseConcluded"]:
        raise MaterialError("Exactly one native AGPL application component is required")
    application = applications[0]
    if not any(row.get("kind") == "source-archive" for row in application.get("correspondingSource", [])):
        raise MaterialError("Native AGPL full corresponding application source archive is missing")
    if build["executable"]["sha256"] not in {row["sha256"] for row in application["artifacts"]}:
        raise MaterialError("Application legal artifact does not match the actual built executable")
    static_lgpl = [component for component in document["components"] if component["linkage"] == "static" and "LGPL-" in component["licenseConcluded"]]
    if static_lgpl:
        validate_relink(document.get("relink", {}), root, static_lgpl, executable)


def command_path(value: str, working_directory: str = "") -> str:
    if not isinstance(value, str) or not value.strip() or any(char in value for char in "\x00\r\n"):
        raise MaterialError("Relink command paths must be concrete single-line strings")
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        value = value[1:-1]
    value = value.replace("\\", "/")
    windows = bool(re.match(r"^[A-Za-z]:/", value)) or value.startswith("//")
    if working_directory and not value.startswith("/") and not windows:
        value = working_directory.replace("\\", "/").rstrip("/") + "/" + value
        windows = bool(re.match(r"^[A-Za-z]:/", value)) or value.startswith("//")
    # Command paths are provenance only, never followed outside the bundle.
    import posixpath
    value = posixpath.normpath(value)
    return value.casefold() if windows else value


def probe_link_inputs(probe: dict, log: dict, root: Path) -> set[str]:
    import shlex

    argv = log.get("argv")
    if not isinstance(argv, list) or not argv or any(not isinstance(arg, str) or not arg.strip() for arg in argv):
        raise MaterialError("Relink probe argv must be a nonempty array of concrete strings")
    working_directory = log.get("workingDirectory", "")
    if not isinstance(working_directory, str):
        raise MaterialError("Relink workingDirectory must be a string")
    responses = {}
    for row in probe.get("responseFiles", []):
        key = command_path(row.get("commandPath"), working_directory)
        if key in responses:
            raise MaterialError("Duplicate relink response command path")
        responses[key] = reference(root, row)
    consumed = set()
    inputs = set()
    pending = list(argv[1:])
    while pending:
        arg = pending.pop(0).strip()
        if len(arg) >= 2 and arg[0] == arg[-1] and arg[0] in {"'", '"'}:
            arg = arg[1:-1]
        if arg.startswith("-Wl,"):
            pending.extend(arg[4:].split(","))
            continue
        if arg.startswith("@"):
            key = command_path(arg[1:], working_directory)
            if key not in responses:
                raise MaterialError("Actual relink response file was not supplied as hashed material")
            if key in consumed:
                raise MaterialError("Repeated or cyclic relink response-file input")
            consumed.add(key)
            data = responses[key].read_bytes()
            encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
            try:
                pending.extend(shlex.split(data.decode(encoding), posix=False))
            except (UnicodeError, ValueError) as error:
                raise MaterialError(f"Invalid relink response file: {error}") from error
            continue
        if arg.lower().startswith("/wholearchive:"):
            arg = arg[len("/wholearchive:"):]
        if arg:
            inputs.add(command_path(arg, working_directory))
    if set(responses) != consumed:
        raise MaterialError("Recorded relink response file was not consumed by the actual link command")
    return inputs


def validate_relink(relink: dict, root: Path, libraries: list[dict], original_executable: Path):
    for field in ["applicationCode", "linkCommands", "buildInstructions", "installationInformation", "resources"]:
        rows = relink.get(field, [])
        if not rows:
            raise MaterialError(f"Static LGPL requires actual relink.{field}; AGPL does not waive it")
        for row in rows:
            path = reference(root, row)
            if field == "applicationCode" and row.get("kind") in {"object", "static-library"}:
                if binary_kind(path) != row["kind"]:
                    raise MaterialError(f"Relink object/archive is not genuine: {row['path']}")
            if field == "applicationCode" and row.get("kind") == "source-archive":
                verify_source_archive(path)
    if not any(row.get("kind") in {"object", "source-archive", "static-library"} for row in relink["applicationCode"]):
        raise MaterialError("Corresponding Application Code must be real object/source files, not a README promise")
    probe = relink.get("probe", {})
    result_binary = reference(root, probe.get("relinkedExecutable", {}))
    if binary_kind(result_binary) != "executable" or hash_file(result_binary) == hash_file(original_executable):
        raise MaterialError("Relink probe must produce a real, changed executable")
    log_path = reference(root, probe.get("log", {}))
    log = json.loads(log_path.read_text(encoding="utf-8"))
    if log.get("exitCode") != 0 or log.get("outputSha256") != hash_file(result_binary):
        raise MaterialError("Relink probe log must record successful argv and the actual output SHA-256")
    inputs = probe_link_inputs(probe, log, root)
    replacements = probe.get("replacementLibraries", [])
    required = {component["id"] for component in libraries}
    replaced = set()
    by_id = {component["id"]: component for component in libraries}
    for row in replacements:
        path = reference(root, row)
        identifier = row.get("componentId")
        if identifier not in required or binary_kind(path) != "static-library":
            raise MaterialError("Relink replacement must be a real static LGPL linked-library archive")
        if row["sha256"] in {artifact["sha256"] for artifact in by_id[identifier]["artifacts"]}:
            raise MaterialError("Unmodified original library is not proof of relinking against a modified library")
        if command_path(row.get("commandPath"), log.get("workingDirectory", "")) not in inputs:
            raise MaterialError("Actual replacement library was not consumed by the relink command")
        replaced.add(identifier)
    if replaced != required:
        raise MaterialError("Relink probe must cover every static LGPL linked component")


def all_refs(document: dict):
    for component in document["components"]:
        for role, row in component_materials(component):
            yield component["id"], role, row
        for row in component.get("qtThirdPartyCoverage", []):
            yield component["id"], "qtThirdPartyCoverage", row
    if document.get("build"):
        yield "build", "executable", document["build"]["executable"]
        for row in document["build"].get("evidence", []):
            yield "build", row["kind"], row
    relink = document.get("relink", {})
    for role in ["applicationCode", "linkCommands", "buildInstructions", "installationInformation", "resources"]:
        for row in relink.get(role, []):
            yield "relink", role, row
    if relink.get("probe"):
        probe = relink["probe"]
        for role in ["relinkedExecutable", "log"]:
            yield "relink", role, probe[role]
        for role in ["replacementLibraries", "responseFiles"]:
            for row in probe.get(role, []):
                yield "relink", role, row


def emit_spdx(document: dict, material_rows: list[dict], input_sha256: str, root: Path) -> dict:
    packages, relationships, extracted = [], [], {}
    for component in document["components"]:
        ident = "SPDXRef-Package-" + component["id"]
        packages.append({"SPDXID": ident, "name": component["name"], "versionInfo": component["version"],
                         "downloadLocation": component["origin"], "filesAnalyzed": False,
                         "licenseConcluded": component["licenseConcluded"], "licenseDeclared": component.get("licenseDeclared", component["licenseConcluded"]),
                         "copyrightText": component["copyrightText"], "sourceInfo": component.get("sourceDescription", "Real material hashes are in materials-manifest.json"),
                         "checksums": [{"algorithm": "SHA256", "checksumValue": row["sha256"]} for row in component.get("artifacts", [])]})
        relationships.append({"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": ident})
        for row in component.get("licenseRefTexts", []):
            text_id = row["licenseId"]
            text = reference(root, row).read_text(encoding="utf-8", errors="strict")
            if text_id in extracted and extracted[text_id]["extractedText"] != text:
                raise MaterialError(f"Conflicting extracted text for {text_id}")
            extracted[text_id] = {"licenseId": text_id, "extractedText": text}
    application = next((component for component in document["components"] if component.get("category") == "application"), None)
    if application:
        for component in document["components"]:
            if component is application:
                continue
            relation = "STATIC_LINK" if component["linkage"] == "static" else "DYNAMIC_LINK" if component["linkage"] == "dynamic" else "CONTAINS"
            relationships.append({"spdxElementId": "SPDXRef-Package-" + application["id"], "relationshipType": relation,
                                  "relatedSpdxElement": "SPDXRef-Package-" + component["id"]})
    return {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
            "name": document["releaseId"] + "-actual-legal-materials", "documentNamespace": "https://spdx.org/spdxdocs/" + document["releaseId"] + "-" + input_sha256,
            "creationInfo": {"creators": ["Tool: Nickel brand-legal.py"], "created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")},
            "documentComment": f"Actual copied and hash-verified materials, scope={document['scope']}; legal validation is not native release authorization.",
            "hasExtractedLicensingInfos": [extracted[key] for key in sorted(extracted)],
            "packages": packages, "relationships": relationships}


def collect(input_path: Path, output: Path, root: Path) -> dict:
    raw = input_path.read_bytes()
    document = json.loads(raw)
    errors = validate_input(document, root)
    if errors:
        raise MaterialError("; ".join(errors))
    if output.exists() and any(output.iterdir()):
        raise MaterialError("Refusing to overwrite a non-empty bundle; use a fresh output path")
    content_scoped = document["scope"] in {"native-dependency-snapshot", "native-release"}
    planned, unique, content_names = [], {}, {}
    for identifier, role, row in all_refs(document):
        source = reference(root, row)
        if not IDENTIFIER.fullmatch(role):
            raise MaterialError(f"Invalid material role: {role}")
        if content_scoped:
            target_name = content_names.setdefault(row["sha256"], f"materials/content/{row['sha256']}/{source.name}")
        else:
            target_name = f"materials/{identifier}/{role}/{row['sha256'][:12]}-{source.name}"
        planned.append((identifier, role, row, source, target_name))
        unique[target_name] = source.stat().st_size
    parent = output.parent
    while not parent.exists():
        parent = parent.parent
    required_bytes = sum(unique.values()) + len(raw) + 16 * 1024 * 1024
    if shutil.disk_usage(parent).free < required_bytes:
        raise MaterialError("Insufficient disk space before collecting legal materials; no bundle files were written")
    output.mkdir(parents=True, exist_ok=True)
    (output / "input-materials.json").write_bytes(raw)
    materials = []
    seen = {}
    for identifier, role, row, source, target_name in planned:
        target = output / target_name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target_name not in seen:
            shutil.copyfile(source, target)
            if hash_file(target) != row["sha256"]:
                raise MaterialError(f"Material changed while copying: {row['path']}")
            seen[target_name] = row["sha256"]
        materials.append({"component": identifier, "role": role, "originalPath": row["path"], "path": target_name,
                          "sha256": row["sha256"], "bytes": target.stat().st_size, "kind": row.get("kind", "file")})
    notices = []
    for component in document["components"]:
        notices.append(f"{component['name']} {component['version']}\nSPDX: {component['licenseConcluded']}\n{component['copyrightText']}\nOrigin: {component['origin']}\nLinkage: {component['linkage']}\n")
        for role in ["licenseFiles", "noticeFiles"]:
            for row in component.get(role, []):
                text = reference(root, row).read_text(encoding="utf-8", errors="replace")
                notices.append(f"--- verbatim {role}: {row['path']} (SHA256 {row['sha256']}) ---\n{text}\n")
    notices.append("Native static Qt/other LGPL is not exempted by the application AGPL. Source and relink material entries must remain with the matching release.\n")
    (output / "THIRD-PARTY-NOTICES.txt").write_bytes("\n".join(notices).encode("utf-8"))
    spdx = emit_spdx(document, materials, hashlib.sha256(raw).hexdigest(), root)
    (output / "dependencies.spdx.json").write_bytes((json.dumps(spdx, indent=2) + "\n").encode("utf-8"))
    standard_errors = validate_spdx(output / "dependencies.spdx.json")
    if standard_errors:
        raise MaterialError("SPDX-2.3 validation failed: " + "; ".join(standard_errors))
    manifests = {"schemaVersion": 1, "scope": document["scope"], "releaseId": document["releaseId"],
                 "inputSha256": hashlib.sha256(raw).hexdigest(), "materialValidationPassed": True,
                 **readiness(document["scope"], passed=True), "spdxValidationPassed": True, "materials": materials,
                 "generated": {name: {"sha256": hash_file(output / name), "bytes": (output / name).stat().st_size}
                               for name in ["input-materials.json", "THIRD-PARTY-NOTICES.txt", "dependencies.spdx.json"]}}
    (output / "materials-manifest.json").write_bytes((json.dumps(manifests, indent=2) + "\n").encode("utf-8"))
    return manifests


def validate_spdx(path: Path) -> list[str]:
    try:
        return [str(message) for message in validate_full_spdx_document(parse_file(str(path)))]
    except Exception as error:
        return [str(error)]


def verify_bundle(output: Path) -> dict:
    manifest = json.loads((output / "materials-manifest.json").read_text(encoding="utf-8"))
    errors = []
    for row in manifest.get("materials", []):
        try:
            path = reference(output, row)
            if path.stat().st_size != row["bytes"]:
                raise MaterialError(f"Bundle length drift: {row['path']}")
        except (OSError, MaterialError) as error:
            errors.append(str(error))
    for name, row in manifest.get("generated", {}).items():
        try:
            reference(output, {"path": name, "sha256": row["sha256"]})
        except (OSError, MaterialError) as error:
            errors.append(str(error))
    if not manifest.get("materials"):
        errors.append("Empty material bundle")
    document = json.loads((output / "input-materials.json").read_text(encoding="utf-8"))
    lookup = {row["originalPath"]: row["path"] for row in manifest["materials"]}
    relocated = json.loads(json.dumps(document))
    for _, _, row in all_refs(relocated):
        if row["path"] not in lookup:
            errors.append(f"Input material omitted from bundle: {row['path']}")
        else:
            row["path"] = lookup[row["path"]]
    # Artifact role rows bind copied material, while sourcePath remains immutable origin metadata.
    for component in relocated["components"]:
        for row in component.get("artifactRoles", []):
            if row["path"] not in lookup:
                errors.append(f"Artifact source binding omitted from bundle: {row['path']}")
            else:
                row["path"] = lookup[row["path"]]
    relocated["_bundlePathMap"] = lookup
    errors += validate_input(relocated, output)
    errors += validate_spdx(output / "dependencies.spdx.json")
    if manifest.get("inputSha256") != hash_file(output / "input-materials.json"):
        errors.append("Input manifest hash drift")
    if document.get("scope") != manifest.get("scope"):
        errors.append("Scope drift")
    return {"scope": manifest.get("scope"), "releaseId": manifest.get("releaseId"), "materialCount": len(manifest["materials"]),
            "materialValidationPassed": not errors, **readiness(document["scope"], passed=not errors), "errors": errors}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    check = subs.add_parser("check-input", help="Validate actual material references without making a package")
    check.add_argument("--input", required=True)
    pack = subs.add_parser("collect", help="Copy exact files, notices, sources and relink materials to a fresh bundle")
    pack.add_argument("--input", required=True)
    pack.add_argument("--output", required=True)
    check_bundle = subs.add_parser("verify", help="Hash/contract-check the staged bundle independent of original paths")
    check_bundle.add_argument("--bundle", required=True)
    args = parser.parse_args()
    try:
        if args.command == "check-input":
            input_path = inside(ROOT, args.input)
            document = json.loads(input_path.read_text(encoding="utf-8"))
            errors = validate_input(document, ROOT)
            result = {"scope": document.get("scope"), "materialValidationPassed": not errors,
                      **readiness(document.get("scope"), passed=not errors), "errors": errors}
        elif args.command == "collect":
            result = collect(inside(ROOT, args.input), inside(ROOT, args.output), ROOT)
        else:
            result = verify_bundle(inside(ROOT, args.bundle))
        print(json.dumps(result, indent=2))
        return 0 if result.get("materialValidationPassed") else 1
    except (MaterialError, OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps({"materialValidationPassed": False, "nativeReleaseReady": False, "errors": [str(error)]}, indent=2))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
