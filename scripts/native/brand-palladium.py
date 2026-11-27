#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Import supplied Palladium raster bytes; never redraw or infer an art license."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import sys
from fontTools.ttLib import TTFont
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
BRAND = ROOT / "native/assets/brand"
INTAKE = "source/palladium-intake.json"
INTAKE_SHA256 = "685dc8657abbd73d0bb458dd90ea7507bf52a40f5c6129028c3bea78a911a066"
PROJECT_ART_LICENSE = "CC-BY-SA-4.0"
REVISION = "palladium-user-art-2026-11-21"
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
FONT_SOURCE = "src/assets/fonts/nickel-line.woff2"
FUNCTIONAL_BASELINE = "tests/brand/functional-icons.sha256"
# Files the package ships that belong to this project (fonts, licence text, functional list)
# rather than to the supplied artwork. They are read from the checkout, never from a foreign tree.
PROJECT_FILES = (
    ("fonts/nickel-line.woff2", "text-font-original", "OFL-1.1", "WOFF2", False),
    ("fonts/nickel-text-subset.ttf", "qt-text-font", "OFL-1.1", "TTF", True),
    ("fonts/OFL-1.1.txt", "font-license", "OFL-1.1", "text", True),
    ("fonts/NOTICE.md", "font-notice", "AGPL-3.0-only", "markdown", True),
    ("functional-resources.qrc", "immutable-resource-references", "AGPL-3.0-only", "QRC", False),
)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def json_bytes(value: dict) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def relative(name: str) -> str:
    if not isinstance(name, str) or not name or name != name.strip():
        raise ValueError("An exact non-empty relative path is required")
    path = PurePosixPath(name)
    if path.is_absolute() or path.as_posix() != name or any(c in name for c in "\\:\x00\r\n") or ".." in path.parts:
        raise ValueError("Unsafe material path: " + name)
    return name


def confined(root: Path, name: str) -> Path:
    path = root
    for part in PurePosixPath(relative(name)).parts:
        path = path / part
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            raise ValueError("Nested material links are not followed: " + name)
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("Material escaped its root: " + name)
    return path


def read(root: Path, name: str, expected: dict | None = None) -> bytes:
    path = confined(root, name)
    before = path.stat()
    data = path.read_bytes()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Material changed while reading: " + name)
    if expected and (digest(data) != expected["sha256"] or len(data) != expected["bytes"]):
        raise ValueError("Material differs from the frozen source: " + name)
    return data


def load_intake(data: bytes) -> dict:
    if digest(data) != INTAKE_SHA256:
        raise ValueError("Use the exact independently inventoried user artwork, not an updated or fabricated intake")
    intake = json.loads(data)
    if intake.get("kind") != "palladium-user-source-inventory-read-only" or intake.get("fileCount") != 108:
        raise ValueError("Not the reviewed Palladium source inventory")
    files = intake["files"]
    if digest(json.dumps(files, sort_keys=True, ensure_ascii=True, separators=(",", ":")).encode("utf-8")) != intake["fileSetSha256"]:
        raise ValueError("Source file-set fingerprint differs")
    paths = [relative(row["path"]) for row in files]
    targets = [relative(row["targetPath"]) for row in intake["selectedMappings"]]
    if len(paths) != 108 or len(paths) != len(set(paths)) or targets != sorted(targets) or len(targets) != 54 or len(targets) != len(set(targets)):
        raise ValueError("Source inventory contains missing, duplicate or unsorted mappings")
    if any(row["sourcePath"].startswith("blocks/") or row["license"] != "NOASSERTION" for row in intake["selectedMappings"]):
        raise ValueError("Functional artwork or unverified art licensing entered the brand mapping")
    return intake


def font_profile() -> dict:
    """Describe the project text font that the brand package reuses, without any foreign source."""
    original = read(ROOT, FONT_SOURCE)
    with TTFont(ROOT / FONT_SOURCE, recalcTimestamp=False) as font:
        cmap = font.getBestCmap()
    private_use = sum(1 for code in cmap if 0xE000 <= code <= 0xF8FF)
    return {
        "originalSha256": digest(original),
        "unicodeCharacters": len(cmap),
        "privateUseCharacters": private_use,
        "iconFont": False,
        "completeCjkCoverage": False,
        "brandWordmarksNeedFont": False,
    }


def functional_baseline() -> dict:
    """Record the checked-in functional icon baseline the package must not drift from."""
    raw = read(ROOT, FUNCTIONAL_BASELINE)
    # Hash the LF form so the recorded baseline does not depend on the checkout's line endings.
    normalized = raw.replace(b"\r\n", b"\n")
    entries = [line for line in normalized.decode("utf-8").splitlines() if line.strip()]
    return {"path": FUNCTIONAL_BASELINE, "entries": len(entries), "sha256": digest(normalized), "modified": False}


def notice() -> bytes:
    return (
        "Palladium / Pd native brand and artwork notice\n"
        "Artwork: Palladium brand and art assets, Nickel Studio (ItzArona, ItzPlana), 2026.\n"
        "License: Creative Commons Attribution-ShareAlike 4.0 International (CC-BY-SA-4.0).\n"
        "Full text: the LICENSE-ASSETS file at the repository root, also copied as LICENSES/CC-BY-SA-4.0.txt.\n"
        "Source: the Palladium art package recorded in source/palladium-intake.json; PNG and ICO files are copied byte-for-byte.\n"
        "Existing Noto font software remains under OFL-1.1; see fonts/OFL-1.1.txt and fonts/NOTICE.md.\n"
        "Code and project documentation: AGPL-3.0-only, see the root LICENSE.\n"
        "Game, loader, skin and third-party artwork referenced by the functional resource list keep their own rights; the /nickel/ resource namespaces stay unchanged for compatibility.\n"
    ).encode("utf-8")


PROJECT_ONLY_EXCLUDED_FUNCTIONAL_ALIASES = ("credits/pcl.png", "credits/pcl-ce.png")


def pruned_functional_qrc(data: bytes) -> bytes:
    """Keep the functional rows Palladium ships, minus third-party credits it no longer shows.

    The functional resource list is carried over from the historical package because the icons
    and their aliases are the compatibility baseline. Only the credit line entries for projects
    that are no longer acknowledged are dropped; every other row keeps its original bytes.
    """
    root = ET.fromstring(data)
    removed = 0
    for resource in root.findall("qresource"):
        for row in list(resource.findall("file")):
            if row.attrib.get("alias") in PROJECT_ONLY_EXCLUDED_FUNCTIONAL_ALIASES:
                resource.remove(row)
                removed += 1
    if not removed:
        return data
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n"


def prepare(intake_bytes: bytes, supplied: dict[str, bytes]) -> tuple[dict[str, bytes], dict]:
    intake = load_intake(intake_bytes)
    payloads: dict[str, bytes] = {}
    assets = []

    def put(name: str, data: bytes, role: str, license_id: str, fmt: str, embedded: bool = False, **extra):
        relative(name)
        if name in payloads:
            raise ValueError("Duplicate planned output: " + name)
        payloads[name] = data
        assets.append({"path": name, "sha256": digest(data), "bytes": len(data), "role": role,
                       "license": license_id, "format": fmt, "qtEmbedded": embedded, **extra})

    art_scope = {"sourceLicenseDeclared": PROJECT_ART_LICENSE,
                 "sourceCopyright": "Nickel Studio (ItzArona, ItzPlana)"}
    for row in intake["selectedMappings"]:
        data = supplied[row["sourcePath"]]
        if digest(data) != row["sha256"] or len(data) != row["bytes"]:
            raise ValueError("Selected source bytes changed: " + row["sourcePath"])
        metadata = {key: row[key] for key in ("width", "height", "frameSizes", "rgbaSha256", "alphaBounds") if key in row}
        put(row["targetPath"], data, row["role"], PROJECT_ART_LICENSE, row["format"], row["qtEmbedded"],
            sourcePath=row["sourcePath"], sourceSha256=row["sha256"], derivation="byte-identical-user-copy", **art_scope, **metadata)
    for theme in ("", "-light"):
        name = "logo/Palladium" + theme + ".png"
        put("platform/icon" + theme + "-1024.png", supplied[name], "platform-icon", PROJECT_ART_LICENSE, "PNG",
            width=1024, height=1024, sourcePath=name, sourceSha256=digest(supplied[name]), derivation="byte-identical-user-copy", **art_scope)
    for size in (16, 32, 48):
        name = "logo/sizes/Palladium-" + str(size) + ".png"
        put("platform/favicon-" + str(size) + ".png", supplied[name], "website-favicon", PROJECT_ART_LICENSE, "PNG",
            width=size, height=size, sourcePath=name, sourceSha256=digest(supplied[name]), derivation="byte-identical-user-copy", **art_scope)

    # ICNS only packages original PNG payloads; no missing 512px image is invented.
    frames = [(b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128), (b"ic08", 256),
              (b"ic10", 1024), (b"ic11", 32), (b"ic12", 64), (b"ic13", 256)]
    chunks, frame_sources = [], []
    for code, size in frames:
        name = "logo/Palladium.png" if size == 1024 else "logo/sizes/Palladium-" + str(size) + ".png"
        data = supplied[name]
        chunks.append(code + struct.pack(">I", len(data) + 8) + data)
        frame_sources.append({"type": code.decode("ascii"), "size": size, "sourcePath": name, "sha256": digest(data)})
    payload = b"".join(chunks)
    put("platform/app.icns", b"icns" + struct.pack(">I", len(payload) + 8) + payload, "macos-icon", PROJECT_ART_LICENSE, "ICNS",
        derivation="container-only-original-png-payloads", frameSizes=[16, 32, 64, 128, 256, 1024],
        frameSources=frame_sources, missingSuppliedFrameSizes=[512], **art_scope)

    for name, role, license_id, fmt, embedded in PROJECT_FILES:
        data = read(BRAND, name)
        if name == "functional-resources.qrc":
            data = pruned_functional_qrc(data)
        put(name, data, role, license_id, fmt, embedded)
    put("NOTICE.txt", notice(), "brand-attribution", PROJECT_ART_LICENSE, "text", True)
    put(INTAKE, intake_bytes, "source-provenance", "AGPL-3.0-only", "JSON")
    put("source/Palladium-README.md", supplied["README.md"], "supplied-source-documentation", "NOASSERTION", "markdown", sourcePath="README.md")
    put("source/Palladium-verification.json", supplied["verification.json"], "supplied-source-verification-not-build", "NOASSERTION", "JSON", sourcePath="verification.json")

    runtime = sorted(row["path"] for row in assets if row["qtEmbedded"])
    qrc = ET.Element("RCC")
    resource = ET.SubElement(qrc, "qresource", {"prefix": "/nickel/brand"})
    for name in runtime:
        ET.SubElement(resource, "file", {"alias": name}).text = name
    ET.indent(qrc, space="  ")
    put("brand.qrc", ET.tostring(qrc, encoding="utf-8", xml_declaration=True) + b"\n", "qt-resource-list", "AGPL-3.0-only", "QRC")

    manifest = {
        "schemaVersion": 1, "revision": REVISION, "product": "Palladium", "shortName": "Pd", "chineseName": "\u94af",
        "attribution": "Nickel Studio (ItzArona, ItzPlana)", "sourceCopyright": "Nickel Studio (ItzArona, ItzPlana)",
        "sourceLicenseDeclared": PROJECT_ART_LICENSE,
        "preferredSource": INTAKE, "sourceSha256": INTAKE_SHA256, "sourceKind": "supplied-raster-package-inventory",
        "vectorAuthoringSourceProvided": False, "generator": "scripts/native/brand-palladium.py",
        "qtResourcePrefix": ":/nickel/brand/", "qtResourceFiles": runtime,
        "functionalResourcePrefix": ":/nickel/functional/", "renderer": {"engine": "supplied-png-copy-no-redraw"},
        "atomicNumber": 46, "atomicNumberIsSoftwareVersion": False,
        "artLicense": PROJECT_ART_LICENSE, "artLicenseConfirmed": True, "codeLicense": "AGPL-3.0-only", "fontLicense": "OFL-1.1",
        "font": font_profile(), "functionalBaseline": functional_baseline(),
        "sourceFileSetSha256": intake["fileSetSha256"], "suppliedArtworkCount": 54,
        "runtimePngCompressedBytes": sum(row["bytes"] for row in assets if row["qtEmbedded"] and row["format"] == "PNG"),
        "runtimePngDecodedBytes": sum(row["width"] * row["height"] * 4 for row in assets if row["qtEmbedded"] and row["format"] == "PNG"),
        "runtimeLoadPolicy": "Load only the selected alias on demand; never eagerly decode the whole art package",
        "derivedArtPolicy": "Byte-identical copies and ICNS container-only assembly; no geometry, recoloring, cropping or extra raster sizes",
        "missingSuppliedPlatformSizes": [512],
        "assets": sorted(assets, key=lambda row: row["path"]), "nativeReleaseReady": False,
    }
    payloads["asset-manifest.json"] = json_bytes(manifest)
    return payloads, manifest


def write_outputs(output: Path, payloads: dict[str, bytes]):
    output = output.resolve()
    if not output.is_relative_to(ROOT):
        raise ValueError("Outputs must remain inside this checkout")
    required = 0
    for name, data in payloads.items():
        path = confined(output, name)
        if not path.is_file() or path.read_bytes() != data:
            required += len(data) + 4096
    import shutil
    ancestor = output
    while not ancestor.exists():
        ancestor = ancestor.parent
    if shutil.disk_usage(ancestor).free < required + 268435456:
        raise ValueError("Insufficient reserved space for the complete Palladium import")
    names = sorted(payloads, key=lambda name: (not name.startswith("history/"), name == "asset-manifest.json", name))
    for name in names:
        path = confined(output, name)
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.is_file() or path.read_bytes() != payloads[name]:
            path.write_bytes(payloads[name])


def import_sources(source: Path, intake_bytes: bytes, output: Path):
    if not source.is_absolute() or not source.is_dir():
        raise ValueError("Supply the exact absolute user-authorized source directory")
    intake = load_intake(intake_bytes)
    if source.resolve() != Path(intake["sourceRoot"]).resolve():
        raise ValueError("Source root differs from the authorized inventoried directory")
    names = {row["path"] for row in intake["files"]}
    actual = {path.relative_to(source).as_posix() for path in source.rglob("*") if path.is_file()}
    if actual != names:
        raise ValueError("Supplied directory file set changed after intake")
    supplied = {row["path"]: read(source, row["path"], row) for row in intake["files"]}
    payloads, manifest = prepare(intake_bytes, supplied)
    write_outputs(output, payloads)
    for row in intake["files"]:
        read(source, row["path"], row)
    return manifest


def rebuild(output: Path):
    intake_bytes = read(BRAND, INTAKE)
    intake = load_intake(intake_bytes)
    supplied = {row["sourcePath"]: read(BRAND, row["targetPath"], row) for row in intake["selectedMappings"]}
    supplied["README.md"] = read(BRAND, "source/Palladium-README.md", next(row for row in intake["files"] if row["path"] == "README.md"))
    supplied["verification.json"] = read(BRAND, "source/Palladium-verification.json", next(row for row in intake["files"] if row["path"] == "verification.json"))
    payloads, manifest = prepare(intake_bytes, supplied)
    write_outputs(output, payloads)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("import", help="Copy only the inventoried supplied bytes; preserve historical Nickel resources")
    command.add_argument("--source-root", required=True)
    command.add_argument("--inventory", required=True)
    command.add_argument("--output", default="native/assets/brand")
    command = commands.add_parser("rebuild", help="Reconstruct the imported byte/container package without reading an external source tree")
    command.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        output = confined(ROOT, args.output)
        if args.command == "import":
            manifest = import_sources(Path(args.source_root), read(ROOT, args.inventory), output)
        else:
            if output.resolve() == BRAND.resolve():
                raise ValueError("Rebuild into a separate marked directory, not over the current resources")
            manifest = rebuild(output)
        print(json.dumps({"product": manifest["product"], "manifest": str(output / "asset-manifest.json"),
                          "assets": len(manifest["assets"]), "qtEmbedded": len(manifest["qtResourceFiles"]),
                          "sourceSha256": manifest["sourceSha256"], "artLicenseConfirmed": True,
                          "sourceRedrawn": False, "nativeReleaseReady": False}, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        print(json.dumps({"passed": False, "nativeReleaseReady": False, "errors": [str(error)]}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
