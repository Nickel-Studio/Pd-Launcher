#!/usr/bin/env python3
"""SDK supplier regressions only; no fixture accepts the launcher or a release."""
from __future__ import annotations

import json
import io
import tarfile
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import audit_support as support

if os.name == "nt":
    with mock.patch.dict(sys.modules, {"fcntl": SimpleNamespace(LOCK_EX=2, LOCK_NB=4, LOCK_UN=8)}):
        import linux_sdk
else:
    import linux_sdk


class SupplierTests(unittest.TestCase):
    def setUp(self):
        support.capacity(support.OWNED, 1024 * 1024)
        base = support.OWNED / "scratch"
        base.mkdir(exist_ok=True)
        lease = tempfile.TemporaryDirectory(prefix="sdk-tools-", dir=base)
        self.addCleanup(lease.cleanup)
        self.root = Path(lease.name)
        self.marker = self.root / ".nickel-portability-owned.json"
        info = self.root.stat()
        self.payload = {"kind": "portability-owned-linux-sdk", "owner": str(support.OWNED),
                        "root": str(self.root), "token": "fixture-token", "uid": info.st_uid,
                        "device": info.st_dev, "inode": info.st_ino, "limitBytes": linux_sdk.LIMIT}
        self.marker.write_text(json.dumps(self.payload), encoding="ascii")
        self.sdk = linux_sdk.Sdk.__new__(linux_sdk.Sdk)
        self.sdk.root = self.root
        self.sdk.prefix = self.root / "prefix"
        self.sdk.root_identity = (info.st_dev, info.st_ino, info.st_uid)
        self.sdk.marker_bytes = self.marker.read_bytes()
        self.sdk.evidence = {"commands": [], "completeProductVerified": False, "passed": False}
        self.sdk.environment = {}
        self.sdk.save = mock.Mock()
        self.sdk.command = mock.Mock()
        self.sdk.guard = mock.Mock(return_value={"chargedBytes": 0})
        qt = self.root / "qt/6.8.3/gcc_64"
        for name in ("Qt6", "Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Svg", "Qt6Test"):
            path = qt / "lib/cmake" / name / (name + "Config.cmake")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture configuration only\n", encoding="ascii")

    def construct(self, guard_error=None):
        with mock.patch.object(linux_sdk.sys, "platform", "linux"), \
                mock.patch.object(linux_sdk.os, "pidfd_open", create=True), \
                mock.patch.object(linux_sdk.os, "getuid", return_value=self.payload["uid"], create=True), \
                mock.patch.object(linux_sdk.Sdk, "guard", return_value={}, side_effect=guard_error), \
                mock.patch.object(linux_sdk.Sdk, "save"):
            return linux_sdk.Sdk(self.root, "fixture-token", self.root / "result.json")

    def test_marker_identity_is_bound_before_any_supply(self):
        for key in ("kind", "owner", "root", "token", "uid", "device", "inode", "limitBytes"):
            with self.subTest(key=key):
                payload = dict(self.payload)
                payload[key] = "foreign" if isinstance(payload[key], str) else payload[key] + 1
                self.marker.write_text(json.dumps(payload), encoding="ascii")
                with self.assertRaises(support.AuditSafetyError):
                    self.construct()
        self.assertFalse((self.root / "home").exists())
        self.assertFalse((self.root / "result.json").exists())

    def test_nonobject_marker_fails_closed(self):
        self.marker.write_text("[]", encoding="ascii")
        with self.assertRaises(support.AuditSafetyError):
            self.construct()

    def test_existing_evidence_is_not_replaced(self):
        output = self.root / "result.json"
        output.write_bytes(b"old evidence")
        with self.assertRaises(support.AuditSafetyError):
            self.construct()
        self.assertEqual(output.read_bytes(), b"old evidence")

    def test_qt_global_config_exact_modules_and_icu(self):
        self.sdk.qt("https://ftp.fau.de/qtproject")
        argv = self.sdk.command.call_args_list[0].args[0]
        env = self.sdk.command.call_args_list[0].kwargs["env"]
        self.assertNotIn("--config", argv)
        self.assertEqual(env["AQT_CONFIG"], str(self.root / "aqt.ini"))
        self.assertEqual(argv[argv.index("--archives") + 1:argv.index("--modules")], ["qtbase", "qtsvg", "icu"])
        self.assertIn("qtimageformats", argv)
        self.assertIn("6.8.3", argv)
        self.assertIn("linux_gcc_64", argv)
        self.assertNotIn("qtdeclarative", argv)
        self.assertNotIn("qtwebengine", argv)
        self.assertIn("ignore_hash: False", (self.root / "aqt.ini").read_text())
        self.assertFalse(self.sdk.evidence["completeProductVerified"])

    def test_icu_only_supply_does_not_redownload_qtbase(self):
        self.sdk.qt("https://ftp.fau.de/qtproject", ["icu"])
        argv = self.sdk.command.call_args_list[0].args[0]
        self.assertNotIn("qtbase", argv)
        self.assertNotIn("--modules", argv)
        self.assertEqual(self.sdk.evidence["qtSelectedArchives"], ["icu"])
        self.assertEqual(self.sdk.command.call_args_list[-1].args[0][1:], ["-query", "QT_VERSION"])

    def test_altered_aqt_config_is_preserved(self):
        config = self.root / "aqt.ini"
        config.write_bytes(b"foreign or insecure config")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.qt()
        self.sdk.command.assert_not_called()
        self.assertEqual(config.read_bytes(), b"foreign or insecure config")

    def test_forbidden_duplicate_and_empty_archive_sets_are_rejected(self):
        for archives in (["qtdeclarative"], ["qtbase", "qtbase"], []):
            with self.subTest(archives=archives), self.assertRaises(support.AuditSafetyError):
                self.sdk.qt(archives=archives)
        self.sdk.command.assert_not_called()
        self.assertFalse((self.root / "downloads").exists())

    def test_wrong_platform_package_set_is_rejected_before_write(self):
        path = self.root / "index.json"
        path.write_text(json.dumps({"command": {"passed": True, "stdout": json.dumps({"exitCode": 0, "packages": []})}}), encoding="ascii")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.platform_runtime(path)
        self.sdk.command.assert_not_called()
        self.assertFalse((self.root / "runtime").exists())

    def test_platform_package_digest_mismatch_never_extracts(self):
        names = ("libxkbcommon-x11-0", "libxcb-cursor0", "libxcb-icccm4", "libxcb-image0",
                 "libxcb-keysyms1", "libxcb-render-util0", "libxcb-xkb1", "libxcb-util1")
        packages = [{"Package": name, "Architecture": "amd64", "Version": "1-fixture",
                     "Filename": "pool/" + name + ".deb", "Size": "3", "SHA256": "f" * 64} for name in names]
        path = self.root / "index.json"
        path.write_text(json.dumps({"command": {"passed": True, "stdout": json.dumps({"exitCode": 0, "packages": packages})}}), encoding="ascii")
        self.sdk.command.side_effect = lambda argv, **kwargs: (kwargs["cwd"] / (names[0] + ".deb")).write_bytes(b"bad")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.platform_runtime(path)
        self.assertEqual(self.sdk.command.call_count, 1)
        self.assertEqual(self.sdk.command.call_args.args[0][0], "apt-get")
        self.assertEqual(list((self.root / "runtime").iterdir()), [])

    def test_replaced_root_identity_is_rejected(self):
        self.sdk.root_identity = (-1, -1, -1)
        with self.assertRaises(support.AuditSafetyError):
            linux_sdk.Sdk.guard(self.sdk)

    @unittest.skipIf(os.name == "nt", "Actual Linux allocated-size quota test")
    def test_latest_physical_capacity_is_recorded_even_when_blocked(self):
        self.sdk.evidence = {}
        with mock.patch.object(linux_sdk.shutil, "disk_usage", side_effect=[SimpleNamespace(free=linux_sdk.RESERVE_C - 1),
                                                                         SimpleNamespace(free=linux_sdk.RESERVE_D + 1)]):
            with self.assertRaises(support.AuditSafetyError):
                linux_sdk.Sdk.guard(self.sdk)
        self.assertEqual(self.sdk.evidence["latestCapacity"]["freeCBytes"], linux_sdk.RESERVE_C - 1)
        self.assertGreater(self.sdk.evidence["latestCapacity"]["chargedBytes"], 0)

    @unittest.skipIf(os.name == "nt", "Actual Linux flock test")
    def test_sdk_writer_is_exclusive(self):
        with self.sdk.writer():
            with self.assertRaises(BlockingIOError):
                with self.sdk.writer():
                    self.fail("Concurrent supplier must not enter")

    def test_constructor_capacity_failure_creates_no_home_or_evidence(self):
        with self.assertRaises(support.AuditSafetyError):
            self.construct(support.AuditSafetyError("capacity blocked"))
        self.assertFalse((self.root / "home").exists())
        self.assertFalse((self.root / "result.json").exists())

    def test_qt_capacity_failure_creates_no_config_or_downloads(self):
        self.sdk.guard.side_effect = support.AuditSafetyError("capacity blocked")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.qt()
        self.assertFalse((self.root / "aqt.ini").exists())
        self.assertFalse((self.root / "downloads").exists())
        self.sdk.command.assert_not_called()

    def test_writer_capacity_failure_creates_no_lock(self):
        self.sdk.guard.side_effect = support.AuditSafetyError("capacity blocked")
        with self.assertRaises(support.AuditSafetyError):
            with self.sdk.writer():
                self.fail("Blocked supplier must not enter")
        self.assertFalse((self.root / ".sdk-writer.lock").exists())

    def test_platform_capacity_failure_creates_no_runtime(self):
        names = ("libxkbcommon-x11-0", "libxcb-cursor0", "libxcb-icccm4", "libxcb-image0",
                 "libxcb-keysyms1", "libxcb-render-util0", "libxcb-xkb1", "libxcb-util1")
        path = self.root / "index.json"
        path.write_text(json.dumps({"command": {"passed": True, "stdout": json.dumps({
            "exitCode": 0, "packages": [{"Package": name} for name in names]})}}), encoding="ascii")
        self.sdk.guard.side_effect = support.AuditSafetyError("capacity blocked")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.platform_runtime(path)
        self.assertFalse((self.root / "runtime").exists())
        self.assertFalse((self.root / "runtime-downloads").exists())
        self.sdk.command.assert_not_called()

    def test_cmake_capacity_failure_creates_no_build(self):
        (self.root / "build").mkdir()
        self.sdk.guard.side_effect = support.AuditSafetyError("capacity blocked")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.cmake("fixture", self.root, [])
        self.assertFalse((self.root / "build/fixture").exists())
        self.sdk.command.assert_not_called()

    def test_completed_child_after_slow_guard_is_not_timed_out(self):
        self.sdk.output = self.root / "actual-command.json"
        self.sdk.control = self.root / "control.json"
        child = mock.Mock(pid=12345)
        child.poll.side_effect = [None, 0]
        child.wait.return_value = 0
        with mock.patch.object(linux_sdk.subprocess, "Popen", return_value=child), \
                mock.patch.object(linux_sdk, "proc_identity", return_value={"pid": 12345}), \
                mock.patch.object(linux_sdk.time, "monotonic", side_effect=[0, 200]):
            linux_sdk.Sdk.command(self.sdk, ["fixture-completed"], seconds=1)
        record = self.sdk.evidence["commands"][-1]
        self.assertEqual(record["state"], "FINISHED")
        self.assertEqual(record["exitCode"], 0)
        self.assertTrue(record["passed"])

    def archive_fixture(self):
        cache = self.root / "archive-cache"
        cache.mkdir()
        self.sdk.archive_root = cache
        (self.root / "sources").mkdir()
        self.sdk.evidence["sources"] = []
        archive = cache / "fixture.tar"
        data = b"bounded archive fixture\n"
        with tarfile.open(archive, "w") as bundle:
            member = tarfile.TarInfo("fixture-source/member.txt")
            member.size = len(data)
            bundle.addfile(member, io.BytesIO(data))
        entry = {"name": "fixture", "archive": archive.name, "sourceDirectory": "fixture-source",
                 "sha256": support.sha256(archive)}
        return entry, archive

    def test_explicit_physical_archive_root_is_reused_read_only(self):
        entry, archive = self.archive_fixture()
        before = archive.read_bytes()
        source = self.sdk.source(entry)
        self.assertEqual((source / "member.txt").read_bytes(), b"bounded archive fixture\n")
        self.assertEqual(archive.read_bytes(), before)
        self.assertEqual(self.sdk.evidence["sources"][0]["archiveRoot"], str(self.sdk.archive_root))
        self.sdk.command.assert_not_called()

    def test_archive_hash_mismatch_is_rejected_before_extraction(self):
        entry, archive = self.archive_fixture()
        entry["sha256"] = "f" * 64
        before = archive.read_bytes()
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.source(entry)
        self.assertEqual(archive.read_bytes(), before)
        self.assertEqual(list((self.root / "sources").iterdir()), [])
        self.sdk.command.assert_not_called()

    def test_explicit_archive_root_does_not_bypass_path_rejection(self):
        entry, archive = self.archive_fixture()
        with mock.patch.object(linux_sdk, "plain_path", side_effect=support.AuditSafetyError("reparse path")), \
                mock.patch.object(linux_sdk.tarfile, "open") as extract:
            with self.assertRaises(support.AuditSafetyError):
                self.sdk.source(entry)
            extract.assert_not_called()
        self.assertTrue(archive.is_file())
        self.assertEqual(list((self.root / "sources").iterdir()), [])

    def test_cmake_preserves_trycompile_trees_without_weakening_inventory(self):
        (self.root / "build").mkdir()
        cache = self.root / "build" / "fixture" / "CMakeCache.txt"

        def command(argv, **kwargs):
            if "-S" in argv:
                cache.write_bytes(b"fixture configuration\n")

        self.sdk.command.side_effect = command
        self.sdk.cmake("fixture", self.root, [])
        argv = self.sdk.command.call_args_list[0].args[0]
        self.assertEqual(argv[:2], ["cmake", "--debug-trycompile"])
        self.assertEqual(self.sdk.command.call_count, 3)
        self.assertEqual(self.sdk.evidence["buildCaches"][0]["sha256"], support.sha256(cache))


if __name__ == "__main__":
    unittest.main(verbosity=2)
