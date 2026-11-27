# SPDX-License-Identifier: AGPL-3.0-only
"""Scope-gate negative fixtures are synthetic and never evidence of a real diff."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("nickel_brand_audit", ROOT / "scripts/native/brand-audit.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class BrandScopeTests(unittest.TestCase):
    def setUp(self):
        self.changes = {"schemaVersion": 1, "owner": "brand-release", "base": "a" * 40, "candidate": "b" * 40,
                        "paths": ["native/assets/brand/mark-black.svg", "native/tests/brand/test_audit.py",
                                  "scripts/native/brand-audit.py", "docs/native/licenses/brand-materials.json",
                                  "scripts/native/.gitattributes"]}

    def test_accepts_only_authorized_owner_paths(self):
        self.assertEqual(AUDIT.validate_changes(self.changes), [])

    def test_rejects_ui_functional_and_other_owner_changes(self):
        for path in ["src/assets/blocks/Grass.png", "src/assets/fonts/nickel-line.woff2", "tests/brand/functional-icons.sha256",
                     "native/ui/main_window.cpp", "README.md", "website/index.html", "tests/native-acceptance/record_ctest.py",
                     "scripts/native/build-release.py", "native/core/common/error.hpp"]:
            with self.subTest(path=path):
                document = copy.deepcopy(self.changes)
                document["paths"].append(path)
                self.assertTrue(AUDIT.validate_changes(document))

    def test_rejects_traversal_aliases_and_windows_drive_paths(self):
        for path in ["", None, "../native/assets/brand/icon.svg", "native/assets/brand/../ui/page.cpp",
                     "native/assets/brand//icon.svg", "native/assets/brand/./icon.svg", "D:/native/assets/brand/icon.svg",
                     "native\\assets\\brand\\icon.svg", "native/assets/brand/icon.svg:secret", " native/assets/brand/icon.svg"]:
            with self.subTest(path=path):
                self.assertFalse(AUDIT.owned_path(path))

    def test_rejects_duplicate_and_case_colliding_paths(self):
        self.changes["paths"].append("native/assets/brand/mark-black.svg")
        self.assertTrue(AUDIT.validate_changes(self.changes))
        self.changes["paths"][-1] = "native/assets/brand/MARK-black.svg"
        self.assertTrue(AUDIT.validate_changes(self.changes))

    def test_rejects_missing_provenance_empty_diff_and_wrong_owner(self):
        for field, value in [("owner", "interface"), ("schemaVersion", 0), ("base", "unrecorded"),
                             ("candidate", self.changes["base"]), ("paths", []), ("paths", "native/assets/brand/icon.svg")]:
            with self.subTest(field=field):
                document = copy.deepcopy(self.changes)
                document[field] = value
                self.assertTrue(AUDIT.validate_changes(document))

    def test_real_manifest_and_runtime_subset_pass(self):
        result = AUDIT.validate_assets(ROOT)
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["assetCount"], 70)
        self.assertEqual(result["qtEmbeddedCount"], 20)
        self.assertEqual(result["artLicense"], "CC-BY-SA-4.0")
        self.assertTrue(result["artLicenseConfirmed"])
        self.assertLess(result["qtEmbeddedCount"], result["assetCount"])

    def test_rejects_website_and_preferred_source_runtime_embedding(self):
        for path in ["banners/palladium-launcher-hero-light.png", "platform/favicon-16.png", "fonts/nickel-line.woff2"]:
            with self.subTest(path=path):
                manifest = json.loads((ROOT / "native/assets/brand/asset-manifest.json").read_bytes())
                entries = {alias: alias for alias in manifest["qtResourceFiles"]}
                entries[path] = path
                manifest["qtResourceFiles"].append(path)
                next(row for row in manifest["assets"] if row["path"] == path)["qtEmbedded"] = True
                self.assertTrue(AUDIT.validate_runtime(manifest, entries))

    def test_rejects_missing_alias_and_false_embedding_flag(self):
        manifest = json.loads((ROOT / "native/assets/brand/asset-manifest.json").read_bytes())
        entries = {alias: alias for alias in manifest["qtResourceFiles"]}
        del entries["mark-black.png"]
        self.assertTrue(AUDIT.validate_runtime(manifest, entries))
        entries["mark-black.png"] = "mark-white.png"
        self.assertTrue(AUDIT.validate_runtime(manifest, entries))

    def test_missing_diff_cannot_claim_final_acceptance(self):
        result = AUDIT.audit(None)
        self.assertFalse(result["passed"])
        self.assertFalse(result["finalDifferenceGatePassed"])
        self.assertFalse(result["nativeReleaseReady"])

    def test_explicit_asset_only_does_not_claim_diff_or_native_acceptance(self):
        result = AUDIT.audit(None, assets_only=True)
        self.assertTrue(result["passed"], result["errors"])
        self.assertFalse(result["scopeValidationPassed"])
        self.assertFalse(result["finalDifferenceGatePassed"])
        self.assertFalse(result["nativeUiConsumerVerified"])
        self.assertFalse(result["nativeReleaseReady"])

    def test_source_inventory_and_format_only_fake_diff_cannot_pass_final_gate(self):
        result = AUDIT.audit(self.changes)
        self.assertFalse(result["passed"])
        self.assertFalse(result["finalDifferenceGatePassed"])
        self.assertIn("genuine git-owner-diff", "\n".join(result["errors"]))

    def test_claiming_git_kind_without_real_commits_cannot_pass_final_gate(self):
        self.changes["kind"] = "git-owner-diff"
        result = AUDIT.audit(self.changes)
        self.assertFalse(result["passed"])
        self.assertFalse(result["finalDifferenceGatePassed"])
        self.assertIn("Actual Git delta", "\n".join(result["errors"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
