"""Host-independent regression for the CI smoke fixture generation binding.

The native CI publication gate (prepare_same_graph.prepare ->
generate_fixtures.verify_template) must accept only fixture templates bound to the
exact production/harness generation. These cases reproduce the predicate that
blocked CI run 45035683970 on Windows and Linux and keep every tamper path closed.
"""
import shutil
import sys
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/native-production-smoke"))
import evidence
import generate_fixtures

IMMUTABLE_MESSAGE = "fixtures are not an immutable template for this production/harness generation"


class FixtureGenerationBindingTests(unittest.TestCase):
    COMMIT = "1a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d"
    SOURCE_PIN = "b" * 64
    HARNESS_PIN = "c" * 64

    def setUp(self):
        self.scratch = evidence.checked_local(
            ROOT / ".snow/production-smoke/tests-scratch-ci-fixture-binding" / uuid.uuid4().hex[:16], writable=True)
        self.scratch.mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        shutil.rmtree(self.scratch, ignore_errors=True)

    def generate(self, *, bound=True):
        output = self.scratch / ("fixtures-" + uuid.uuid4().hex[:8])
        pins = {"source_commit": self.COMMIT, "source_sha256": self.SOURCE_PIN,
                "harness_sha256": self.HARNESS_PIN} if bound else {}
        return generate_fixtures.generate(output, "0.1.0", deep=True, **pins)

    def verify(self, document, *, source_commit=None, source_sha256=None, harness_sha256=None):
        manifest = Path(document["root"]) / "fixture-manifest.json"
        return generate_fixtures.verify_template(manifest, evidence.sha256(manifest),
            source_commit=source_commit or self.COMMIT, source_sha256=source_sha256 or self.SOURCE_PIN,
            harness_sha256=harness_sha256 or self.HARNESS_PIN)

    def test_current_bound_generation_passes_the_publish_predicate(self):
        document = self.generate()
        self.assertEqual(document["fixtureRole"], "immutable-template")
        self.assertEqual(document["sourceCommit"], self.COMMIT)
        self.assertEqual(document["sourceGenerationManifestSha256"], self.SOURCE_PIN)
        self.assertEqual(document["harnessManifestSha256"], self.HARNESS_PIN)
        result = self.verify(document)
        self.assertEqual(result["document"]["fixtureRole"], "immutable-template")
        self.assertGreater(result["fileCount"], 0)
        self.assertGreaterEqual(len(str(result["root"]).encode("utf-8")), 310)
        self.assertEqual(result["manifestSha256"], evidence.sha256(Path(result["root"]) / "fixture-manifest.json"))

    def test_unbound_ci_preparation_generation_is_rejected(self):
        document = self.generate(bound=False)
        self.assertEqual(document["fixtureRole"], "mutable-legacy-fixture")
        self.assertNotIn("sourceCommit", document)
        with self.assertRaises(evidence.EvidenceError) as raised:
            self.verify(document)
        self.assertEqual(str(raised.exception), IMMUTABLE_MESSAGE)

    def test_stale_generation_pins_are_rejected(self):
        document = self.generate()
        for pins in ({"source_commit": "0" * 40}, {"source_sha256": "0" * 64}, {"harness_sha256": "0" * 64}):
            with self.subTest(**pins):
                with self.assertRaises(evidence.EvidenceError) as raised:
                    self.verify(document, **pins)
                self.assertEqual(str(raised.exception), IMMUTABLE_MESSAGE)

    def test_changed_fixture_bytes_are_rejected(self):
        document = self.generate()
        target = next(row for row in document["files"] if row["format"] == "bytes")
        (Path(document["root"]) / target["path"]).write_bytes(b"tampered synthetic fixture bytes\n")
        with self.assertRaises(evidence.EvidenceError) as raised:
            self.verify(document)
        self.assertTrue(str(raised.exception).startswith("fixture template file size/hash changed: "),
                        str(raised.exception))

    def test_extra_fixture_file_is_rejected(self):
        document = self.generate()
        (Path(document["root"]) / "accepted/extra-unsanctioned.bin").write_bytes(b"extra\n")
        with self.assertRaises(evidence.EvidenceError) as raised:
            self.verify(document)
        self.assertEqual(str(raised.exception), "fixture template has an unlisted file or directory")

    def test_fixture_path_drift_is_rejected(self):
        document = self.generate()
        root = Path(document["root"])
        drifted = root.with_name("drifted-" + root.name)
        shutil.copytree(root, drifted)
        self.addCleanup(shutil.rmtree, drifted, ignore_errors=True)
        manifest = drifted / "fixture-manifest.json"
        with self.assertRaises(evidence.EvidenceError) as raised:
            generate_fixtures.verify_template(manifest, evidence.sha256(manifest),
                source_commit=self.COMMIT, source_sha256=self.SOURCE_PIN, harness_sha256=self.HARNESS_PIN)
        self.assertEqual(str(raised.exception), "fixture template manifest is outside its declared root")

    def test_generation_binding_requires_full_commit_and_pinned_hashes(self):
        with self.assertRaises(evidence.EvidenceError):
            generate_fixtures.generation_binding("short", self.SOURCE_PIN, self.HARNESS_PIN)
        with self.assertRaises(evidence.EvidenceError):
            generate_fixtures.generation_binding(self.COMMIT, "nothex", self.HARNESS_PIN)
        with self.assertRaises(evidence.EvidenceError):
            generate_fixtures.generate(self.scratch / "fixtures-partial", "0.1.0", deep=True,
                                       source_commit=self.COMMIT)
        binding = generate_fixtures.generation_binding(self.COMMIT, self.SOURCE_PIN, self.HARNESS_PIN)
        self.assertEqual(binding, {"sourceCommit": self.COMMIT,
                                   "sourceGenerationManifestSha256": self.SOURCE_PIN,
                                   "harnessManifestSha256": self.HARNESS_PIN})


if __name__ == "__main__":
    unittest.main()
