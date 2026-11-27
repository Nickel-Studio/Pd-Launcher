"""Exercise pinned TLS supply with inert inputs; no dependency or product is built."""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import ci_evidence as ci
import supply_openssl as supplier

spec = importlib.util.spec_from_file_location("pd_bootstrap_fixture", HERE / "bootstrap-deps.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class OpenSslSupplyTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pd-openssl-fixture-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def pin(self):
        base = "https://github.com/openssl/openssl/releases/download/openssl-3.5.9/"
        digest = "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a"
        asset = {"name": "openssl-3.5.9.tar.gz", "size": 53279637,
                 "digest": "sha256:" + digest, "browser_download_url": base + "openssl-3.5.9.tar.gz"}
        metadata = {"assets": [asset]}
        verification = {"archive": asset, "version": "3.5.9", "checksumSha256": digest,
                        "metadataAndChecksumMatched": True}
        pin = {"kind": "supplemental-nonwindows-dependency-source-pin", "name": "openssl",
               "version": "3.5.9", "archive": asset["name"], "bytes": asset["size"],
               "sha256": digest, "sourceDirectory": "openssl-3.5.9", "url": asset["browser_download_url"],
               "metadataAndChecksumMatched": True,
               "signatureFile": {"url": base + asset["name"] + ".asc", "bytes": 931,
                                 "sha256": "bdccf2057a7c4ae377b40e832cb8c632ae8b532d4fb64cc7b843066eee748f89"}}
        for name, document in (("metadataEvidence", metadata), ("verificationEvidence", verification)):
            path = self.root / (name + ".json")
            path.write_text(json.dumps(document), encoding="ascii")
            pin[name] = {"path": path.name, "sha256": ci.file_ref(path)["sha256"]}
        path = self.root / "pin.json"
        path.write_text(json.dumps(pin), encoding="ascii")
        return path, pin

    def test_exact_official_pin_and_evidence_are_loaded(self):
        path, expected = self.pin()
        self.assertEqual(supplier.load_pin(path), expected)

    def test_wrong_version_origin_archive_or_unsigned_pin_is_rejected(self):
        path, pin = self.pin()
        for key, value in (("version", "3.5.8"), ("url", "https://example.invalid/openssl.tar.gz"),
                           ("archive", "other.tar.gz"), ("bytes", 0), ("metadataAndChecksumMatched", False)):
            wrong = {**pin, key: value}
            path.write_text(json.dumps(wrong), encoding="ascii")
            with self.subTest(key=key), self.assertRaises(ci.EvidenceError):
                supplier.load_pin(path)

    def test_official_metadata_bytes_and_directory_escape_are_rejected(self):
        path, pin = self.pin()
        evidence = self.root / pin["metadataEvidence"]["path"]
        evidence.write_bytes(evidence.read_bytes() + b" ")
        with self.assertRaises(ci.EvidenceError):
            supplier.load_pin(path)
        wrong = copy.deepcopy(pin)
        wrong["metadataEvidence"]["path"] = "../outside.json"
        path.write_text(json.dumps(wrong), encoding="ascii")
        with self.assertRaises(ci.EvidenceError):
            supplier.load_pin(path)

    def test_checksum_must_identify_the_actual_official_asset(self):
        path, pin = self.pin()
        evidence = self.root / pin["verificationEvidence"]["path"]
        document = json.loads(evidence.read_bytes())
        document["archive"]["browser_download_url"] = "https://example.invalid/other.tar.gz"
        evidence.write_text(json.dumps(document), encoding="ascii")
        pin["verificationEvidence"]["sha256"] = ci.file_ref(evidence)["sha256"]
        path.write_text(json.dumps(pin), encoding="ascii")
        with self.assertRaises(ci.EvidenceError):
            supplier.load_pin(path)

    def signature(self, primary=supplier.SIGNING_PRIMARY):
        return ("[GNUPG:] NEWSIG\n[GNUPG:] GOODSIG ignored Untrusted Name\n"
                "[GNUPG:] VALIDSIG " + "A" * 40 + " 2026-09-29 1790681408 0 4 0 1 10 00 " + primary + "\n")

    def test_signature_requires_the_pinned_primary_not_display_name(self):
        supplier.verify_signature_status(self.signature())
        with self.assertRaises(ci.EvidenceError):
            supplier.verify_signature_status(self.signature("B" * 40))
        with self.assertRaises(ci.EvidenceError):
            supplier.verify_signature_status("Good signature from official OpenSSL signer\n")

    def test_expired_revoked_failed_duplicate_and_truncated_signatures_are_rejected(self):
        for status in (self.signature() + self.signature(), "[GNUPG:] VALIDSIG short\n",
                       *(self.signature() + "[GNUPG:] " + flag + " ignored\n" for flag in
                         ("BADSIG", "ERRSIG", "EXPSIG", "EXPKEYSIG", "REVKEYSIG", "NO_PUBKEY"))):
            with self.subTest(status=status), self.assertRaises(ci.EvidenceError):
                supplier.verify_signature_status(status)

    def archive(self, name="openssl-3.5.9/README", member_type=tarfile.REGTYPE, size=7):
        path = self.root / "source.tar"
        with tarfile.open(path, "w") as archive:
            member = tarfile.TarInfo(name)
            member.type = member_type
            if member_type == tarfile.REGTYPE:
                member.size = size
                archive.addfile(member, io.BytesIO(b"fixture"[:size]))
            else:
                member.linkname = "../outside"
                archive.addfile(member)
        return path

    def test_archive_extracts_only_regular_members_beneath_the_pinned_root(self):
        output = self.root / "sources"
        output.mkdir()
        supplier.unpack(self.archive(), output, "openssl-3.5.9")
        self.assertEqual((output / "openssl-3.5.9/README").read_bytes(), b"fixture")

    def test_archive_traversal_links_devices_and_wrong_root_are_rejected(self):
        output = self.root / "sources"
        output.mkdir()
        for name, member_type in (("../outside", tarfile.REGTYPE), ("/absolute", tarfile.REGTYPE),
                                  ("other/README", tarfile.REGTYPE),
                                  ("openssl-3.5.9/link", tarfile.SYMTYPE),
                                  ("openssl-3.5.9/hardlink", tarfile.LNKTYPE),
                                  ("openssl-3.5.9/device", tarfile.CHRTYPE)):
            with self.subTest(name=name, type=member_type), self.assertRaises(ci.EvidenceError):
                supplier.unpack(self.archive(name, member_type), output, "openssl-3.5.9")
        self.assertEqual(list(output.iterdir()), [])

    def test_windows_is_not_admitted_and_existing_outputs_are_preserved(self):
        path, _ = self.pin()
        options = argparse.Namespace(pin=path, prefix=self.root / "prefix", work_root=self.root / "work",
                                     output=self.root / "output.json")
        with patch.object(supplier.sys, "platform", "win32"), self.assertRaises(ci.EvidenceError):
            supplier.supply(options)
        options.output.write_bytes(b"preserved")
        with patch.object(supplier.sys, "platform", "linux"), patch.object(supplier.platform, "machine", return_value="x86_64"):
            with self.assertRaises(ci.EvidenceError):
                supplier.supply(options)
        self.assertEqual(options.output.read_bytes(), b"preserved")
        self.assertFalse(options.work_root.exists())

    def test_failed_actual_command_retains_failure_receipt_not_a_success_claim(self):
        path, _ = self.pin()
        options = argparse.Namespace(pin=path, prefix=self.root / "prefix", work_root=self.root / "work",
                                     output=self.root / "output.json")
        def fail_command(argv, environment, log, evidence):
            log.write_bytes(b"inert failure, not a real dependency download\n")
            ci.write_json(evidence, {"argv": argv, "complete": True, "returnCode": 22})
            return 22
        with patch.object(supplier.sys, "platform", "linux"), \
                patch.object(supplier.platform, "machine", return_value="x86_64"), \
                patch.object(supplier.shutil, "disk_usage", return_value=argparse.Namespace(free=8 * 1024 ** 3)), \
                patch.object(supplier.shutil, "which", return_value="/usr/bin/curl"), \
                patch.object(supplier, "run_logged", side_effect=fail_command):
            self.assertEqual(supplier.supply(options), 1)
        record = ci.read_json(options.output)
        self.assertFalse(record["complete"])
        self.assertFalse(record["signatureVerified"])
        self.assertFalse(record["productAcceptance"])
        self.assertEqual(len(record["commands"]), 1)
        self.assertIn("failed", record["failure"])

    def builder(self, prefix):
        instance = bootstrap.Builder.__new__(bootstrap.Builder)
        instance.options = argparse.Namespace(openssl_prefix=prefix)
        instance.prefix = self.root / "dependencies"
        instance.evidence = {}
        instance.commands = []
        instance.save_evidence = lambda: None
        instance.source = lambda name: self.root / name
        instance.cmake_project = lambda name, definitions, *args, **kwargs: instance.commands.append((name, definitions))
        return instance

    def test_linux_core_refuses_implicit_system_tls_before_building(self):
        instance = self.builder(None)
        with patch.object(bootstrap.os, "name", "posix"), self.assertRaises(RuntimeError):
            instance.build_core()
        self.assertEqual(instance.commands, [])

    def test_linux_curl_uses_explicit_approved_static_libraries(self):
        prefix = self.root / "openssl"
        (prefix / "include/openssl").mkdir(parents=True)
        (prefix / "lib").mkdir()
        (prefix / "include/openssl/opensslv.h").write_text('# define OPENSSL_VERSION_STR "3.5.9"\n', encoding="ascii")
        for name in ("libssl.a", "libcrypto.a"):
            (prefix / "lib" / name).write_bytes(b"inert archive fixture")
        instance = self.builder(prefix)
        with patch.object(bootstrap.os, "name", "posix"):
            instance.build_core()
        curl = dict(instance.commands)["curl"]
        self.assertIn("-DCURL_USE_OPENSSL=ON", curl)
        self.assertNotIn("-DCURL_USE_OPENSSL=OFF", curl)
        self.assertIn("-DOPENSSL_SSL_LIBRARY=" + (prefix / "lib/libssl.a").as_posix(), curl)
        self.assertIn("-DOPENSSL_CRYPTO_LIBRARY=" + (prefix / "lib/libcrypto.a").as_posix(), curl)
        self.assertFalse(instance.evidence["supplementalOpenSSL"]["systemSslUsed"])

    def test_windows_core_remains_schannel_without_openssl(self):
        instance = self.builder(None)
        with patch.object(bootstrap.os, "name", "nt"):
            instance.build_core()
        curl = dict(instance.commands)["curl"]
        self.assertIn("-DCURL_USE_SCHANNEL=ON", curl)
        self.assertIn("-DCURL_USE_OPENSSL=OFF", curl)
        self.assertFalse(any(value.startswith("-DOPENSSL_") for value in curl))
        self.assertNotIn("supplementalOpenSSL", instance.evidence)


if __name__ == "__main__":
    unittest.main()
