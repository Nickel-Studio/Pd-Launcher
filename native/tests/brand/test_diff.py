# SPDX-License-Identifier: AGPL-3.0-only
"""Synthetic raw Git parser tests; not actual owner-difference evidence."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("nickel_brand_diff", ROOT / "scripts/native/brand-diff.py")
DIFF = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DIFF)


def record(status: bytes, *paths: bytes) -> bytes:
    return b":100644 100644 " + b"a" * 40 + b" " + b"b" * 40 + b" " + status + b"\0" + b"\0".join(paths) + b"\0"


class GenuineDiffParserTests(unittest.TestCase):
    def test_add_modify_delete_paths_are_all_retained(self):
        raw = record(b"A", b"native/assets/brand/new.svg") + record(b"M", b"native/tests/brand/test_brand.py")
        raw += record(b"D", b"native/assets/brand/removed.svg")
        changes, paths = DIFF.parse_raw(raw)
        self.assertEqual([row["status"] for row in changes], ["A", "M", "D"])
        self.assertEqual(len(paths), 3)
        self.assertIn("native/assets/brand/removed.svg", paths)

    def test_both_rename_and_copy_endpoints_are_retained_even_outside_owner(self):
        raw = record(b"R100", b"src/assets/functional.png", b"native/assets/brand/stolen.png")
        raw += record(b"C095", b"README.md", b"docs/native/licenses/copied.md")
        _, paths = DIFF.parse_raw(raw)
        self.assertEqual(len(paths), 4)
        self.assertIn("src/assets/functional.png", paths)
        self.assertIn("README.md", paths)

    def test_nul_paths_preserve_spaces_and_utf8(self):
        raw = record(b"M", "native/assets/brand/space name.svg".encode("utf-8"))
        _, paths = DIFF.parse_raw(raw)
        self.assertEqual(paths, ["native/assets/brand/space name.svg"])

    def test_truncation_empty_diff_and_missing_endpoint_are_rejected(self):
        for raw in [b"", record(b"M", b"native/assets/brand/icon.svg")[:-1],
                    record(b"R100", b"native/assets/brand/one.svg"), b"not-git\0", record(b"M", b"")]:
            with self.subTest(raw=raw):
                with self.assertRaises(ValueError):
                    DIFF.parse_raw(raw)

    def test_short_revision_branch_and_fabricated_inventory_are_not_git_commits(self):
        for value in ["b19e28b", "HEAD", "brand-release", "inventory", "a" * 64]:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    DIFF.actual_commit(value)
        with patch.object(DIFF, "git", return_value=b"b" * 40 + b"\n"):
            with self.assertRaisesRegex(ValueError, "exact recorded commit"):
                DIFF.actual_commit("a" * 40)

    def test_genuine_delta_never_filters_unauthorized_changes_from_git_output(self):
        def output(*args):
            if args[0] == "diff-tree":
                return record(b"M", b"native/ui/Icons.cpp") + record(b"A", b"native/assets/brand/icon.svg")
            return b"c" * 40 + b"\n"
        with patch.object(DIFF, "actual_commit", side_effect=lambda value: value), patch.object(DIFF, "git", side_effect=output):
            document = DIFF.delta("a" * 40, "b" * 40)
        self.assertEqual(document["paths"], ["native/assets/brand/icon.svg", "native/ui/Icons.cpp"])
        self.assertEqual(document["kind"], "git-owner-diff")
        self.assertIn("--no-abbrev", document["gitArgv"])
        self.assertNotEqual(document["base"], document["candidate"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
