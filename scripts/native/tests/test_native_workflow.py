"""Static contract checks for the active build workflow (`.github/workflows/build.yml`).

These assertions read the committed workflow text; they never execute a hosted
runner. They exist to keep the build entrypoints, pinned toolchain and test
inventory in sync with the repository layout.
"""

from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github/workflows/build.yml"
PRESETS = ROOT / "CMakePresets.json"
REFERENCE = re.compile(r"(?<![\w./-@])((?:scripts|tests|native|cmake|src|website|docs)/[\w./-]+)")


class BuildWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = WORKFLOW.read_text(encoding="utf-8")

    def test_workflow_is_read_only_and_never_publishes(self):
        self.assertIn("name: Build", self.text)
        self.assertIn("permissions:\n  contents: read", self.text)
        self.assertNotIn("contents: write", self.text)
        self.assertNotIn("secrets.", self.text)
        self.assertNotIn("gh release", self.text)
        self.assertNotIn("--install", self.text)

    def test_triggers_cover_main_pull_requests_and_manual_runs(self):
        self.assertIn("branches: [main]", self.text)
        self.assertIn("pull_request:", self.text)
        self.assertIn("workflow_dispatch:", self.text)

    def test_runner_matrix_covers_windows_and_linux_only(self):
        self.assertEqual(re.findall(r"^\s+host:\s*(\S+)", self.text, re.MULTILINE),
                         ["windows-2025", "ubuntu-24.04"])

    def test_pinned_fixture_toolchain_installs_before_configure(self):
        for required in ("--no-deps", "--only-binary=:all:", "--require-hashes",
                         "scripts/native/test-fixture-python.lock",
                         "scripts/native/test-fixture-python-verify.py"):
            self.assertIn(required, self.text)
        self.assertLess(self.text.index("test-fixture-python.lock"),
                        self.text.index("name: Configure the complete production application"))
        self.assertNotIn("pip install cryptography", self.text)

    def test_pinned_static_openssl_stays_linux_only(self):
        self.assertNotIn("libssl-dev", self.text)
        self.assertIn("scripts/native/supply_openssl.py --pin "
                      "tests/native-portability/openssl-linux-pin.json", self.text)
        self.assertIn("-DOPENSSL_SSL_LIBRARY=", self.text)
        self.assertIn("-DOPENSSL_CRYPTO_LIBRARY=", self.text)

    def test_complete_test_graph_is_configured_and_executed_serially(self):
        for value in ("'cmake','--preset','native-release'",
                      "-DNICKEL_BUILD_UI=ON", "-DNICKEL_BUILD_TESTS=ON",
                      "-DNICKEL_TEST_TRANSPORT_FAULTS=ON",
                      "'--parallel','1'", "'--no-tests=error'",
                      "ci_evidence.py inventory", "ci_evidence.py verify-ctest",
                      "sha256sum --check tests/brand/functional-icons.sha256"):
            self.assertIn(value, self.text)
        self.assertNotIn("-DNICKEL_BUILD_TESTS=OFF", self.text)

    def test_release_preset_exists_and_builds_into_the_workflow_directory(self):
        presets = json.loads(PRESETS.read_text(encoding="utf-8"))
        names = {preset["name"] for preset in presets["configurePresets"]}
        self.assertIn("native-release", names)
        self.assertIn("'cmake','--build','--preset','native-release'", self.text)
        self.assertIn(".snow/native-build", self.text)

    def test_every_repository_path_referenced_by_the_workflow_exists(self):
        for match in REFERENCE.finditer(self.text):
            candidate = match.group(1).rstrip(".,;:)")
            if candidate.endswith("/"):
                candidate = candidate.rstrip("/")
            if not candidate or "$" in candidate or "*" in candidate:
                continue
            with self.subTest(path=candidate):
                self.assertTrue((ROOT / candidate).exists(), candidate)

    def test_ci_evidence_and_logs_are_preserved(self):
        self.assertIn("if: always()", self.text)
        self.assertIn("actions/upload-artifact@", self.text)
        self.assertIn("include-hidden-files: true", self.text)
        self.assertIn(".snow/ci-ctest.xml", self.text)


if __name__ == "__main__":
    unittest.main()