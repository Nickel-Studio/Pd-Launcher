# SPDX-License-Identifier: AGPL-3.0-only
"""Supplied raster provenance, resource bounds and no-redraw regressions."""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
BRAND = ROOT / "native/assets/brand"
sys.path.insert(0, str(ROOT / ".snow/brand-tools"))
from PIL import Image

SPEC = importlib.util.spec_from_file_location("palladium_importer", ROOT / "scripts/native/brand-palladium.py")
IMPORTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IMPORTER)


class PalladiumSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.intake_bytes = (BRAND / IMPORTER.INTAKE).read_bytes()
        cls.intake = IMPORTER.load_intake(cls.intake_bytes)
        cls.manifest = json.loads((BRAND / "asset-manifest.json").read_bytes())
        cls.assets = {row["path"]: row for row in cls.manifest["assets"]}

    def test_active_rasters_are_exact_supplied_bytes_and_pixels(self):
        self.assertEqual(self.manifest["suppliedArtworkCount"], 54)
        for row in self.intake["selectedMappings"]:
            with self.subTest(source=row["sourcePath"]):
                data = (BRAND / row["targetPath"]).read_bytes()
                self.assertEqual(len(data), row["bytes"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), row["sha256"])
                actual = self.assets[row["targetPath"]]
                self.assertEqual(actual["sourcePath"], row["sourcePath"])
                self.assertEqual(actual["sourceSha256"], row["sha256"])
                self.assertEqual(actual["derivation"], "byte-identical-user-copy")
                self.assertEqual(actual["license"], IMPORTER.PROJECT_ART_LICENSE)
                self.assertEqual(actual["sourceCopyright"], "Nickel Studio (ItzArona, ItzPlana)")
                if row["format"] == "PNG":
                    with Image.open(io.BytesIO(data)) as source:
                        image = source.convert("RGBA")
                        self.assertEqual(image.size, (row["width"], row["height"]))
                        self.assertEqual(hashlib.sha256(image.tobytes()).hexdigest(), row["rgbaSha256"])
                        self.assertEqual(list(image.getchannel("A").getbbox() or []), row["alphaBounds"])

    def test_runtime_is_only_six_marks_icons_and_ten_raster_wordmarks(self):
        expected = {"icon.png", "icon-light.png"} | {f"mark-{color}.png" for color in ("black", "white", "blue", "silver")}
        expected |= {f"wordmarks/{name}-{color}.png" for name in ("palladium", "pd", "ba", "palladium-launcher", "pd-launcher") for color in ("black", "white")}
        expected |= {"fonts/nickel-text-subset.ttf", "fonts/OFL-1.1.txt", "fonts/NOTICE.md", "NOTICE.txt"}
        resource = ET.parse(BRAND / "brand.qrc").getroot().findall("qresource")
        self.assertEqual(len(resource), 1)
        self.assertEqual(resource[0].attrib["prefix"], "/nickel/brand")
        entries = {row.attrib["alias"]: row.text for row in resource[0].findall("file")}
        self.assertEqual(set(entries), expected)
        self.assertEqual(set(self.manifest["qtResourceFiles"]), expected)
        self.assertTrue(all(alias == target for alias, target in entries.items()))
        self.assertEqual(self.manifest["runtimePngCompressedBytes"], 269306)
        self.assertEqual(self.manifest["runtimePngDecodedBytes"], 41549824)
        self.assertNotIn("blocks/", " ".join(row["sourcePath"] for row in self.intake["selectedMappings"]))
        self.assertNotIn("Glass", " ".join(row["sourcePath"] for row in self.intake["selectedMappings"]))

    def test_artwork_ships_under_cc_by_sa_with_attribution(self):
        self.assertEqual(self.manifest["artLicense"], "CC-BY-SA-4.0")
        self.assertTrue(self.manifest["artLicenseConfirmed"])
        self.assertEqual(self.manifest["sourceCopyright"], "Nickel Studio (ItzArona, ItzPlana)")
        self.assertEqual(self.manifest["sourceLicenseDeclared"], "CC-BY-SA-4.0")
        notice = (BRAND / "NOTICE.txt").read_text(encoding="utf-8")
        self.assertIn("Creative Commons Attribution-ShareAlike 4.0", notice)
        self.assertIn("CC-BY-SA-4.0", notice)
        self.assertNotIn("LicenseRef-Palladium-Project-Art", notice)
        self.assertFalse(self.manifest["nativeReleaseReady"])
        self.assertEqual(self.manifest["atomicNumber"], 46)
        self.assertFalse(self.manifest["atomicNumberIsSoftwareVersion"])

    def test_original_ico_frames_are_pixel_equal_not_container_byte_equal(self):
        for theme in ("", "-light"):
            data = (BRAND / ("platform/app" + theme + ".ico")).read_bytes()
            self.assertEqual(struct.unpack_from("<HHH", data), (0, 1, 9))
            for index, size in enumerate(IMPORTER.SIZES):
                with self.subTest(theme=theme, size=size):
                    width, height, colors, reserved, planes, depth, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + index * 16)
                    self.assertEqual((width or 256, height or 256), (size, size))
                    # Supplied PNG-compressed ICO frames use zero planes; preserve their exact headers.
                    self.assertEqual((colors, reserved, planes, depth), (0, 0, 0, 32))
                    self.assertGreaterEqual(offset, 6 + 9 * 16)
                    self.assertLessEqual(offset + length, len(data))
                    payload = data[offset:offset + length]
                    standalone = (BRAND / f"platform/icon{theme}-{size}.png").read_bytes()
                    with Image.open(io.BytesIO(payload)) as frame, Image.open(io.BytesIO(standalone)) as source:
                        self.assertEqual(frame.convert("RGBA").tobytes(), source.convert("RGBA").tobytes())
                    self.assertNotEqual(payload, standalone, "ICO compression differs; pixel equivalence is not byte identity")

    def test_icns_only_packages_original_png_bytes_without_invented_512(self):
        row = self.assets["platform/app.icns"]
        data = (BRAND / row["path"]).read_bytes()
        self.assertEqual(data[:4], b"icns")
        self.assertEqual(struct.unpack_from(">I", data, 4)[0], len(data))
        self.assertEqual(row["missingSuppliedFrameSizes"], [512])
        sources = {mapping["sourcePath"]: mapping["targetPath"] for mapping in self.intake["selectedMappings"]}
        cursor = 8
        for frame in row["frameSources"]:
            kind, size = struct.unpack_from(">4sI", data, cursor)
            self.assertEqual(kind.decode("ascii"), frame["type"])
            payload = data[cursor + 8:cursor + size]
            self.assertEqual(payload, (BRAND / sources[frame["sourcePath"]]).read_bytes())
            self.assertEqual(hashlib.sha256(payload).hexdigest(), frame["sha256"])
            cursor += size
        self.assertEqual(cursor, len(data))
        self.assertNotIn(512, row["frameSizes"])

    def test_isolated_rebuild_reproduces_all_active_bytes(self):
        parent = ROOT / ".snow/legal-unit"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="pd-rebuild-", dir=parent) as temp:
            output = Path(temp)
            rebuilt = IMPORTER.rebuild(output)
            self.assertEqual(rebuilt, self.manifest)
            self.assertEqual((output / "asset-manifest.json").read_bytes(), (BRAND / "asset-manifest.json").read_bytes())
            for row in self.manifest["assets"]:
                self.assertEqual((output / row["path"]).read_bytes(), (BRAND / row["path"]).read_bytes())

    def test_wrong_inventory_and_changed_source_fail_before_output(self):
        with self.assertRaisesRegex(ValueError, "exact independently inventoried"):
            IMPORTER.load_intake(self.intake_bytes + b" ")
        supplied = {row["sourcePath"]: (BRAND / row["targetPath"]).read_bytes() for row in self.intake["selectedMappings"]}
        supplied["logo/Palladium.png"] += b"tampered"
        with self.assertRaisesRegex(ValueError, "Selected source bytes changed"):
            IMPORTER.prepare(self.intake_bytes, supplied)

    def test_output_preflight_rejects_missing_disk_reserve(self):
        parent = ROOT / ".snow/legal-unit"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="pd-preflight-", dir=parent) as temp:
            output = Path(temp)
            with patch("shutil.disk_usage") as usage:
                usage.return_value.free = 268435455
                with self.assertRaisesRegex(ValueError, "reserved space"):
                    IMPORTER.write_outputs(output, {"first.txt": b"must not write"})
            self.assertFalse((output / "first.txt").exists())

if __name__ == "__main__":
    unittest.main(verbosity=2)
