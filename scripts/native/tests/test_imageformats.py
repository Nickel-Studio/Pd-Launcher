"""WebP-only supply safety checks use private fixtures, never production acceptance."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location("nickel_imageformats_bootstrap", HERE / "bootstrap-deps.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class ImageFormatsTests(unittest.TestCase):
    def test_locked_webp_archive_and_bundled_license(self):
        lock = json.loads((HERE / "deps-lock.json").read_text(encoding="utf-8"))
        source = next(row for row in lock["sources"] if row["name"] == "qtimageformats")
        self.assertEqual(source["version"], "6.8.3")
        self.assertEqual(source["sha256"], "049bfb99845e4801672aca07c3c4fc4c140f932a3a33faa899419579e33ef1c8")
        bundled = source["bundledSources"][0]
        self.assertEqual((bundled["name"], bundled["version"], bundled["license"]), ("libwebp", "1.5.0", "BSD-3-Clause"))
        for key in ("sourceTreeSha256", "licenseSha256", "versionEvidenceSha256"):
            self.assertRegex(bundled[key], "^[a-f0-9]{64}$")
        self.assertIn("PATENTS", bundled["noticeFiles"])

    def test_only_real_webp_targets_and_official_install_directory(self):
        builder = bootstrap.Builder.__new__(bootstrap.Builder)
        with patch.object(builder, "cmake_project") as configure:
            builder.build_imageformats()
        configure.assert_called_once()
        args, kwargs = configure.call_args
        self.assertEqual(args[0], "qtimageformats")
        self.assertEqual(kwargs["build_targets"], ("QWebpPlugin", "QWebpPlugin_init"))
        self.assertEqual(kwargs["install_subdirectory"], "src/plugins/imageformats/webp")
        for required in ("-DINPUT_webp=qt", "-DFEATURE_webp=ON", "-DFEATURE_system_webp=OFF",
                         "-DFEATURE_tiff=OFF", "-DFEATURE_mng=OFF", "-DFEATURE_jasper=OFF"):
            self.assertIn(required, args[1])
        self.assertFalse(any("QT_BUILD_PLUGIN" in value for value in args[1]))

    def test_computed_features_reject_dynamic_or_unneeded_decoders(self):
        builder = bootstrap.Builder.__new__(bootstrap.Builder)
        builder.evidence = {}
        required = {"QT_FEATURE_webp:INTERNAL": "ON", "QT_FEATURE_system_webp:INTERNAL": "OFF",
                    "QT_FEATURE_tiff:INTERNAL": "OFF", "QT_FEATURE_mng:INTERNAL": "OFF",
                    "QT_FEATURE_jasper:INTERNAL": "OFF", "BUILD_SHARED_LIBS:BOOL": "OFF"}
        with tempfile.TemporaryDirectory() as temporary, patch.object(builder, "save_evidence"):
            directory = Path(temporary)
            cache = directory / "CMakeCache.txt"
            cache.write_text("\n".join(f"{key}={value}" for key, value in required.items()), encoding="ascii")
            builder.verify_imageformats_cache(directory)
            for key, value in required.items():
                with self.subTest(key=key):
                    invalid = dict(required)
                    invalid[key] = "OFF" if value == "ON" else "ON"
                    cache.write_text("\n".join(f"{name}={state}" for name, state in invalid.items()), encoding="ascii")
                    with self.assertRaisesRegex(RuntimeError, "WebP-only policy"):
                        builder.verify_imageformats_cache(directory)

    def test_source_tree_hash_is_sorted_and_content_bound(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "z").mkdir()
            (root / "z/a.txt").write_bytes(b"first")
            (root / "a.txt").write_bytes(b"second")
            original = bootstrap.source_tree_digest(root)
            (root / "z/a.txt").write_bytes(b"changed")
            self.assertNotEqual(bootstrap.source_tree_digest(root), original)
            (root / "z/a.txt").write_bytes(b"first")
            self.assertEqual(bootstrap.source_tree_digest(root), original)

    def test_private_work_and_build_roots_keep_install_prefix_canonical(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            options = argparse.Namespace(tools_root=root / "installed", work_root=root / "private", build_root=root / "short",
                                         target="fixture-static", only="imageformats", jobs=1, vs_root=root / "trusted-vs")
            with patch.object(bootstrap, "require_free_space"), patch.object(bootstrap, "developer_environment", return_value={"PATH": "fixture"}), \
                    patch.object(bootstrap.shutil, "which", return_value="fixture-tool"):
                builder = bootstrap.Builder(options)
            self.assertEqual(builder.prefix, (root / "installed/install/fixture-static").resolve())
            self.assertEqual(builder.log.parent, (root / "private/logs").resolve())
            self.assertEqual(builder.build_root, (root / "short").resolve())
            self.assertFalse((root / "installed/sources").exists())
            self.assertFalse((root / "installed/downloads").exists())

    def test_capacity_failure_precedes_any_file_or_tool_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            options = argparse.Namespace(tools_root=root / "installed", work_root=root / "private", build_root=root / "short",
                                         target="fixture-static", only="imageformats", jobs=1, vs_root=root / "trusted-vs")
            with patch.object(bootstrap, "require_free_space", side_effect=RuntimeError("low capacity")), \
                    patch.object(bootstrap, "developer_environment", side_effect=AssertionError("Must not invoke tools")):
                with self.assertRaisesRegex(RuntimeError, "low capacity"):
                    bootstrap.Builder(options)
            self.assertFalse(options.tools_root.exists())
            self.assertFalse(options.work_root.exists())
            self.assertFalse(options.build_root.exists())

    def test_budget_does_not_weaken_full_bootstrap_requirements(self):
        builder = bootstrap.Builder.__new__(bootstrap.Builder)
        builder.root = Path("installed")
        builder.work_root = Path("private")
        builder.build_root = Path("short")
        for scope, work, install in (("imageformats", 512 * 1024 ** 2, 64 * 1024 ** 2),
                                     ("all", 4 * 1024 ** 3, 4 * 1024 ** 3)):
            builder.options = argparse.Namespace(only=scope)
            with patch.object(bootstrap, "require_free_space") as check:
                builder.check_space()
            self.assertEqual(check.call_args_list[0].args, (builder.root, install))
            self.assertEqual(check.call_args_list[1].args, (builder.work_root, work))
            self.assertEqual(check.call_args_list[2].args, (builder.build_root, work))

    def test_free_space_failure_does_not_create_missing_parent(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "missing/nested"
            with patch.object(bootstrap.shutil, "disk_usage", return_value=shutil._ntuple_diskusage(100, 100, 0)):
                with self.assertRaisesRegex(RuntimeError, "before writing"):
                    bootstrap.require_free_space(destination, 1)
            self.assertFalse(destination.parent.exists())


if __name__ == "__main__":
    unittest.main()
