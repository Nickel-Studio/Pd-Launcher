#!/usr/bin/env python3
"""Bounded metadata verifier tests; native cleanup behavior stays in acceptance.cpp."""
import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
import zipfile

import verify_snapshot as verifier


ROOT = Path(__file__).resolve().parents[2]
TEMP = ROOT / ".snow/posix-cleanup-acceptance/tmp"


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


class ManifestZipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="owner-verifier-unit-", dir=TEMP)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.content = b"bounded\n"
        self.member = "native/input.cpp"
        self.manifest = {"files": [{"path": self.member, "bytes": len(self.content), "sha256": sha(self.content)}]}
        self.manifest_raw = (json.dumps(self.manifest) + "\n").encode("ascii")
        (self.root / "manifest.json").write_bytes(self.manifest_raw)

    def bundle(self, pairs):
        with zipfile.ZipFile(self.root / "inputs.zip", "w") as archive:
            for name, raw in pairs:
                archive.writestr(name, raw)

    def verify(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return verifier.verify_manifest_zip(self.root, "manifest.json", "inputs.zip", "files", 1)

    def test_exact_sources_and_embedded_manifest(self):
        self.bundle([(self.member, self.content), ("manifest.json", self.manifest_raw)])
        _, files = self.verify()
        self.assertEqual(files[self.member]["sha256"], sha(self.content))

    def test_missing_embedded_manifest_rejected(self):
        self.bundle([(self.member, self.content)])
        with self.assertRaisesRegex(RuntimeError, "exact sources"):
            self.verify()

    def test_extra_member_rejected(self):
        self.bundle([(self.member, self.content), ("manifest.json", self.manifest_raw), ("extra.txt", b"x")])
        with self.assertRaisesRegex(RuntimeError, "exact sources"):
            self.verify()

    def test_changed_source_rejected(self):
        self.bundle([(self.member, b"changed\n"), ("manifest.json", self.manifest_raw)])
        with self.assertRaisesRegex(RuntimeError, "member mismatch"):
            self.verify()

    def test_changed_embedded_manifest_rejected(self):
        self.bundle([(self.member, self.content), ("manifest.json", b"{}\n")])
        with self.assertRaises(RuntimeError):
            self.verify()

    def test_duplicate_manifest_path_rejected(self):
        value = copy.deepcopy(self.manifest)
        value["files"].append(value["files"][0])
        (self.root / "manifest.json").write_text(json.dumps(value))
        self.bundle([(self.member, self.content), ("manifest.json", self.manifest_raw)])
        with self.assertRaisesRegex(RuntimeError, "duplicate path"):
            self.verify()

    def test_traversal_manifest_path_rejected(self):
        value = copy.deepcopy(self.manifest)
        value["files"][0]["path"] = "../outside.cpp"
        (self.root / "manifest.json").write_text(json.dumps(value))
        self.bundle([(self.member, self.content), ("manifest.json", self.manifest_raw)])
        with self.assertRaisesRegex(RuntimeError, "unsafe"):
            self.verify()


class CanonicalPublicationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get("NICKEL_POSIX_SOURCE_ROOT")
        if not source:
            raise unittest.SkipTest("Set NICKEL_POSIX_SOURCE_ROOT to a completed owner-local canonical fixture for publication regression cases")
        cls.source = Path(source).resolve(strict=True)
        cls.source.relative_to(ROOT / ".snow/posix-cleanup-acceptance/incoming")
        cls.manifest = json.loads((cls.source / "snapshot.json").read_text())

    def test_exact_actual_publication(self):
        with contextlib.redirect_stdout(io.StringIO()):
            manifest = verifier.verify_root(self.source, ROOT, "e5f625881ee99936f5696a7df0cef32aff5873bbaa429aa2174d15774e17c9d2")
            verifier.verify_canonical_source1(self.source, manifest)

    def test_inherited_runtime_pass_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["metadata"]["runtimePassed"] = True
        with self.assertRaisesRegex(RuntimeError, "inherit"):
            verifier.verify_canonical_source1(self.source, manifest)

    def test_security_pass_publication_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["metadata"]["securityQualification"] = "PASS"
        with self.assertRaisesRegex(RuntimeError, "inherit"):
            verifier.verify_canonical_source1(self.source, manifest)

    def test_changed_retained_source_binding_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["files"]["native/core/diagnostics/junk.cpp"]["sha256"] = "0" * 64
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "13 retained"):
                verifier.verify_canonical_source1(self.source, manifest)


if __name__ == "__main__":
    unittest.main()
