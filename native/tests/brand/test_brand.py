# SPDX-License-Identifier: AGPL-3.0-only
"""Real Qt resources, outlined geometry, platform frames and pixel regressions."""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / ".snow/brand-tools"))
from PIL import Image
from fontTools.ttLib import TTFont
from PySide6.QtCore import QByteArray, QFile, QIODevice, QResource, qVersion
from PySide6.QtGui import QFontDatabase, QGuiApplication, QImage
from PySide6.QtSvg import QSvgRenderer
import PySide6

BRAND = ROOT / "native/assets/brand"
SVG = "http://www.w3.org/2000/svg"
XLINK = "http://www.w3.org/1999/xlink"


class BrandResourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])
        cls.active_manifest = json.loads((BRAND / "asset-manifest.json").read_bytes())
        cls.manifest = cls.active_manifest

    def test_exact_asset_manifest_hashes_and_types(self):
        self.assertEqual(self.manifest["artLicense"], "CC-BY-SA-4.0")
        self.assertTrue(self.manifest["artLicenseConfirmed"])
        self.assertEqual(self.manifest["fontLicense"], "OFL-1.1")
        self.assertEqual(self.manifest["sourceSha256"],
                         hashlib.sha256((BRAND / self.manifest["preferredSource"]).read_bytes()).hexdigest())
        names = set()
        for item in self.manifest["assets"]:
            with self.subTest(asset=item["path"]):
                path = (BRAND / item["path"]).resolve()
                self.assertTrue(path.is_relative_to(BRAND))
                self.assertNotIn(item["path"], names)
                names.add(item["path"])
                data = path.read_bytes()
                self.assertEqual(len(data), item["bytes"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
                if item["format"] == "PNG":
                    image = Image.open(io.BytesIO(data))
                    image.verify()
                    self.assertEqual(image.size, (item["width"], item["height"]))

    def test_qt_svg_profile_has_no_bitmap_remote_text_or_hidden_icons(self):
        allowed = {"svg", "title", "desc", "defs", "g", "path", "rect", "use", "metadata"}
        for item in self.manifest["assets"]:
            if item["format"] != "SVG":
                continue
            with self.subTest(asset=item["path"]):
                data = (BRAND / item["path"]).read_bytes()
                self.assertTrue(QSvgRenderer(QByteArray(data)).isValid())
                tree = ET.fromstring(data)
                ids = {node.attrib["id"] for node in tree.iter() if "id" in node.attrib}
                for node in tree.iter():
                    self.assertIn(node.tag.removeprefix(f"{{{SVG}}}"), allowed)
                    if node.tag == f"{{{SVG}}}use":
                        href = node.attrib[f"{{{XLINK}}}href"]
                        self.assertTrue(href.startswith("#"))
                        self.assertIn(href[1:], ids)
                self.assertNotIn(b"base64", data)
                self.assertNotIn(b"<image", data)
                self.assertNotIn(b"<text", data)
                self.assertNotIn(b"filter=", data)
                self.assertNotIn(b"<linearGradient", data)

    def test_dark_and_light_share_transparent_alpha_geometry(self):
        # The supplied dark/light marks differ only in colour: alpha contours must be identical.
        black = Image.open(BRAND / "mark-black.png").convert("RGBA")
        white = Image.open(BRAND / "mark-white.png").convert("RGBA")
        self.assertEqual(black.size, white.size)
        self.assertEqual(black.getchannel("A").tobytes(), white.getchannel("A").tobytes())
        self.assertNotEqual(black.tobytes(), white.tobytes())

    def test_existing_noto_is_text_not_an_icon_font(self):
        original = ROOT / "src/assets/fonts/nickel-line.woff2"
        copy = BRAND / "fonts/nickel-line.woff2"
        self.assertEqual(original.read_bytes(), copy.read_bytes())
        source = TTFont(original, recalcTimestamp=False)
        ttf = TTFont(BRAND / "fonts/nickel-text-subset.ttf", recalcTimestamp=False)
        self.assertEqual(source.getBestCmap(), ttf.getBestCmap())
        cmap = ttf.getBestCmap()
        self.assertEqual(len(cmap), 109)
        self.assertFalse(any(0xE000 <= char <= 0xF8FF for char in cmap))
        self.assertNotIn(ord("镍"), cmap)
        self.assertNotIn(ord("/"), cmap)
        self.assertTrue(all(ord(char) in cmap for char in "Nickel Launcher 28"))
        self.assertNotIn("fvar", ttf)
        self.assertEqual(ttf["OS/2"].usWeightClass, 400)
        self.assertTrue(any("Adobe" in row.toUnicode() for row in ttf["name"].names if row.nameID == 0))
        self.assertTrue(any(row.toUnicode() == "NotoSansSC-Regular" for row in ttf["name"].names if row.nameID == 6))
        font_id = QFontDatabase.addApplicationFont(str(BRAND / "fonts/nickel-text-subset.ttf"))
        self.assertGreaterEqual(font_id, 0)
        self.assertTrue(QFontDatabase.applicationFontFamilies(font_id))
        QFontDatabase.removeApplicationFont(font_id)
        text = (BRAND / "fonts/OFL-1.1.txt").read_text(encoding="utf-8")
        self.assertIn("Reserved Font Name 'Source'", text)
        self.assertIn("SIL OPEN FONT LICENSE", text)

    def test_real_rcc_compilation_and_loaded_resource_bytes(self):
        executable = Path(PySide6.__file__).parent / ("rcc.exe" if sys.platform == "win32" else "rcc")
        self.assertTrue(executable.is_file(), "Pinned Qt rcc tool must exist")
        evidence = ROOT / ".snow/brand-evidence"
        evidence.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="qt-rcc-", dir=evidence) as temp:
            output = Path(temp) / "brand.rcc"
            completed = subprocess.run([str(executable), "--binary", "--output", str(output), str(BRAND / "brand.qrc"),
                                        str(BRAND / "functional-resources.qrc")], capture_output=True, text=True, timeout=60)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertTrue(QResource.registerResource(str(output)))
            try:
                expected_resources = {":/nickel/brand/" + path: BRAND / path for path in self.active_manifest["qtResourceFiles"]}
                functional = BRAND / "functional-resources.qrc"
                for node in ET.parse(functional).getroot().find("qresource").findall("file"):
                    expected_resources[":/nickel/functional/" + node.attrib["alias"]] = (functional.parent / node.text).resolve()
                for path, original in expected_resources.items():
                    with self.subTest(resource=path):
                        file = QFile(path)
                        self.assertTrue(file.open(QIODevice.OpenModeFlag.ReadOnly))
                        data = bytes(file.readAll())
                        self.assertEqual(data, original.read_bytes())
                        file.close()
                        # QFile still owns a QResource reference after close().
                        del file
                        if path.endswith(".svg"):
                            self.assertTrue(QSvgRenderer(QByteArray(data)).isValid())
                        if path.endswith(".png"):
                            self.assertFalse(QImage.fromData(QByteArray(data)).isNull())
                for row in self.active_manifest["assets"]:
                    if not row["qtEmbedded"]:
                        self.assertFalse(QFile.exists(":/nickel/brand/" + row["path"]))
                if self.active_manifest.get("product") == "Palladium":
                    self.assertFalse(QFile.exists(":/nickel/brand/mark-black.svg"))
                    self.assertFalse(QFile.exists(":/nickel/brand/CC-BY-SA-4.0.txt"))
                font_id = QFontDatabase.addApplicationFont(":/nickel/brand/fonts/nickel-text-subset.ttf")
                self.assertGreaterEqual(font_id, 0)
                QFontDatabase.removeApplicationFont(font_id)
            finally:
                self.assertTrue(QResource.unregisterResource(str(output)))

    def test_cmake_trusted_checkout_preserves_mirrored_functional_resources(self):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "CMake 3.24+ must be on PATH for the trusted-checkout regression")
        rcc = Path(PySide6.__file__).parent / ("rcc.exe" if sys.platform == "win32" else "rcc")
        self.assertTrue(rcc.is_file())
        source = ROOT / "native/tests/brand/CMakeLists.txt"
        original = BRAND / "functional-resources.qrc"
        original_entries = ET.parse(original).getroot().find("qresource").findall("file")
        evidence = ROOT / ".snow/brand-evidence"
        evidence.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="cmake-brand-path-", dir=evidence) as temp:
            workspace = Path(temp)
            mirror = workspace / "mirror"
            mirrored_source = mirror / "native/tests/brand/CMakeLists.txt"
            mirrored_qrc = mirror / "native/assets/brand/functional-resources.qrc"
            for destination, data in ((mirrored_source, source.read_bytes()),
                                      (mirrored_qrc, original.read_bytes()),
                                      (mirror / "native/tests/brand/functional-map.json", (source.parent / "functional-map.json").read_bytes()),
                                      (mirror / "native/assets/brand/asset-manifest.json", (BRAND / "asset-manifest.json").read_bytes())):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
            self.assertFalse((mirror / "src").exists(), "No original artwork is copied into the mirror")
            # The sparse oracle contains only referenced originals, never native brand/map materials.
            oracle = workspace / "original oracle & assets"
            originals = {(original.parent / node.text).resolve().relative_to(ROOT) for node in original_entries}
            originals.update({Path("src/assets/fonts/nickel-line.woff2"), Path("tests/brand/functional-icons.sha256")})
            for relative in originals:
                destination = oracle / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes((ROOT / relative).read_bytes())
            self.assertFalse((oracle / "native").exists())
            # Script-mode command stubs avoid a second compiler/object tree, not Qt resource compilation.
            commands = ("project", "enable_testing", "find_package", "add_executable", "target_link_libraries",
                        "target_compile_definitions", "add_test", "set_tests_properties")
            stubs = "".join(f'function({command})\n'
                            f'    set(_captured_{command} "${{ARGV}}" PARENT_SCOPE)\n'
                            'endfunction()\n' for command in commands)
            cases = (("standalone", source, source.parent, None, None),
                     ("nested", mirrored_source, ROOT, oracle, None),
                     ("explicit", mirrored_source, mirror, oracle, None),
                     ("missing", mirrored_source, ROOT, None, "explicit trusted ASSET_ORACLE_ROOT"),
                     ("wrong-root", mirrored_source, ROOT, mirror, "missing src/assets/mod-placeholder.png"),
                     ("relative-root", mirrored_source, ROOT, Path("."), "must be an absolute trusted checkout path"))
            for name, included, source_root, trusted, expected_error in cases:
                with self.subTest(mode=name):
                    output = workspace / name
                    output.mkdir()
                    trace = output / "target-contract.txt"
                    script = output / "probe.cmake"
                    settings = (f'set(CMAKE_SOURCE_DIR [[{source_root.as_posix()}]])\n'
                                f'set(CMAKE_CURRENT_SOURCE_DIR [[{included.parent.as_posix()}]])\n'
                                f'set(CMAKE_CURRENT_BINARY_DIR [[{output.as_posix()}]])\n')
                    if trusted is not None:
                        settings += f'set(ASSET_ORACLE_ROOT [[{trusted.as_posix()}]] CACHE PATH "" FORCE)\n'
                    text = stubs + settings + f'include([[{included.as_posix()}]])\n'
                    text += (f'file(WRITE [[{trace.as_posix()}]] '
                             '"${_captured_add_executable}\\n${_captured_target_compile_definitions}\\n'
                             '${_captured_add_test}\\n${_captured_set_tests_properties}\\n'
                             '${_captured_project}\\n${_captured_find_package}\\n${_nickel_functional_template}\\n")\n')
                    script.write_bytes(text.encode("utf-8"))
                    completed = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True, timeout=60)
                    generated = output / "nickel_brand_functional_resources.qrc"
                    if expected_error is not None:
                        self.assertNotEqual(completed.returncode, 0)
                        self.assertIn(expected_error, " ".join(completed.stderr.split()))
                        self.assertFalse(generated.exists())
                        continue
                    self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                    contract = trace.read_text(encoding="utf-8").splitlines()
                    self.assertEqual(contract[0].split(";"), ["nickel_brand_resource_test", "brand_resources_test.cpp",
                                                            "../../assets/brand/brand.qrc", generated.as_posix()])
                    brand_root = included.parents[3]
                    oracle_root = ROOT if trusted is None else trusted
                    self.assertEqual(contract[1].split(";"), ["nickel_brand_resource_test", "PRIVATE",
                        f'NICKEL_BRAND_CHECKOUT="{brand_root.as_posix()}"', f'ASSET_ORACLE_ROOT="{oracle_root.as_posix()}"'])
                    self.assertEqual(contract[2], "NAME;nickel.brand.qt-resources;COMMAND;nickel_brand_resource_test")
                    self.assertIn("QT_QPA_PLATFORM=offscreen", contract[3])
                    self.assertEqual(contract[4], "NickelBrandResourceTests;LANGUAGES;CXX" if name == "standalone" else "")
                    self.assertEqual(contract[5], "Qt6;6.8;REQUIRED;COMPONENTS;Core;Gui;Svg")
                    self.assertEqual(Path(contract[6]), (included.parent / "../../assets/brand/functional-resources.qrc").resolve())
                    self.assertNotIn("/../", contract[6], "Configure input must not become a duplicate Ninja phony output")
                    tree = ET.parse(generated).getroot()
                    self.assertEqual(tree.find("qresource").attrib, {"prefix": "/nickel/functional"})
                    entries = tree.find("qresource").findall("file")
                    self.assertEqual(len(entries), len(original_entries))
                    for actual, expected in zip(entries, original_entries):
                        self.assertEqual(actual.attrib, expected.attrib)
                        original_path = (original.parent / expected.text).resolve()
                        self.assertEqual(Path(actual.text), oracle_root / original_path.relative_to(ROOT))
                        self.assertEqual(Path(actual.text).read_bytes(), original_path.read_bytes())
                    compiled = output / "functional.rcc"
                    result = subprocess.run([str(rcc), "--binary", "--output", str(compiled), str(generated)],
                                            capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertTrue(QResource.registerResource(str(compiled)))
                    try:
                        for entry in entries:
                            resource = QFile(":/nickel/functional/" + entry.attrib["alias"])
                            try:
                                self.assertTrue(resource.open(QIODevice.OpenModeFlag.ReadOnly))
                                self.assertEqual(bytes(resource.readAll()), Path(entry.text).read_bytes())
                            finally:
                                resource.close()
                                del resource
                    finally:
                        self.assertTrue(QResource.unregisterResource(str(compiled)))
            self.assertFalse((mirror / "src").exists())
            self.assertFalse((oracle / "native").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
