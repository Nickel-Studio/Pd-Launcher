#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Gate immutable assets and an explicitly supplied, owner-scoped release diff."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import sys
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "native/tests/brand"))
from test_functional import FunctionalIntegrityTests, baseline_entries, load_fixture

PREFIXES = ("native/assets/brand/", "native/tests/brand/", "docs/native/licenses/")
EXACT_PATHS = {"scripts/native/.gitattributes", "scripts/native/.gitignore"}
ASSET_SUFFIXES = {".svg", ".png", ".ico", ".icns", ".woff2", ".ttf", ".qrc"}
RUNTIME_ROLES = {"brand-mark", "application-icon", "outlined-wordmark", "raster-wordmark", "qt-text-font",
                 "font-license", "font-notice", "brand-license", "brand-attribution"}
REQUIRED_RUNTIME = {"mark-black.svg", "mark-white.svg", "icon.svg", "icon.png",
                    "fonts/nickel-text-subset.ttf", "fonts/OFL-1.1.txt", "fonts/NOTICE.md", "NOTICE.txt"}
PALLADIUM_RUNTIME = {"mark-black.png", "mark-white.png", "mark-blue.png", "mark-silver.png", "icon.png", "icon-light.png",
                     "fonts/nickel-text-subset.ttf", "fonts/OFL-1.1.txt", "fonts/NOTICE.md", "NOTICE.txt"}
PALLADIUM_RUNTIME |= {f"wordmarks/{name}-{color}.png" for name in ("palladium", "pd", "ba", "palladium-launcher", "pd-launcher")
                      for color in ("black", "white")}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_path(value: str) -> bool:
    if not isinstance(value, str) or not value or value != value.strip():
        return False
    if any(char in value for char in "\\:\x00\r\n"):
        return False
    path = PurePosixPath(value)
    return not path.is_absolute() and path.as_posix() == value and all(part not in {".", ".."} for part in path.parts)


def owned_path(value: str) -> bool:
    if not canonical_path(value):
        return False
    if value in EXACT_PATHS or value.startswith(PREFIXES):
        return True
    path = PurePosixPath(value)
    return path.parent.as_posix() == "scripts/native" and path.name.startswith("brand") and path.suffix in {".py", ".txt"}


def validate_changes(document: dict) -> list[str]:
    errors = []
    if not isinstance(document, dict) or document.get("schemaVersion") != 1 or document.get("owner") != "brand-release":
        return ["A schemaVersion=1 owner=brand-release diff inventory is required"]
    for field in ("base", "candidate"):
        if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", str(document.get(field, ""))):
            errors.append(f"Diff {field} must identify an actual revision or SHA-256 snapshot")
    if document.get("base") == document.get("candidate"):
        errors.append("Diff base and candidate must differ")
    paths = document.get("paths")
    if not isinstance(paths, list) or not paths:
        return errors + ["Diff paths must be a non-empty complete changed-path list"]
    seen = set()
    for path in paths:
        if not owned_path(path):
            errors.append(f"Outside the authorized brand scope or invalid path: {path!r}")
            continue
        key = path.casefold()
        if key in seen:
            errors.append(f"Duplicate or case-colliding diff path: {path}")
        seen.add(key)
    return errors


def verify_git_changes(document: dict) -> list[str]:
    if document.get("kind") != "git-owner-diff":
        return ["Final difference gate requires a genuine git-owner-diff, not an owned source inventory"]
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location("nickel_actual_brand_diff", ROOT / "scripts/native/brand-diff.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        actual = module.delta(document["base"], document["candidate"])
        for field in ["paths", "changes", "rawSha256", "baseTree", "candidateTree", "gitArgv"]:
            if document.get(field) != actual[field]:
                return [f"Actual Git delta mismatch or omitted changes: {field}"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        return [f"Actual Git delta could not be verified: {error}"]
    return []


def validate_runtime(manifest: dict, entries: dict[str, str]) -> list[str]:
    errors = []
    declared = manifest.get("qtResourceFiles", [])
    if not isinstance(declared, list) or len(declared) != len(set(declared)) or set(declared) != set(entries):
        errors.append("Qt runtime aliases must exactly match the explicit unique resource manifest")
    if manifest.get("product") == "Palladium":
        if set(entries) != PALLADIUM_RUNTIME:
            errors.append("Only the exact 16 Palladium PNGs, retained TTF and current offline notices may enter desktop resources")
    elif not REQUIRED_RUNTIME.issubset(entries):
        errors.append("Required desktop brand, TTF or offline notice resources are missing")
    assets = {row["path"]: row for row in manifest["assets"]}
    for alias, target in entries.items():
        if alias != target or alias not in assets:
            errors.append(f"Unexpected Qt brand resource alias/target: {alias}")
            continue
        row = assets[alias]
        if alias.startswith(("banners/", "platform/", "source/", "history/")) or "@" in alias or alias.endswith(".woff2"):
            errors.append(f"Website, platform packaging or preferred source embedded in desktop: {alias}")
        if row.get("role") not in RUNTIME_ROLES:
            errors.append(f"Non-desktop resource role embedded: {alias}")
    for path, row in assets.items():
        if row.get("qtEmbedded") is not (path in entries):
            errors.append(f"Per-asset Qt embedding flag drift: {path}")
    return errors


def validate_assets(root: Path) -> dict:
    brand = root / "native/assets/brand"
    manifest_path = brand / "asset-manifest.json"
    manifest = json.loads(manifest_path.read_bytes())
    errors = []
    if manifest.get("schemaVersion") != 1 or not manifest.get("assets"):
        raise ValueError("A non-empty schemaVersion=1 actual asset manifest is required")
    files = set()
    forbidden_hashes = set(baseline_entries().values()) | set(load_fixture()["additionalImmutable"].values())
    for row in manifest["assets"]:
        path = row["path"]
        if not canonical_path(path) or path.casefold() in {name.casefold() for name in files}:
            errors.append(f"Invalid or duplicate asset path: {path!r}")
            continue
        files.add(path)
        actual = brand / path
        if actual.is_symlink() or not actual.resolve().is_relative_to(brand.resolve()) or not actual.is_file():
            errors.append(f"Missing or unsafe actual asset: {path}")
            continue
        if actual.stat().st_size != row["bytes"] or digest(actual) != row["sha256"]:
            errors.append(f"Asset bytes/hash drift: {path}")
        if row.get("role") in {"brand-mark", "application-icon", "raster-wordmark", "brand-banner", "platform-icon"} and row["sha256"] in forbidden_hashes:
            errors.append(f"Functional third-party artwork cannot be relabeled as brand: {path}")
    preferred = manifest.get("preferredSource")
    if not canonical_path(preferred) or not (brand / preferred).is_file() or digest(brand / preferred) != manifest.get("sourceSha256"):
        errors.append("Preferred supplied source inventory or historical vector source is missing or changed")
    expected_files = files | {preferred}
    if manifest.get("product") == "Palladium":
        intake = json.loads((brand / preferred).read_bytes())
        if digest(brand / preferred) != "685dc8657abbd73d0bb458dd90ea7507bf52a40f5c6129028c3bea78a911a066":
            raise ValueError("Palladium supplied-source inventory changed")
        assets = {row["path"]: row for row in manifest["assets"]}
        for mapping in intake["selectedMappings"]:
            row = assets.get(mapping["targetPath"], {})
            if row.get("sha256") != mapping["sha256"] or row.get("sourcePath") != mapping["sourcePath"]:
                errors.append("Palladium supplied source/target binding differs: " + mapping["targetPath"])
    actual_files = {path.relative_to(brand).as_posix() for path in brand.rglob("*") if path.is_file() and path.suffix.lower() in ASSET_SUFFIXES}
    if actual_files != {path for path in expected_files if path and PurePosixPath(path).suffix.lower() in ASSET_SUFFIXES}:
        errors.append("Unmanifested or missing active/historical artwork/font/QRC files")
    tree = ET.parse(brand / "brand.qrc").getroot()
    resources = tree.findall("qresource")
    if len(resources) != 1 or resources[0].attrib.get("prefix") != "/nickel/brand":
        raise ValueError("Exactly one stable /nickel/brand Qt prefix is required")
    entries = {}
    for node in resources[0].findall("file"):
        alias = node.attrib.get("alias")
        if not canonical_path(alias) or not canonical_path(node.text) or alias in entries:
            errors.append("Malformed or duplicate Qt resource entry")
        else:
            entries[alias] = node.text
    errors += validate_runtime(manifest, entries)
    return {"assetCount": len(files), "qtEmbeddedCount": len(entries), "manifestSha256": digest(manifest_path),
            "sourceSha256": manifest.get("sourceSha256"), "artLicense": manifest.get("artLicense"),
            "artLicenseConfirmed": manifest.get("artLicenseConfirmed", False), "errors": errors}


def audit(changes: dict | None, *, assets_only: bool = False) -> dict:
    log = io.StringIO()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FunctionalIntegrityTests)
    functional = unittest.TextTestRunner(stream=log, verbosity=2).run(suite)
    errors = []
    if not functional.wasSuccessful():
        errors.append(log.getvalue())
    try:
        assets = validate_assets(ROOT)
        errors += assets["errors"]
    except (OSError, ValueError, KeyError, TypeError, AssertionError, ET.ParseError) as error:
        assets = {"errors": [str(error)]}
        errors += assets["errors"]
    scope_errors = validate_changes(changes) if changes is not None else []
    if changes is not None and not scope_errors:
        scope_errors += verify_git_changes(changes)
    if changes is None and not assets_only:
        scope_errors.append("Missing actual owner Git diff; use --assets-only only for non-final asset checks")
    errors += scope_errors
    scope_passed = changes is not None and not scope_errors
    return {"schemaVersion": 1, "scope": "asset-only", "assetsOnly": assets_only,
            "assetIntegrityPassed": not assets["errors"], "functionalIntegrityPassed": functional.wasSuccessful(),
            "functionalTestsRun": functional.testsRun, "historicalHashes": 18, "additionalHashes": 7, "iconKeys": 12,
            "scopeValidationPassed": scope_passed, "finalDifferenceGatePassed": scope_passed and not errors,
            "nativeUiConsumerVerified": False, "nativeReleaseReady": False,
            "diff": changes, "assets": assets, "passed": not errors, "errors": errors}


def inside(value: str) -> Path:
    if not canonical_path(value):
        raise ValueError("Use an exact relative path inside this checkout")
    result = ROOT / value
    if not result.resolve().is_relative_to(ROOT):
        raise ValueError("Path must remain inside this checkout")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--changes", help="Trusted CI/lead actual base-to-candidate owner diff JSON; never fabricated from an allowlist")
    mode.add_argument("--assets-only", action="store_true", help="Check assets, explicitly leave final difference/native consumer gates unverified")
    parser.add_argument("--evidence", default=".snow/brand-evidence/audit.json")
    args = parser.parse_args()
    try:
        changes = json.loads(inside(args.changes).read_bytes()) if args.changes else None
        result = audit(changes, assets_only=args.assets_only)
        output = inside(args.evidence)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes((json.dumps(result, indent=2, ensure_ascii=True) + "\n").encode("utf-8"))
    except (OSError, ValueError, KeyError, TypeError) as error:
        result = {"passed": False, "finalDifferenceGatePassed": False, "nativeReleaseReady": False, "errors": [str(error)]}
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
