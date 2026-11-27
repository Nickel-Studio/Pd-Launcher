#!/usr/bin/env python3
"""Synthetic immutable Git capture contracts; not complete application acceptance."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import audit_support as support
import git_source


class GitSourceTests(unittest.TestCase):
    def setUp(self):
        base = support.OWNED / "scratch"
        support.capacity(base, 1024 * 1024)
        base.mkdir(exist_ok=True)
        lease = tempfile.TemporaryDirectory(prefix="git-source-tests-", dir=base)
        self.addCleanup(lease.cleanup)
        self.root = Path(lease.name) / "capture"
        self.commit = "a" * 40
        self.blobs = {key: b"bounded fixture\n" for key in git_source.REQUIRED}
        self.blobs["native/metadata.json"] = json.dumps({"schemaVersion": 1, "displayName": "Palladium",
                                                        "executableName": "pd-launcher"}).encode("ascii")
        self.blobs["native/app/CMakeLists.txt"] = b"qt_add_executable(ni-launcher MANUAL_FINALIZATION main.cpp)\n"
        self.entries = {key: {"bytes": len(raw), "gitBlob": hashlib.sha1(b"blob " + str(len(raw)).encode("ascii")
                                                                         + b"\0" + raw).hexdigest(),
                              "gitMode": "100644", "sha256": hashlib.sha256(raw).hexdigest()}
                        for key, raw in self.blobs.items()}
        patch = mock.patch.object(git_source, "commit_inputs", return_value=("b" * 40, self.entries, self.blobs))
        patch.start()
        self.addCleanup(patch.stop)
        guard = mock.patch.object(git_source, "physical_guard")
        self.guard = guard.start()
        self.addCleanup(guard.stop)

    def captured(self):
        path = git_source.capture(self.commit, self.root)
        return path, support.sha256(path)

    def test_capture_preserves_raw_git_bytes_and_actual_target(self):
        path, digest = self.captured()
        freeze = git_source.GitSourceFreeze(path, digest, self.commit)
        self.assertEqual((self.root / "native/metadata.json").read_bytes(), self.blobs["native/metadata.json"])
        self.assertEqual(freeze.evidence["metadata"]["displayName"], "Palladium")
        self.assertEqual(freeze.evidence["productTarget"], "ni-launcher")
        self.assertFalse(freeze.evidence["completeProductVerified"])
        self.assertTrue(freeze.evidence["sourceVerifiedBefore"])

    def test_existing_capture_is_preserved_without_writes(self):
        self.root.mkdir()
        sentinel = self.root / "keep"
        sentinel.write_bytes(b"preserve")
        with self.assertRaises(support.AuditSafetyError):
            git_source.capture(self.commit, self.root)
        self.assertEqual(sentinel.read_bytes(), b"preserve")
        self.guard.assert_not_called()

    def test_capacity_failure_precedes_root_and_marker(self):
        self.guard.side_effect = support.AuditSafetyError("capacity")
        with self.assertRaises(support.AuditSafetyError):
            git_source.capture(self.commit, self.root)
        self.assertFalse(self.root.exists())

    def test_manifest_pin_and_commit_are_both_required(self):
        path, digest = self.captured()
        for sha, commit in (("", self.commit), ("f" * 64, self.commit), (digest, "c" * 40)):
            with self.subTest(sha=sha, commit=commit), self.assertRaises(support.AuditSafetyError):
                git_source.GitSourceFreeze(path, sha, commit)

    def test_changed_recipe_is_rejected(self):
        path, digest = self.captured()
        freeze = git_source.GitSourceFreeze(path, digest, self.commit)
        (self.root / "scripts/native/deps-lock.json").write_bytes(b"changed")
        with self.assertRaises(support.AuditSafetyError):
            freeze.check()

    def test_changed_member_and_unlisted_file_are_rejected(self):
        path, digest = self.captured()
        freeze = git_source.GitSourceFreeze(path, digest, self.commit)
        foreign = self.root / "native/foreign.cpp"
        foreign.write_bytes(b"keep")
        with self.assertRaises(support.AuditSafetyError):
            freeze.postcheck()
        self.assertEqual(foreign.read_bytes(), b"keep")

    def test_completion_record_change_is_rejected(self):
        path, digest = self.captured()
        freeze = git_source.GitSourceFreeze(path, digest, self.commit)
        (self.root / git_source.COMPLETE).write_bytes(b"{}")
        with self.assertRaises(support.AuditSafetyError):
            freeze.check()

    def test_capture_rejects_unbound_product_metadata_before_write(self):
        self.blobs["native/metadata.json"] = b'{"schemaVersion":1,"executableName":"../wrong"}'
        with self.assertRaises(support.AuditSafetyError):
            git_source.capture(self.commit, self.root)
        self.assertFalse(self.root.exists())

    def test_mutable_ref_is_rejected_before_git_read(self):
        with mock.patch.object(git_source, "git") as command:
            for commit in ("main", "HEAD", "a" * 7, "", None):
                with self.subTest(commit=commit), self.assertRaises(support.AuditSafetyError):
                    original_inputs(support.OWNED.parents[1], commit)
            command.assert_not_called()

    def test_tracked_policy_build_is_not_a_product_source_input(self):
        tree = b"".join(("100644 blob " + row["gitBlob"] + " " + str(row["bytes"]) + "\t" + key + "\0").encode("utf-8")
                        for key, row in self.entries.items())
        tree += b"100644 blob " + b"f" * 40 + b" 3\tnative/tests/minecraft/.build-policy/old.exe\0"
        batch = b"".join((row["gitBlob"] + " blob " + str(row["bytes"]) + "\n").encode("ascii")
                         + self.blobs[key] + b"\n" for key, row in self.entries.items())
        with mock.patch.object(git_source, "git", side_effect=[b"commit\n", b"b" * 40 + b"\n", tree, batch]) as command:
            _, entries, blobs = original_inputs(support.OWNED.parents[1], self.commit)
        self.assertEqual(entries, self.entries)
        self.assertEqual(blobs, self.blobs)
        self.assertNotIn(b"f" * 40, command.call_args.kwargs["data"])


original_inputs = git_source.commit_inputs
if __name__ == "__main__":
    unittest.main(verbosity=2)
