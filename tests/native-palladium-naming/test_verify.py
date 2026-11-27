"""Synthetic mutation tests for the static checker, not native application tests."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("palladium_naming_verify", HERE / "verify.py")
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class MemorySource:
    def __init__(self, files):
        self.files = {path: data.encode("utf-8") if isinstance(data, str) else data for path, data in files.items()}

    def read(self, path):
        verify.source_path(path)
        if path not in self.files:
            raise verify.SourceError("Synthetic source absent")
        return self.files[path]

    def provenance(self):
        return {"sourceMode": "synthetic-fixture", "sourceCommit": None, "reviewedFiles": []}


class NamingChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = verify.load_contract()

    def rule(self, identifier):
        return copy.deepcopy(next(rule for rule in self.contract["rules"] if rule["id"] == identifier))

    def run_rule(self, identifier, files):
        return verify.audit(MemorySource(files), {"rules": [self.rule(identifier)]})

    def test_product_display_and_data_namespace_are_independent(self):
        files = {"native/metadata.json": json.dumps({"displayName": "Palladium", "applicationId": "studio.nickel.launcher"})}
        self.assertEqual(self.run_rule("display.metadata", files)["status"], "pass")
        self.assertEqual(self.run_rule("compat.metadata-id", files)["status"], "pass")
        files["native/metadata.json"] = json.dumps({"displayName": "Palladium", "applicationId": "studio.palladium.launcher"})
        self.assertEqual(self.run_rule("compat.metadata-id", files)["status"], "blocked")

    def test_historical_display_does_not_satisfy_new_metadata(self):
        files = {"native/metadata.json": '{"displayName":"Nickel"}'}
        result = self.run_rule("display.metadata", files)
        self.assertEqual(result["findings"][0]["category"], "display")
        self.assertFalse(result["fullApplicationAcceptance"])
        self.assertFalse(result["releaseArtifactExecuted"])

    def test_cpp_data_constant_is_preserved(self):
        path = "native/core/config/paths.hpp"
        for value, expected in [("Nickel", "pass"), ("Palladium", "blocked"), ("Pd", "blocked")]:
            result = self.run_rule("compat.data-directory", {path: 'inline constexpr std::string_view data_directory_name = "' + value + '";'})
            self.assertEqual(result["status"], expected)

    def test_comment_cannot_mask_changed_cpp_constant(self):
        source = '// data_directory_name = "Nickel";\ninline constexpr std::string_view data_directory_name = "Pd";'
        self.assertEqual(self.run_rule("compat.data-directory", {"native/core/config/paths.hpp": source})["status"], "blocked")

    def test_duplicate_cpp_constant_does_not_pass(self):
        source = 'data_directory_name = "Nickel";\ndata_directory_name = "Nickel";'
        self.assertEqual(self.run_rule("compat.data-directory", {"native/core/config/paths.hpp": source})["status"], "blocked")

    def test_cpp_escaped_ownership_token(self):
        source = 'offline_skin_owner_content = "Nickel offline skin pack v1\\n";'
        self.assertEqual(self.run_rule("compat.skin-ownership", {"native/core/media/offline_skin.hpp": source})["status"], "pass")

    def test_cpp_comments_are_ignored_but_string_slashes_are_not(self):
        source = 'const auto url = "https://example.invalid/Nickel"; // "Nickel could not start"\nQObject::tr("Palladium could not start");'
        self.assertEqual(self.run_rule("display.entry-errors", {"native/app/main.cpp": source})["status"], "pass")
        source += '\nQObject::tr("Nickel could not start");'
        result = self.run_rule("display.entry-errors", {"native/app/main.cpp": source})
        self.assertEqual(result["status"], "blocked")
        self.assertEqual(result["findings"][0]["line"], 3)

    def test_missing_source_is_a_blocker(self):
        result = self.run_rule("display.window", {})
        self.assertEqual(result["status"], "blocked")
        self.assertIn("unavailable", result["findings"][0]["reason"])

    def test_malformed_json_is_not_a_pass(self):
        self.assertEqual(self.run_rule("display.metadata", {"native/metadata.json": "{broken"})["status"], "blocked")

    def test_duplicate_json_metadata_does_not_satisfy_contract(self):
        files = {"native/metadata.json": '{"displayName":"Nickel","displayName":"Palladium"}'}
        self.assertEqual(self.run_rule("display.metadata", files)["status"], "blocked")

    def test_compatibility_mode_does_not_claim_display_acceptance(self):
        rules = [self.rule("compat.data-directory"), self.rule("display.metadata")]
        source = MemorySource({"native/core/config/paths.hpp": 'data_directory_name = "Nickel";'})
        result = verify.audit(source, {"rules": rules}, "compatibility")
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["checkedRules"], 1)
        self.assertFalse(result["fullApplicationAcceptance"])
        self.assertFalse(result["cppParserExecuted"])
        self.assertFalse(result["userArtInspected"])

    def portable(self, windows, linux, parser):
        writer = 'const auto name = os == "windows" ? "' + windows + '" : "' + linux + '";'
        return self.run_rule("compat.portable-entry-coordination", {"native/core/packs/portable.cpp": writer, "native/core/config/paths.cpp": parser})

    def test_schema_one_legacy_entry_names_are_valid(self):
        self.assertEqual(self.portable("Nickel.exe", "Nickel-launcher", '"Nickel.exe" "Nickel-launcher"')["status"], "pass")

    def test_new_portable_entry_requires_both_legacy_and_new_tokens(self):
        old = '"Nickel.exe" "Nickel-launcher"'
        self.assertEqual(self.portable("pd-launcher.exe", "pd-launcher", old)["status"], "blocked")
        self.assertEqual(self.portable("pd-launcher.exe", "pd-launcher", old + ' "pd-launcher.exe" "pd-launcher"')["status"], "pass")
        self.assertEqual(self.portable("pd-launcher.exe", "pd-launcher", '"pd-launcher.exe" "pd-launcher"')["status"], "blocked")

    def test_portable_comments_do_not_supply_compatibility(self):
        source = '"pd-launcher.exe" "pd-launcher" // "Nickel.exe" "Nickel-launcher"'
        self.assertEqual(self.portable("pd-launcher.exe", "pd-launcher", source)["status"], "blocked")

    def test_arbitrary_and_traversing_portable_entries_are_blocked(self):
        self.assertEqual(self.portable("bin/pd-launcher.exe", "pd-launcher", '"Nickel.exe" "Nickel-launcher" "bin/pd-launcher.exe" "pd-launcher"')["status"], "blocked")

    def test_rule_selector_changes_fail_for_owner_review(self):
        files = {"native/core/packs/portable.cpp": "name = selected_name();", "native/core/config/paths.cpp": '"Nickel.exe" "Nickel-launcher"'}
        self.assertEqual(self.run_rule("compat.portable-entry-coordination", files)["status"], "blocked")

    def test_initial_request_uses_content_hash_not_updated_plan_text(self):
        rule = self.rule("compat.initial-request")
        request = "Synthetic initial request"
        rule["expected"] = hashlib.sha256(request.encode()).hexdigest()
        text = "# Plan\n```text\n" + request + "\n```\n## Palladium implementation addendum\n"
        source = MemorySource({rule["path"]: text})
        self.assertEqual(verify.audit(source, {"rules": [rule]})["status"], "pass")
        source = MemorySource({rule["path"]: text.replace(request, "Changed initial request")})
        self.assertEqual(verify.audit(source, {"rules": [rule]})["status"], "blocked")

    def test_normalized_fixture_hash_allows_crlf_only(self):
        rule = self.rule("compat.functional-baseline")
        rule["expected"] = hashlib.sha256(b"frozen\n").hexdigest()
        self.assertEqual(verify.audit(MemorySource({rule["path"]: b"frozen\r\n"}), {"rules": [rule]})["status"], "pass")
        self.assertEqual(verify.audit(MemorySource({rule["path"]: b"renamed\n"}), {"rules": [rule]})["status"], "blocked")

    def test_paths_cannot_escape_repository(self):
        for path in ["", "../secret", "a/../secret", "/secret", "C:/secret", "a\\secret", ".git/config", "a/./b"]:
            with self.subTest(path=path), self.assertRaises(verify.SourceError):
                verify.source_path(path)

    def test_theme_only_changes_visible_label(self):
        files = {"native/ui/Theme.cpp": '{Accent::Nickel, "nickel", "\u94af\u5149", "#8dbcae"}'}
        self.assertEqual(self.run_rule("display.theme-label-only", files)["status"], "pass")
        self.assertEqual(self.run_rule("compat.theme-key", files)["status"], "pass")
        files["native/ui/Theme.cpp"] = '{Accent::Nickel, "nickel", "\u954d\u5149", "#8dbcae"}'
        self.assertEqual(self.run_rule("display.theme-label-only", files)["status"], "blocked")

    def test_native_theme_storage_key_cannot_be_branded_as_pd(self):
        files = {"native/ui/Theme.cpp": '{Accent::Nickel, "pd", "\u94af\u5149", "#8dbcae"}'}
        self.assertEqual(self.run_rule("compat.theme-key", files)["status"], "blocked")


if __name__ == "__main__":
    unittest.main()
