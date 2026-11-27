#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Inventory only real checked-in brand/font legal materials, not native dependencies."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / ".snow/brand-tools"))
from fontTools.ttLib import TTFont


def material(path: str, kind: str = "file") -> dict:
    actual = (ROOT / path).resolve()
    if not actual.is_relative_to(ROOT) or not actual.is_file():
        raise ValueError(f"Missing actual asset material: {path}")
    data = actual.read_bytes()
    if not data:
        raise ValueError(f"Empty actual asset material: {path}")
    return {"path": path, "sha256": hashlib.sha256(data).hexdigest(), "kind": kind}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="docs/native/licenses/brand-materials.json")
    args = parser.parse_args()
    manifest_path = "native/assets/brand/asset-manifest.json"
    manifest = json.loads((ROOT / manifest_path).read_bytes())
    palladium = manifest.get("product") == "Palladium"
    with TTFont(ROOT / "src/assets/fonts/nickel-line.woff2", recalcTimestamp=False) as font:
        version = next(row.toUnicode() for row in font["name"].names if row.nameID == 5)
        copyright_text = next(row.toUnicode() for row in font["name"].names if row.nameID == 0)
        font_metadata = {"version": version, "copyright": copyright_text, "names": [{"id": row.nameID, "value": row.toUnicode()} for row in font["name"].names],
                         "cmapCount": len(font.getBestCmap()), "originalSha256": hashlib.sha256((ROOT / "src/assets/fonts/nickel-line.woff2").read_bytes()).hexdigest()}
    font_metadata_path = "docs/native/licenses/noto-subset-metadata.json"
    (ROOT / font_metadata_path).parent.mkdir(parents=True, exist_ok=True)
    (ROOT / font_metadata_path).write_bytes((json.dumps(font_metadata, indent=2, ensure_ascii=True) + "\n").encode("utf-8"))
    art_rows = [asset for asset in manifest["assets"] if asset["format"] in ["SVG", "PNG", "ICO", "ICNS"]]
    tool_paths = [
        "scripts/native/brand-test.py", "scripts/native/brand-audit.py", "scripts/native/brand-diff.py",
        "scripts/native/brand-legal.py", "scripts/native/brand-license-input.py",
        "scripts/native/brand-native-materials.py", "scripts/native/brand-repro.py",
        "native/tests/brand/test_brand.py", "native/tests/brand/test_functional.py",
        "native/tests/brand/test_legal.py", "native/tests/brand/test_relink.py", "native/tests/brand/test_diff.py",
        "native/tests/brand/test_native_materials.py", "native/tests/brand/test_audit.py", "native/tests/brand/CMakeLists.txt",
        "native/tests/brand/brand_resources_test.cpp", "native/assets/brand/brand.qrc", "native/assets/brand/functional-resources.qrc"]
    if palladium:
        tool_paths += ["scripts/native/brand-palladium.py", "native/tests/brand/test_palladium.py"]
    tool_sources = [material(path, "source-file") for path in tool_paths]
    tool_sources += [material(path, "source-data") for path in [
        "native/tests/brand/functional-map.json", "docs/native/licenses/bundle-input.schema.json",
        "docs/native/licenses/native-dependency-evidence.schema.json"]]
    artwork = {}
    if palladium:
        license_id = "CC-BY-SA-4.0"
        intake = json.loads((ROOT / "native/assets/brand" / manifest["preferredSource"]).read_bytes())
        source_inventory = material("native/assets/brand/" + manifest["preferredSource"], "supplied-source-inventory")
        artwork = {
            "id": "palladium-project-art", "name": "Palladium / Pd brand and artwork", "version": manifest["revision"], "category": "artwork",
            "linkage": "embedded", "redistributed": True, "licenseConcluded": license_id, "licenseDeclared": license_id,
            "sourceLicenseDeclared": license_id, "sourceCopyright": "Nickel Studio (ItzArona, ItzPlana)",
            "copyrightText": "Nickel Studio (ItzArona, ItzPlana), 2026", "origin": "https://github.com/Nickel-Studio/Pd-Launcher",
            "sourceInventory": source_inventory,
            "artifacts": [material("native/assets/brand/" + row["path"], row["role"]) for row in art_rows],
            "artifactRoles": [{**row, "path": "native/assets/brand/" + row["path"]} for row in art_rows],
            "licenseFiles": [material("LICENSES/CC-BY-SA-4.0.txt", "license"), material("LICENSE-ASSETS", "license")],
            "noticeFiles": [material("native/assets/brand/NOTICE.txt", "notice")], "noticePolicy": "copyright-in-notice",
            "versionEvidence": [material(manifest_path, "version-manifest"), source_inventory],
            "correspondingSource": [material("native/assets/brand/" + row["targetPath"], "original-raster-art") for row in intake["selectedMappings"]]
                + [material("scripts/native/brand-palladium.py", "source-file"), material("native/assets/brand/source/Palladium-README.md", "source-metadata"),
                   material("native/assets/brand/source/Palladium-verification.json", "source-metadata")],
            "sourceDescription": "Actual original supplied PNG/ICO bytes plus source-to-artifact role/hash inventory and copy/container code. The artwork ships under CC-BY-SA-4.0; no vector, font or editable source was supplied or invented."}
    components = [artwork,
        {"id": "noto-sans-sc-subset", "name": "Noto Sans SC existing 109-character subset and regular TTF conversion", "version": version,
         "category": "font", "linkage": "embedded", "redistributed": True, "licenseConcluded": "OFL-1.1", "licenseDeclared": "OFL-1.1",
         "copyrightText": copyright_text, "origin": "https://github.com/notofonts/noto-cjk",
         "artifacts": [material("native/assets/brand/fonts/nickel-line.woff2", "font"), material("native/assets/brand/fonts/nickel-text-subset.ttf", "font")],
         "licenseFiles": [material("native/assets/brand/fonts/OFL-1.1.txt", "license")],
         "noticeFiles": [material("native/assets/brand/fonts/NOTICE.md", "notice")], "noticePolicy": "upstream-notice-conveyed",
         "versionEvidence": [material(font_metadata_path, "extracted-font-metadata")],
         "correspondingSource": [material("src/assets/fonts/nickel-line.woff2", "font-original"), material("scripts/native/brand-palladium.py", "source-file")],
         "sourceDescription": "Existing WOFF2 retained byte-for-byte, not embedded runtime. Static weight400 TTF is its deterministic conversion, not a complete CJK font or icon alphabet."},
        {"id": "palladium-brand-tools", "name": "Palladium brand import, validation and documentation code", "version": manifest["revision"],
         "category": "build-tool", "linkage": "source-only", "redistributed": True, "licenseConcluded": "AGPL-3.0-only", "licenseDeclared": "AGPL-3.0-only",
         "copyrightText": "Nickel Studio and project contributors, 2026", "origin": "https://github.com/Nickel-Studio/Pd-Launcher",
         "artifacts": [material(path, "source-file") for path in tool_paths if path.startswith("scripts/native/")],
         "licenseFiles": [material("LICENSE", "license")], "noticeFiles": [material("NOTICE.md", "notice")], "noticePolicy": "upstream-notice-conveyed",
         "versionEvidence": [material(manifest_path, "version-manifest")], "correspondingSource": tool_sources,
         "buildMaterials": [material("scripts/native/brand-requirements.txt", "tool-version-input")],
         "sourceDescription": "Brand-only AGPL source and existing notices. Full native app source, functional oracles and installed dependency licensing/source/relink remain separate release inputs."}
    ]
    document = {"schemaVersion": 1, "scope": "asset-only", "releaseId": manifest["revision"], "components": components,
                "notes": ["Palladium brand and artwork are distributed under CC-BY-SA-4.0 with attribution to Nickel Studio.",
                          "No Qt/libcurl/archive/crypto materials have been substituted by brand-render tool dependencies.",
                          "Functional game/loader/defaultmod/Steve/Alex/credits assets retain their original rights.",
                          "Material validation does not make nativeReleaseReady or F03 PASS; matching native source/build/dependency/relink evidence is still required."]}
    output = (ROOT / args.output).resolve()
    if not output.is_relative_to(ROOT):
        parser.error("Output must remain inside this checkout")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes((json.dumps(document, indent=2, ensure_ascii=True) + "\n").encode("utf-8"))
    print(json.dumps({"input": str(output), "scope": "asset-only", "components": len(components),
                      "artLicense": "CC-BY-SA-4.0", "nativeReleaseReady": False}))


if __name__ == "__main__":
    main()
