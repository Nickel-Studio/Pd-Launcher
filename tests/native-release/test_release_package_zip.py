"""Synthetic regression for the program ZIP writer's deflate level only.

No fixture or result here is a launcher artifact: the payloads are generated
text and the "installed" directory is a temp directory.  The regression being
pinned: `ZipFile(compresslevel=9)` does not propagate to members written
through an explicitly constructed `ZipInfo` (each member's level must be
passed to `writestr()` itself, public API since Python 3.7).  Without that,
the packaging tool silently deflates at zlib's default level 6 while its
manifest claims "deflate-level-9" (fixed 2026-11-23).

The payload is deliberately level-sensitive: measured margins on this data
are ~40% between deflate level 1 and level 9 and ~8% between level 6 and
level 9, so these assertions detect a wrong level — including the historical
fallback to zlib's default level 6 — rather than only detecting "stored".
"""
from __future__ import annotations

import hashlib
import io
import random
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "native"))

import release_package  # noqa: E402

TOP_LEVEL = "pd-launcher-windows-x64-0.1.0"
EXECUTABLE = "pd-launcher.exe"


def level_sensitive_payload(total_bytes: int = 1 << 20) -> bytes:
    """Deterministic log-like text whose deflate size separates levels.

    Incrementing timestamps force near-duplicate lines at shifting offsets, so
    higher deflate effort keeps finding better matches: zlib L1 ≈ 0.260×,
    L6 ≈ 0.170×, L9 ≈ 0.156× of the raw size on this fixture.
    """
    rng = random.Random(77)
    messages = ("INFO worker ready", "WARN cache miss", "INFO scan complete",
                "TRACE frame drawn")
    lines, size, timestamp = [], 0, 1_766_500_000
    while size < total_bytes:
        timestamp += rng.randint(1, 5)
        line = "[%d] %s id=%d dur=%dms\n" % (timestamp, rng.choice(messages),
                                             rng.randrange(4096), rng.randrange(50))
        lines.append(line.encode("ascii"))
        size += len(line) + 1
    return b"".join(lines)


class ProgramZipCompressionTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory(prefix="release-package-zip-")
        self.root = Path(self._temp.name)
        self.installed = self.root / "install"
        self.installed.mkdir()
        self.payload = level_sensitive_payload()
        (self.installed / EXECUTABLE).write_bytes(self.payload)

    def tearDown(self):
        self._temp.cleanup()

    def install_rows(self):
        return release_package.installed_program_files(self.installed, EXECUTABLE)

    def reference_zip_bytes(self, compresslevel: int) -> bytes:
        """Same construction as build_program_zip with the level passed explicitly."""
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED,
                             compresslevel=9) as archive:
            directory = zipfile.ZipInfo(TOP_LEVEL + "/",
                                        date_time=release_package.FIXED_ZIP_DATETIME)
            directory.create_system = 0
            directory.external_attr = 0x10  # MS-DOS directory attribute
            archive.writestr(directory, b"")
            member = zipfile.ZipInfo("%s/%s" % (TOP_LEVEL, EXECUTABLE),
                                     date_time=release_package.FIXED_ZIP_DATETIME)
            member.create_system = 0
            member.external_attr = 0x20  # MS-DOS FILE_ATTRIBUTE_ARCHIVE
            member.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(member, self.payload, compresslevel=compresslevel)
        return buffer.getvalue()

    def build_program_zip(self, name: str) -> Path:
        zip_path = self.root / name
        package = release_package.build_program_zip(
            self.installed, self.install_rows(), zip_path, TOP_LEVEL)
        self.assertEqual(package["compression"], "deflate-level-9")
        return zip_path

    def test_writestr_accepts_compresslevel_keyword(self):
        # The fix relies on writestr()'s documented compresslevel keyword
        # (Python 3.7+); if a future Python drops it, packaging would fail
        # with TypeError.  This check makes the dependency explicit.
        self.assertIn("compresslevel", zipfile.ZipFile.writestr.__code__.co_varnames)

    def test_members_are_deflated_at_explicit_level_9(self):
        zip_path = self.build_program_zip("program.zip")
        payload = self.payload
        l1 = len(zlib.compress(payload, 1))
        l6 = len(zlib.compress(payload, 6))
        l9 = len(zlib.compress(payload, 9))
        # The fixture must stay level-sensitive or the assertions below are void.
        self.assertGreaterEqual(l1 - l9, l1 // 10)  # measured ~40%
        self.assertGreaterEqual(l6 - l9, l6 // 25)  # measured ~8%

        with zipfile.ZipFile(zip_path) as archive:
            self.assertIsNone(archive.testzip())
            member = archive.getinfo("%s/%s" % (TOP_LEVEL, EXECUTABLE))
            self.assertEqual(member.compress_type, zipfile.ZIP_DEFLATED)
            # A raw deflate stream is its zlib-wrapped form minus the 6-byte
            # wrapper, so genuine level-9 output never exceeds zlib's L9 size;
            # zlib default level 6 (the historical bug) lands ~8% above it.
            self.assertLessEqual(member.compress_size, l9)
            self.assertLess(member.compress_size, l6)
            self.assertLess(member.compress_size, l1)
            self.assertEqual(archive.read(member), payload)
        self.assertEqual(member.file_size, len(payload))

    def test_archive_is_smaller_than_level_1_and_level_6_controls(self):
        zip_path = self.build_program_zip("program.zip")
        control1 = self.root / "control-level-1.zip"
        control6 = self.root / "control-level-6.zip"
        control1.write_bytes(self.reference_zip_bytes(1))
        control6.write_bytes(self.reference_zip_bytes(6))
        built = zip_path.stat().st_size
        self.assertLess(control6.stat().st_size, control1.stat().st_size)
        self.assertLess(built, control6.stat().st_size)
        # The task-mandated comparison: level 9 must be clearly below level 1.
        self.assertLess(built, int(control1.stat().st_size * 0.85))

    def test_output_matches_explicit_level_9_reference_writer(self):
        zip_path = self.build_program_zip("program.zip")
        self.assertEqual(zip_path.read_bytes(), self.reference_zip_bytes(9))

    def test_repeated_builds_are_byte_identical(self):
        first = self.build_program_zip("first.zip")
        second = self.build_program_zip("second.zip")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(hashlib.sha256(first.read_bytes()).hexdigest(),
                         hashlib.sha256(second.read_bytes()).hexdigest())

    def test_member_order_mode_bits_timestamps_and_separators_are_fixed(self):
        zip_path = self.build_program_zip("program.zip")
        with zipfile.ZipFile(zip_path) as archive:
            entries = archive.infolist()
        self.assertEqual([entry.filename for entry in entries],
                         [TOP_LEVEL + "/", "%s/%s" % (TOP_LEVEL, EXECUTABLE)])
        for entry in entries:
            self.assertNotIn("\\", entry.filename)  # POSIX separators only
            self.assertEqual(entry.date_time, release_package.FIXED_ZIP_DATETIME)
            self.assertEqual(entry.create_system, 0)
        directory, member = entries
        self.assertTrue(directory.is_dir())
        self.assertEqual(directory.external_attr, 0x10)
        self.assertEqual(directory.compress_type, zipfile.ZIP_STORED)
        self.assertEqual(directory.file_size, 0)
        self.assertFalse(member.is_dir())
        self.assertEqual(member.external_attr, 0x20)
        self.assertEqual(member.file_size, len(self.payload))

    def test_verify_program_zip_roundtrip_and_tamper_detection(self):
        zip_path = self.build_program_zip("program.zip")
        members = release_package.verify_program_zip(zip_path, self.installed, TOP_LEVEL)
        self.assertEqual([row["distributionPath"] for row in members], [EXECUTABLE])
        self.assertEqual(members[0]["sha256"], hashlib.sha256(self.payload).hexdigest())
        # Any change to the installed set must fail verification, not pass.
        (self.installed / EXECUTABLE).write_bytes(self.payload + b"tampered\n")
        with self.assertRaises(release_package.PackageError):
            release_package.verify_program_zip(zip_path, self.installed, TOP_LEVEL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
