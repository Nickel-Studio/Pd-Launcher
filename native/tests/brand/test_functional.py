# SPDX-License-Identifier: AGPL-3.0-only
"""Immutable old functional artwork and instance mapping checks (stdlib only)."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
FIXTURE_PATH = ROOT / "native/tests/brand/functional-map.json"
BASELINE_LF_SHA256 = "585116fbc8852f10c649d64aabd74a19ea44b33b1c994ed703ed6bfbbabdbb8d"


def load_fixture() -> dict:
    raw = FIXTURE_PATH.read_bytes()
    expected = "95517ca822a7acafe750eaed93503e25dbee9f17f50e8248b1024c662c1dcdc4"
    if hashlib.sha256(raw.replace(b"\r\n", b"\n")).hexdigest() != expected:
        raise AssertionError("The immutable functional mapping/skin/credit contract was edited")
    return json.loads(raw)


def baseline_entries() -> dict[str, str]:
    raw = (ROOT / "tests/brand/functional-icons.sha256").read_bytes()
    if hashlib.sha256(raw.replace(b"\r\n", b"\n")).hexdigest() != BASELINE_LF_SHA256:
        raise AssertionError("The 18-entry historical baseline was edited")
    entries = {}
    for line in raw.decode("utf-8").splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (src/assets/[^\r\n]+)", line)
        if not match or match[2] in entries:
            raise AssertionError("Malformed or duplicate historical baseline")
        entries[match[2]] = match[1]
    if len(entries) != 18:
        raise AssertionError("The historical baseline must contain exactly 18 images")
    return entries


def complete_block(text: str, needle: str) -> str:
    start = text.index(needle)
    opening = text.index("{", start)
    depth = 1
    cursor = opening + 1
    while cursor < len(text) and depth:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"Unclosed source block: {needle}")
    return text[opening + 1:cursor - 1]


class FunctionalIntegrityTests(unittest.TestCase):
    def test_historical_18_hashes(self):
        for path, expected in baseline_entries().items():
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected)

    def test_default_heads_and_external_marks_unchanged(self):
        for path, expected in load_fixture()["additionalImmutable"].items():
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected)

    def test_native_ui_key_mapping_and_fallback(self):
        # The legacy React sources are gone; the live mapping and fallback must stay identical.
        fixture = load_fixture()
        source = (ROOT / "native/ui/Icons.cpp").read_text(encoding="utf-8")
        block = complete_block(source, "QImage functionalImage(const QString& key)")
        aliases = dict(re.findall(r'\{"(\w+)",\s*"icons/([^"]+)"\}', block))
        for key in fixture["iconKeys"]:
            self.assertEqual(aliases.get(key), f"{key}.png", f"Native UI icon key vanished: {key}")
        self.assertEqual(fixture["defaultMod"], "src/assets/mod-placeholder.png")
        instances = (ROOT / "native/core/instances/instances.cpp").read_text(encoding="utf-8")
        self.assertEqual(re.search(r'if \(!has_json\) return "(\w+)";', complete_block(instances, "std::string pick_icon(")).group(1),
                         fixture["fallback"])

    def test_native_loader_and_type_inference(self):
        fixture = load_fixture()
        source = (ROOT / "native/core/instances/instances.cpp").read_text(encoding="utf-8")
        block = complete_block(source, "std::string pick_icon(")
        loaders = {name.lower(): icon.lower() for name, icon in re.findall(r'if \(loader == "(\w+)"\) return "(\w+)";', block)}
        self.assertEqual(loaders, fixture["loaderInference"])
        self.assertNotIn("optifabric", block.lower())
        self.assertIn('if (version_type == "release") return "grass";', block)
        self.assertIn('if (version_type == "snapshot") return "command_block";', block)
        self.assertIn('if (version_type == "old_beta" || version_type == "old_alpha") return "cobblestone";', block)
        self.assertLess(block.index("if (!has_json)"), block.index("is_april_fools"))
        self.assertLess(block.index("is_april_fools"), block.index('version_type == "release"'))

    def test_native_qrc_references_unchanged_original_bytes(self):
        fixture = load_fixture()
        qrc = ROOT / "native/assets/brand/functional-resources.qrc"
        tree = ET.parse(qrc).getroot()
        res = tree.find("qresource")
        self.assertEqual(res.attrib["prefix"], "/nickel/functional")
        entries = baseline_entries() | fixture["additionalImmutable"]
        covered = set()
        aliases = {}
        for row in res.findall("file"):
            target = (qrc.parent / row.text).resolve()
            self.assertTrue(target.is_relative_to(ROOT))
            path = target.relative_to(ROOT).as_posix()
            self.assertIn(path, entries, "Only old functional resources may be referenced")
            self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(), entries[path])
            self.assertNotIn(row.attrib["alias"], aliases)
            aliases[row.attrib["alias"]] = path
            covered.add(path)
        self.assertEqual(covered, set(entries))
        for key, path in fixture["iconKeys"].items():
            self.assertEqual(aliases[f"icons/{key}.png"], path)
        self.assertEqual(aliases["icons/optifabric.png"], fixture["optifabricDedicated"])
        self.assertEqual(aliases["mod-placeholder.png"], fixture["defaultMod"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
