#!/usr/bin/env python3
"""D supplier/source-pin regressions only; never launcher acceptance."""
from __future__ import annotations

import copy
import json
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
        import linux_d_sdk
else:
    import linux_d_sdk

from dependency_source import SourceFreeze


class DriveTests(unittest.TestCase):
    def setUp(self):
        base = support.OWNED / "scratch"
        support.capacity(base, 1024 * 1024)
        base.mkdir(exist_ok=True)
        lease = tempfile.TemporaryDirectory(prefix="d-sdk-tools-", dir=base)
        self.addCleanup(lease.cleanup)
        self.root = Path(lease.name)
        self.sdk = linux_d_sdk.DriveSdk.__new__(linux_d_sdk.DriveSdk)
        self.sdk.root = self.root
        self.sdk.qt_root = self.root / "qt-retained"
        self.sdk.qt_prefix = self.sdk.qt_root
        self.sdk.evidence = {"commands": [], "completeProductVerified": False}
        self.sdk.guard = mock.Mock(side_effect=support.AuditSafetyError("capacity blocked"))
        self.sdk.command = mock.Mock()

    def test_physical_c_reserve_is_not_guest_virtual_free(self):
        with mock.patch.object(linux_d_sdk.shutil, "disk_usage", side_effect=[SimpleNamespace(free=linux_d_sdk.RESERVE_C - 1),
                                                                            SimpleNamespace(free=10 * linux_d_sdk.LIMIT)]):
            with self.assertRaises(support.AuditSafetyError):
                linux_d_sdk.DriveSdk.host_preflight()

    def test_d_budget_is_reserved_before_root_write(self):
        with mock.patch.object(linux_d_sdk.shutil, "disk_usage", side_effect=[SimpleNamespace(free=2 * linux_d_sdk.RESERVE_C),
                                                                            SimpleNamespace(free=linux_d_sdk.RESERVE_D + linux_d_sdk.LIMIT - 1)]):
            with self.assertRaises(support.AuditSafetyError):
                linux_d_sdk.DriveSdk.host_preflight(linux_d_sdk.LIMIT)

    def test_product_low_space_has_no_build_or_command(self):
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.product()
        self.assertFalse((self.root / "build/product").exists())
        self.sdk.command.assert_not_called()

    def test_d_supplier_rejects_qt_duplication(self):
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.qt()
        self.sdk.command.assert_not_called()

    def test_platform_low_space_has_no_runtime_or_download(self):
        names = ("libxkbcommon-x11-0", "libxcb-cursor0", "libxcb-icccm4", "libxcb-image0",
                 "libxcb-keysyms1", "libxcb-render-util0", "libxcb-xkb1", "libxcb-util1")
        path = self.root / "index.json"
        path.write_text(json.dumps({"command": {"passed": True, "stdout": json.dumps({
            "exitCode": 0, "packages": [{"Package": name, "Size": "3"} for name in names]})}}), encoding="ascii")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.platform_runtime(path)
        self.assertFalse((self.root / "runtime").exists())
        self.assertFalse((self.root / "runtime-downloads").exists())
        self.sdk.command.assert_not_called()

    def test_official_openssl_pin_is_cross_bound(self):
        pin = linux_d_sdk.load_openssl_pin(support.OWNED / "openssl-linux-pin.json")
        self.assertEqual(pin["version"], "3.5.9")
        self.assertFalse(pin["signatureVerified"])
        self.assertFalse(pin["completeProductVerified"])

    def test_altered_openssl_hash_url_size_fail_before_download(self):
        original = json.loads((support.OWNED / "openssl-linux-pin.json").read_bytes())
        for name in ("metadataEvidence", "verificationEvidence"):
            record = original[name]
            destination = self.root / record["path"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((support.OWNED / record["path"]).read_bytes())
        for key, value in (("sha256", "f" * 64), ("url", "https://example.invalid/openssl.tar.gz"),
                           ("bytes", original["bytes"] + 1), ("version", "4.1.0")):
            with self.subTest(key=key):
                pin = copy.deepcopy(original)
                pin[key] = value
                pin_path = self.root / ("altered-" + key + ".json")
                pin_path.write_text(json.dumps(pin), encoding="ascii")
                with self.assertRaises(support.AuditSafetyError):
                    self.sdk.openssl(pin_path)
                self.sdk.command.assert_not_called()

    def test_source_pin_missing_identity_is_rejected(self):
        with self.assertRaises(support.AuditSafetyError):
            SourceFreeze(self.root / "snapshot.json", "")

    def runtime_fixture(self):
        names = ("libxkbcommon-x11-0", "libxcb-cursor0", "libxcb-icccm4", "libxcb-image0",
                 "libxcb-keysyms1", "libxcb-render-util0", "libxcb-xkb1", "libxcb-util1")
        data = b"deb"
        digest = support.hashlib.sha256(data).hexdigest()
        packages = [{"Package": name, "Version": "1.0", "Architecture": "amd64", "Size": str(len(data)),
                     "SHA256": digest, "Filename": "pool/" + name + ".deb"} for name in names]
        index = self.root / "runtime-index.json"
        index.write_text(json.dumps({"command": {"passed": True, "stdout": json.dumps({
            "exitCode": 0, "packages": packages})}}), encoding="ascii")
        runtime, downloads = self.root / "runtime", self.root / "runtime-downloads"
        runtime.mkdir()
        downloads.mkdir()
        self.sdk.guard = mock.Mock(return_value={"chargedBytes": 0})
        self.sdk.save = mock.Mock()
        self.sdk.measure = mock.Mock(return_value={"chargedBytes": 0})
        self.sdk.token = "fixture-token"
        self.sdk.marker_bytes = b"{}"
        self.sdk.freeze = SimpleNamespace(expected_sha256="a" * 64)
        prior = {"kind": "actual-linux-d-sdk-and-product-build", "state": "FAILED_OR_BLOCKED",
                 "root": str(self.root), "token": self.sdk.token,
                 "markerSha256": support.hashlib.sha256(self.sdk.marker_bytes).hexdigest(),
                 "dependencySource": {"manifestSha256": self.sdk.freeze.expected_sha256},
                 "platformRuntime": {"prefix": str(runtime), "indexSha256": support.sha256(index), "packages": []}}
        previous = self.root / "previous-runtime.json"
        previous.write_text(json.dumps(prior), encoding="ascii")
        return index, previous, packages, downloads, runtime, data

    def test_runtime_resume_requires_previous_pin_before_command(self):
        index, previous, packages, downloads, runtime, data = self.runtime_fixture()
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.platform_runtime(index)
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.platform_runtime(index, previous, "f" * 64)
        self.sdk.command.assert_not_called()
        self.assertEqual(list(runtime.iterdir()), [])

    def test_runtime_resume_rejects_unreceipted_tree_and_wrong_archive(self):
        index, previous, packages, downloads, runtime, data = self.runtime_fixture()
        archive = downloads / Path(packages[0]["Filename"]).name
        archive.write_bytes(b"bad")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.platform_runtime(index, previous, support.sha256(previous))
        self.assertEqual(archive.read_bytes(), b"bad")
        self.sdk.command.assert_not_called()
        archive.write_bytes(data)
        foreign = runtime / "foreign"
        foreign.write_bytes(b"keep")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.platform_runtime(index, previous, support.sha256(previous))
        self.assertEqual(foreign.read_bytes(), b"keep")
        self.sdk.command.assert_not_called()

    def test_runtime_resume_reuses_verified_download_without_duplication(self):
        index, previous, packages, downloads, runtime, data = self.runtime_fixture()
        retained = downloads / Path(packages[0]["Filename"]).name
        retained.write_bytes(data)
        calls = []

        def command(argv, **kwargs):
            calls.append(argv)
            if argv[:2] == ["apt-get", "download"]:
                name = argv[2].partition("=")[0]
                (downloads / (name + ".deb")).write_bytes(data)
            elif argv[0] == "dpkg-deb":
                (runtime / Path(argv[2]).stem).write_bytes(b"unpacked")
            elif argv[0] == "ldd":
                log = self.root / "ldd.log"
                log.write_text("libQt6Core.so.6 => retained Qt\n", encoding="ascii")
                self.sdk.evidence["commands"].append({"log": str(log)})

        self.sdk.command = command
        self.sdk.platform_runtime(index, previous, support.sha256(previous))
        self.assertEqual(retained.read_bytes(), data)
        downloads_run = [argv[2] for argv in calls if argv[0] == "apt-get"]
        self.assertNotIn(packages[0]["Package"] + "=1.0", downloads_run)
        self.assertEqual(len(downloads_run), 7)
        self.assertEqual(len(self.sdk.evidence["platformRuntime"]["packages"]), 8)
        self.assertTrue(self.sdk.evidence["platformRuntime"]["lddClosureChecked"])

    def test_drive_guard_records_latest_physical_failure(self):
        self.sdk.qt_root.mkdir()
        for root in (self.root, self.sdk.qt_root):
            (root / ".nickel-portability-owned.json").write_bytes(b"{}")
        self.sdk.marker_bytes = self.sdk.qt_marker_bytes = b"{}"
        self.sdk.root_identity = (self.root.stat().st_dev, self.root.stat().st_ino, self.root.stat().st_uid)
        self.sdk.qt_identity = (self.sdk.qt_root.stat().st_dev, self.sdk.qt_root.stat().st_ino, self.sdk.qt_root.stat().st_uid)
        self.sdk.freeze = SimpleNamespace(check=mock.Mock())
        self.sdk.measure = mock.Mock(return_value={"chargedBytes": 10})
        with mock.patch.object(linux_d_sdk.shutil, "disk_usage", side_effect=[
                SimpleNamespace(free=linux_d_sdk.RESERVE_C - 1), SimpleNamespace(free=linux_d_sdk.LIMIT)]):
            with self.assertRaises(support.AuditSafetyError):
                linux_d_sdk.DriveSdk.guard(self.sdk)
        self.assertEqual(self.sdk.evidence["latestCapacity"]["freeCBytes"], linux_d_sdk.RESERVE_C - 1)

    def test_gpg_status_requires_official_primary_and_rejects_bad_status(self):
        primary = linux_d_sdk.OPENSSL_SIGNING_FINGERPRINT
        good = ("[GNUPG:] GOODSIG 1234567890ABCDEF OpenSSL\n"
                "[GNUPG:] VALIDSIG " + "A" * 40 + " 2026-09-29 1790680000 0 4 0 1 10 00 " + primary + "\n")
        checked = linux_d_sdk.verify_gpg_status(good)
        self.assertEqual(checked["primaryFingerprint"], primary)
        for text in ("gpgv: Good signature", good.replace(primary, "B" * 40),
                     good + "[GNUPG:] BADSIG 1234567890ABCDEF OpenSSL\n", good + good):
            with self.subTest(text=text), self.assertRaises(support.AuditSafetyError):
                linux_d_sdk.verify_gpg_status(text)

    def test_openssl_signature_low_capacity_writes_no_keyring(self):
        pin = linux_d_sdk.load_openssl_pin(support.OWNED / "openssl-linux-pin.json")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.openssl_signature(pin, support.OWNED / "openssl-linux-pin.json", self.root / pin["archive"])
        self.assertFalse((self.root / "home/openssl-gpg").exists())
        self.sdk.command.assert_not_called()

    def test_gpg_status_accepts_binary_notation_without_weakening_rejections(self):
        primary = linux_d_sdk.OPENSSL_SIGNING_FINGERPRINT
        good = (b"[GNUPG:] GOODSIG 1234567890ABCDEF OpenSSL\n"
                b"[GNUPG:] NOTATION_DATA \xbb\xf2n%1Dq\xe8#aV\xc2\x87\n"
                b"[GNUPG:] VALIDSIG " + b"A" * 40 + b" 2026-09-29 1790690724 0 4 0 1 10 00 "
                + primary.encode("ascii") + b"\n")
        self.assertEqual(linux_d_sdk.verify_gpg_status(good)["primaryFingerprint"], primary)
        for text in (good.replace(primary.encode("ascii"), b"B" * 40),
                     good + b"[GNUPG:] BADSIG 1234567890ABCDEF OpenSSL\n", good + good):
            with self.subTest(text=text), self.assertRaises(support.AuditSafetyError):
                linux_d_sdk.verify_gpg_status(text)

    def test_existing_openssl_archive_requires_explicit_resume_before_command(self):
        pin = linux_d_sdk.load_openssl_pin(support.OWNED / "openssl-linux-pin.json")
        self.sdk.guard = mock.Mock(return_value={"chargedBytes": 0})
        self.sdk.prefix = self.root / "prefix"
        archive = self.root / "downloads" / pin["archive"]
        archive.parent.mkdir()
        archive.write_bytes(b"preserve")
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.openssl(support.OWNED / "openssl-linux-pin.json")
        self.assertEqual(archive.read_bytes(), b"preserve")
        self.sdk.command.assert_not_called()

    def test_product_requires_fresh_source_and_verified_ssl_before_command(self):
        self.sdk.guard = mock.Mock(return_value={"chargedBytes": 0})
        with self.assertRaises(support.AuditSafetyError):
            self.sdk.product()
        self.sdk.command.assert_not_called()

    def test_measure_tolerates_only_vanished_mutable_build_members(self):
        with mock.patch.object(linux_d_sdk.os, "walk", return_value=[(str(self.root / "build"), [], ["renamed.d.tmp"])]), \
                mock.patch.object(Path, "lstat", side_effect=FileNotFoundError()):
            result = linux_d_sdk.DriveSdk.measure(self.root)
        self.assertEqual(result["vanishedBuildEntries"], 1)
        self.assertEqual(result["chargedBytes"], 4096)
        for directory, name in (("sources", "renamed.d.tmp"), ("build", "missing.a"),
                                ("tmp", "renamed.d.tmp"), ("prefix", "renamed.d.tmp")):
            with self.subTest(directory=directory, name=name), \
                    mock.patch.object(linux_d_sdk.os, "walk", return_value=[(str(self.root / directory), [], [name])]), \
                    mock.patch.object(Path, "lstat", side_effect=FileNotFoundError()):
                with self.assertRaises(FileNotFoundError):
                    linux_d_sdk.DriveSdk.measure(self.root)
        with mock.patch.object(linux_d_sdk.os, "walk", return_value=[(str(self.root / "build"), [], ["renamed.d.tmp"])]), \
                mock.patch.object(Path, "lstat", side_effect=PermissionError()):
            with self.assertRaises(PermissionError):
                linux_d_sdk.DriveSdk.measure(self.root)


if __name__ == "__main__":
    unittest.main(verbosity=2)
