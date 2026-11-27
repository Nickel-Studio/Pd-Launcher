# SPDX-License-Identifier: AGPL-3.0-only
"""Negative legal-material tests and a real asset-only copied-bundle verification."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / ".snow/brand-tools"))
SPEC = importlib.util.spec_from_file_location("nickel_brand_legal", ROOT / "scripts/native/brand-legal.py")
LEGAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LEGAL)


class LegalMaterialTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / ".snow/legal-unit"
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="case-", dir=parent)
        self.root = Path(self.temporary.name)
        self.component = {
            "id": "nickel-code", "name": "Nickel brand validation code", "version": "native-test-1", "category": "build-tool",
            "linkage": "source-only", "redistributed": True, "licenseConcluded": "AGPL-3.0-only", "licenseDeclared": "AGPL-3.0-only",
            "copyrightText": "Nickel Studio and project contributors, 2026", "origin": "https://github.com/Nickel-Studio/Ni-Launcher",
            "artifacts": [self.put("brand-palladium.py", (ROOT / "scripts/native/brand-palladium.py").read_bytes(), "source-file")],
            "licenseFiles": [self.put("LICENSE.txt", (ROOT / "LICENSE").read_bytes(), "license")],
            "noticeFiles": [self.put("NOTICE.txt", (ROOT / "native/assets/brand/NOTICE.txt").read_bytes(), "notice")],
            "noticePolicy": "copyright-in-notice", "versionEvidence": [self.put("actual-version.json", b'{"version":"native-test-1"}\n', "version-manifest")],
            "correspondingSource": [self.put("brand-source.py", (ROOT / "scripts/native/brand-palladium.py").read_bytes(), "source-file")]
        }
        self.document = {"schemaVersion": 1, "scope": "asset-only", "releaseId": "native-test-1", "components": [self.component]}

    def tearDown(self):
        self.temporary.cleanup()

    def put(self, path: str, data: bytes, kind: str = "file") -> dict:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return {"path": path, "sha256": hashlib.sha256(data).hexdigest(), "kind": kind}

    def errors(self, document: dict | None = None) -> str:
        return "\n".join(LEGAL.validate_input(document or self.document, self.root))

    def test_actual_code_license_source_passes_asset_scope_only(self):
        self.assertEqual(self.errors(), "")
        input_path = self.root / "input.json"
        input_path.write_bytes((json.dumps(self.document) + "\n").encode("utf-8"))
        bundle = self.root / "bundle"
        result = LEGAL.collect(input_path, bundle, self.root)
        self.assertTrue(result["materialValidationPassed"])
        self.assertTrue(result["spdxValidationPassed"])
        self.assertFalse(result["nativeReleaseReady"])
        verified = LEGAL.verify_bundle(bundle)
        self.assertTrue(verified["materialValidationPassed"], verified["errors"])
        self.assertFalse(verified["nativeReleaseReady"])
        self.assertEqual(LEGAL.validate_spdx(bundle / "dependencies.spdx.json"), [])
        self.assertEqual((bundle / result["materials"][0]["path"]).read_bytes(), (self.root / "brand-palladium.py").read_bytes())

    def test_rejects_mutated_material_hash(self):
        (self.root / "brand-palladium.py").write_bytes(b"modified after material inventory\n")
        self.assertIn("hash mismatch", self.errors())

    def test_rejects_missing_and_empty_license_material(self):
        self.component["licenseFiles"][0]["path"] = "missing-license.txt"
        self.assertIn("Missing real material", self.errors())
        self.component["licenseFiles"] = [self.put("empty-license.txt", b"", "license")]
        self.assertIn("Empty material", self.errors())

    def test_rejects_template_and_wrong_license_text(self):
        self.component["licenseFiles"] = [self.put("LICENSE-template.txt", (ROOT / "LICENSE").read_bytes(), "license")]
        self.assertIn("template license", self.errors())
        self.component["licenseFiles"] = [self.put("LICENSE-notes.txt", b"These are release notes, not a grant of rights.\n" * 10, "license")]
        self.assertIn("actual text", self.errors())

    def test_rejects_unknown_or_unconcluded_spdx(self):
        for expression in ["MadeUp-1.0", "NONE", "NOASSERTION", "MIT AND", "(MIT OR Apache-2.0", "MIT GPL-3.0-only"]:
            with self.subTest(expression=expression):
                self.component["licenseConcluded"] = expression
                self.assertTrue(self.errors())

    def test_rejects_version_guess_or_dependency_drift(self):
        self.component["version"] = "999-guessed"
        self.assertIn("version is not present", self.errors())

    def test_rejects_url_as_corresponding_source_and_readme_promise(self):
        self.component["correspondingSource"] = [{"path": "https://example.org/source.zip", "sha256": "0" * 64, "kind": "source-archive"}]
        self.assertTrue(self.errors())
        self.component["correspondingSource"] = [self.put("source-promise.md", b"Please download source from somewhere.\n", "documentation")]
        self.assertIn("README is not corresponding source", self.errors())

    def test_rejects_absolute_traversal_and_invalid_paths(self):
        for path in ["../LICENSE", str(ROOT / "LICENSE"), "", None]:
            with self.subTest(path=path):
                self.component["artifacts"][0]["path"] = path
                self.assertTrue(self.errors())

    def test_cannot_escalate_asset_scope_without_a_real_native_build(self):
        self.document["scope"] = "native-release"
        self.assertIn("build", self.errors())
        self.document["build"] = {"platform": "Windows", "architecture": "x64", "compiler": "MSVC", "compilerVersion": "actual-unavailable",
                                  "executable": self.put("nickel.exe", b"This is not a PE executable\n", "executable"), "evidence": []}
        self.assertTrue(self.errors())
        self.assertEqual(LEGAL.binary_kind(self.root / "nickel.exe"), "other")

    def test_static_library_is_not_a_text_file_with_lib_extension(self):
        self.component["linkage"] = "static"
        self.component["artifacts"] = [self.put("Qt6Core.lib", b"static library goes here\n" * 8, "static-library")]
        self.assertIn("not a real archive", self.errors())

    def test_qt_needs_modules_third_party_and_both_lgpl_gpl_texts(self):
        self.component["category"] = "qt"
        self.assertIn("Qt modules", self.errors())
        with self.assertRaisesRegex(LEGAL.MaterialError, "LGPL-3.0-only"):
            LEGAL.verify_license_text({"id": "qt", "licenseConcluded": "LGPL-3.0-only"}, [self.root / "LICENSE.txt"])
        # Synthetic negative fixture, never shipped or treated as a license.
        lgpl_marker = self.put("negative-lgpl-marker.txt", b"GNU LESSER GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n" + b"negative-test-only\n" * 10, "negative-fixture")
        with self.assertRaisesRegex(LEGAL.MaterialError, "GPLv3"):
            LEGAL.verify_license_text({"id": "qt", "licenseConcluded": "LGPL-3.0-only"}, [self.root / lgpl_marker["path"]])

    def test_missing_static_lgpl_relink_is_not_waived_by_agpl(self):
        with self.assertRaisesRegex(LEGAL.MaterialError, "AGPL does not waive"):
            LEGAL.validate_relink({}, self.root, [{"id": "qtbase"}], self.root / "brand-palladium.py")

    def test_source_archive_requires_preferred_source_and_safe_paths(self):
        path = self.root / "source.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("README.md", "only a source promise")
        with self.assertRaisesRegex(LEGAL.MaterialError, "no preferred-form"):
            LEGAL.verify_source_archive(path)
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("../escape.cpp", "int main() { return 0; }")
        with self.assertRaisesRegex(LEGAL.MaterialError, "unsafe path"):
            LEGAL.verify_source_archive(path)
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("src/core.cpp", (ROOT / "native/tests/brand/brand_resources_test.cpp").read_bytes())
        LEGAL.verify_source_archive(path)

    def test_collected_bundle_detects_tamper_and_cannot_be_overwritten(self):
        input_path = self.root / "input.json"
        input_path.write_bytes(json.dumps(self.document).encode("utf-8"))
        bundle = self.root / "bundle"
        manifest = LEGAL.collect(input_path, bundle, self.root)
        with self.assertRaisesRegex(LEGAL.MaterialError, "non-empty bundle"):
            LEGAL.collect(input_path, bundle, self.root)
        (bundle / manifest["materials"][0]["path"]).write_bytes(b"tampered material\n")
        result = LEGAL.verify_bundle(bundle)
        self.assertFalse(result["materialValidationPassed"])
        self.assertFalse(result["nativeReleaseReady"])

    def test_real_checked_in_brand_font_license_input_schema_and_hashes(self):
        document = json.loads((ROOT / "docs/native/licenses/brand-materials.json").read_bytes())
        self.assertEqual(document["scope"], "asset-only")
        self.assertEqual({row["id"] for row in document["components"]}, {"palladium-project-art", "noto-sans-sc-subset", "palladium-brand-tools"})
        self.assertEqual(LEGAL.validate_input(document, ROOT), [])
        artwork = next(row for row in document["components"] if row["id"] == "palladium-project-art")
        self.assertEqual(artwork["licenseConcluded"], "CC-BY-SA-4.0")
        self.assertEqual(artwork["sourceCopyright"], "Nickel Studio (ItzArona, ItzPlana)")
        self.assertEqual(artwork["sourceLicenseDeclared"], "CC-BY-SA-4.0")
        self.assertEqual({row["path"] for row in artwork["licenseFiles"]}, {"LICENSES/CC-BY-SA-4.0.txt", "LICENSE-ASSETS"})
        self.assertFalse(LEGAL.readiness("asset-only", passed=True)["nativeLegalMaterialValidationPassed"])

    def test_artwork_license_cannot_cover_code_or_lose_attribution(self):
        document = json.loads((ROOT / "docs/native/licenses/brand-materials.json").read_bytes())
        artwork = next(row for row in document["components"] if row["id"] == "palladium-project-art")
        for field, value in [("category", "qt"), ("copyrightText", "TEMPLATE ONLY"),
                             ("licenseDeclared", "MIT"), ("redistributed", False)]:
            with self.subTest(field=field):
                changed = copy.deepcopy(document)
                next(row for row in changed["components"] if row["id"] == artwork["id"])[field] = value
                self.assertTrue(LEGAL.validate_input(changed, ROOT))
        changed = copy.deepcopy(document)
        next(row for row in changed["components"] if row["id"] == artwork["id"])["correspondingSource"] = []
        self.assertIn("actual supplied raster bytes", "\n".join(LEGAL.validate_input(changed, ROOT)))
        document["scope"] = "native-release"
        self.assertTrue(LEGAL.validate_input(document, ROOT), "Project art authority must not bypass real native build/relink inputs")

    def test_actual_project_art_bundle_preserves_source_roles_after_relocation(self):
        bundle = self.root / "palladium-actual-bundle"
        result = LEGAL.collect(ROOT / "docs/native/licenses/brand-materials.json", bundle, ROOT)
        original_input = (bundle / "input-materials.json").read_bytes()
        relocated = self.root / "palladium moved & bundle"
        bundle.rename(relocated)
        self.assertFalse(bundle.exists())
        bundle = relocated
        self.assertEqual((bundle / "input-materials.json").read_bytes(), original_input)
        verified = LEGAL.verify_bundle(bundle)
        self.assertTrue(verified["materialValidationPassed"], verified["errors"])
        self.assertFalse(verified["nativeLegalMaterialValidationPassed"])
        self.assertFalse(verified["dependencySnapshotValidationPassed"])
        self.assertFalse(verified["nativeReleaseReady"])
        recorded_art = next(row for row in json.loads(original_input)["components"] if row["id"] == "palladium-project-art")
        expected_art = next(row for row in json.loads((ROOT / "docs/native/licenses/brand-materials.json").read_bytes())["components"]
                            if row["id"] == "palladium-project-art")
        self.assertEqual(recorded_art["artifactRoles"], expected_art["artifactRoles"])
        spdx = json.loads((bundle / "dependencies.spdx.json").read_bytes())
        artwork_package = next(row for row in spdx["packages"] if row["name"] == expected_art["name"])
        self.assertEqual(artwork_package["licenseConcluded"], "CC-BY-SA-4.0")
        self.assertNotIn("LicenseRef-Palladium-Project-Art",
                         {row["licenseId"] for row in spdx["hasExtractedLicensingInfos"]})
        source = next(row for row in result["materials"] if row["originalPath"] == "native/assets/brand/icon.png")
        (bundle / source["path"]).write_bytes(b"changed copied artwork")
        self.assertFalse(LEGAL.verify_bundle(bundle)["materialValidationPassed"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
