"""Tests of the independent acceptance tooling, not product acceptance."""
from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

import audit_windows
import generate_fixtures


def sample_pe(normal=None, delay=None, bits=64) -> bytes:
    normal = normal or []
    delay = delay or []
    data = bytearray(8192)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    pe = 0x80
    data[pe:pe + 4] = b"PE\0\0"
    opt_size = 240 if bits == 64 else 224
    struct.pack_into("<HH", data, pe + 4, 0x8664 if bits == 64 else 0x14C, 1)
    struct.pack_into("<H", data, pe + 20, opt_size)
    opt = pe + 24
    struct.pack_into("<H", data, opt, 0x20B if bits == 64 else 0x10B)
    if bits == 64:
        struct.pack_into("<Q", data, opt + 24, 0x140000000)
        directories, count_at = 112, 108
    else:
        struct.pack_into("<I", data, opt + 28, 0x400000)
        directories, count_at = 96, 92
    struct.pack_into("<I", data, opt + 60, 0x400)
    struct.pack_into("<H", data, opt + 68, 2)
    struct.pack_into("<I", data, opt + count_at, 16)
    section = opt + opt_size
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x1C00, 0x1000, 0x1C00, 0x400)
    normal_rva, delay_rva, name_rva = 0x1100, 0x1300, 0x1800
    for is_delay, names, rva in [(False, normal, normal_rva), (True, delay, delay_rva)]:
        if not names:
            continue
        stride = 32 if is_delay else 20
        struct.pack_into("<II", data, opt + directories + (13 if is_delay else 1) * 8,
                         rva, (len(names) + 1) * stride)
        for index, name in enumerate(names):
            encoded = name.encode("ascii") + b"\0"
            name_at = 0x400 + name_rva - 0x1000
            data[name_at:name_at + len(encoded)] = encoded
            descriptor = 0x400 + rva - 0x1000 + index * stride
            if is_delay:
                struct.pack_into("<II", data, descriptor, 1, name_rva)
            else:
                struct.pack_into("<I", data, descriptor + 12, name_rva)
            name_rva += len(encoded)
    return bytes(data)


class FixtureTools(unittest.TestCase):
    def test_deterministic_and_hashes(self):
        with tempfile.TemporaryDirectory(prefix="nickel-fixture-tools-") as directory:
            root = Path(directory)
            first = generate_fixtures.write_fixtures(root / "first")
            second = generate_fixtures.write_fixtures(root / "second")
            self.assertEqual(first, second)
            for name, digest in first["files"].items():
                raw = (root / "first" / name).read_bytes()
                self.assertEqual(hashlib.sha256(raw).hexdigest(), digest)
                raw.decode("ascii")
                self.assertEqual(raw, (root / "second" / name).read_bytes())

    def test_gcm_valid_vectors(self):
        for vector in generate_fixtures.vault_vectors()["valid"]:
            key = bytes.fromhex(vector["keyHex"])
            payload = base64.b64decode(vector["sealed"][3:], validate=True)
            self.assertEqual(AESGCM(key).decrypt(payload[:12], payload[12:], None).decode("utf-8"),
                             vector["plaintext"])
            self.assertEqual(len(payload), 12 + len(vector["plaintext"].encode("utf-8")) + 16)

    def test_gcm_invalid_vectors(self):
        vectors = generate_fixtures.vault_vectors()["invalid"]
        for vector in vectors:
            with self.subTest(vector=vector["name"]):
                with self.assertRaises((ValueError, InvalidTag, UnicodeDecodeError)):
                    if not vector["sealed"].startswith("v1:"):
                        raise ValueError("Unknown prefix")
                    payload = base64.b64decode(vector["sealed"][3:], validate=True)
                    if len(payload) < 28:
                        raise ValueError("Truncated ciphertext/tag")
                    AESGCM(bytes.fromhex(vector["keyHex"])).decrypt(payload[:12], payload[12:], None).decode("utf-8")

    def test_uint64_does_not_become_double(self):
        vector = generate_fixtures.settings_vectors()["valid"][1]
        parsed = json.loads(vector["text"])
        self.assertEqual(parsed["future"]["integerBeyondDouble"], 9007199254740993)
        self.assertEqual(parsed["future"]["unsignedMax"], 18446744073709551615)
        self.assertEqual(parsed, vector["expected"])

    def test_fixture_paths_have_no_external_escape(self):
        for case in generate_fixtures.path_vectors()["cases"]:
            for path in [case["executable"], *case["files"], *case["directories"],
                         case["expectedConfig"], case["expectedGame"], case["expectedBundledGame"]]:
                self.assertFalse(Path(path).is_absolute())
                self.assertNotIn("..", Path(path).parts)


class PeTools(unittest.TestCase):
    def test_normal_and_delay_imports(self):
        for bits in [32, 64]:
            pe = audit_windows.PeImage(sample_pe(["KERNEL32.dll", "WS2_32.dll"], ["USER32.dll"], bits))
            self.assertEqual(pe.bitness, bits)
            self.assertEqual(pe.imports(), ["kernel32.dll", "ws2_32.dll"])
            self.assertEqual(pe.imports(True), ["user32.dll"])
            self.assertEqual(audit_windows.check_imports(pe.imports(), pe.imports(True)), [])

    def test_runtime_dlls_rejected(self):
        for name in ["Qt6Core.dll", "Qt6Widgets.dll", "libcurl.dll", "vcruntime140.dll", "Qt6WebEngineCore.dll",
                     "WebView2Loader.dll", "Qt6Qml.dll", "Qt6Quick.dll", "node.exe", "nickel-rust.dll"]:
            self.assertTrue(audit_windows.check_imports([name.lower()], []), name)

    def test_delay_dependency_not_omitted(self):
        pe = audit_windows.PeImage(sample_pe(["kernel32.dll"], ["Qt6Core.dll"]))
        self.assertTrue(audit_windows.check_imports(pe.imports(), pe.imports(True)))

    def test_header_and_rva_fail_closed(self):
        valid = sample_pe(["kernel32.dll"])
        for size in [0, 1, 63, 128, 160, 512]:
            with self.subTest(size=size), self.assertRaises(audit_windows.PeFormatError):
                audit_windows.PeImage(valid[:size])
        malformed = bytearray(valid)
        struct.pack_into("<I", malformed, 0x3C, 0xFFFFFFF0)
        with self.assertRaises(audit_windows.PeFormatError):
            audit_windows.PeImage(bytes(malformed))
        pe = audit_windows.PeImage(valid)
        with self.assertRaises(audit_windows.PeFormatError):
            pe.offset(0xFFFFFFFF, 8)
        with self.assertRaises(audit_windows.PeFormatError):
            pe.offset(0x5000)

    def test_names_cannot_escape_filename(self):
        for name in ["../bad.dll", "C:\\Qt6Core.dll"]:
            pe = audit_windows.PeImage(sample_pe([name]))
            with self.assertRaises(audit_windows.PeFormatError):
                pe.imports()

    def test_no_import_terminator_is_rejected(self):
        data = bytearray(sample_pe(["kernel32.dll"]))
        opt = 0x80 + 24
        struct.pack_into("<I", data, opt + 112 + 1 * 8 + 4, 20)
        with self.assertRaises(audit_windows.PeFormatError):
            audit_windows.PeImage(bytes(data)).imports()

    def test_modules_must_be_system_or_exact_exe(self):
        evidence = {"schemaVersion": 1, "executableSha256": "abc", "snapshots": [{
            "pid": 98765, "scenario": "home-idle", "modules": ["C:\\isolated\\Nickel.exe", "C:\\Windows\\System32\\kernel32.dll"]}]}
        self.assertEqual(audit_windows.audit_modules(evidence, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"), [])
        for extra in ["C:\\isolated\\Qt6Core.dll", "C:\\Windows\\System32\\..\\Temp\\injected.dll",
                      "C:\\Windows\\System32\\WebView2Loader.dll", "C:\\Windows\\System32\\vcruntime140.dll"]:
            altered = json.loads(json.dumps(evidence))
            altered["snapshots"][0]["modules"].append(extra)
            self.assertTrue(audit_windows.audit_modules(altered, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"))

    def test_runtime_gate_never_passes_on_absent_evidence(self):
        with tempfile.TemporaryDirectory(prefix="nickel-pe-tools-") as directory:
            path = Path(directory) / "synthetic.exe"
            path.write_bytes(sample_pe(["kernel32.dll"]))
            result = audit_windows.audit(path)
            self.assertEqual(result["importGate"], "passed")
            self.assertEqual(result["runtimeGate"], "unverified")
            self.assertEqual(result["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_empty_and_mismatched_modules_fail(self):
        for evidence in [{}, {"schemaVersion": 1, "executableSha256": "other", "snapshots": []}]:
            self.assertTrue(audit_windows.audit_modules(evidence, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"))

    def test_documented_exact_system_dlls_include_normal_and_delay_imports(self):
        names = ["authz.dll", "netapi32.dll", "uiautomationcore.dll", "xmllite.dll"]
        evidence = json.loads((Path(__file__).parent / "windows-system-imports.json").read_text())
        self.assertEqual(set(evidence["libraries"]), set(names))
        for name in names:
            record = evidence["libraries"][name]
            self.assertTrue(record["url"].startswith("https://learn.microsoft.com/"))
            self.assertTrue(record["minimumClient"])
            self.assertTrue(record["minimumServer"])
            self.assertIn(name, record["requirementsQuote"].lower())
            pe = audit_windows.PeImage(sample_pe([name.upper()], [name.upper()]))
            self.assertEqual(audit_windows.check_imports(pe.imports(), pe.imports(True)), [])
            for suffix in (".dll", ".evil.dll", "64.dll"):
                self.assertTrue(audit_windows.check_imports([name + suffix], []))

    def test_system_name_imports_do_not_exempt_app_local_runtime_dlls(self):
        names = ["authz.dll", "netapi32.dll", "uiautomationcore.dll", "xmllite.dll"]
        evidence = {"schemaVersion": 1, "executableSha256": "abc", "snapshots": [{
            "pid": 98765, "scenario": "home-idle", "modules": ["C:\\isolated\\Nickel.exe"]
        }]}
        for name in names:
            accepted = json.loads(json.dumps(evidence))
            accepted["snapshots"][0]["modules"].append("C:\\Windows\\System32\\" + name)
            self.assertEqual(audit_windows.audit_modules(accepted, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"), [])
            rejected = json.loads(json.dumps(evidence))
            rejected["snapshots"][0]["modules"].append("C:\\isolated\\" + name)
            self.assertTrue(audit_windows.audit_modules(rejected, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"))

    def test_wintrust_does_not_match_rust_component_policy(self):
        pe = audit_windows.PeImage(sample_pe(["WINTRUST.dll"], ["WINTRUST.dll"]))
        self.assertIsNone(audit_windows.FORBIDDEN.search("WINTRUST.dll"))
        self.assertEqual(audit_windows.check_imports(pe.imports(), pe.imports(True)), [])
        evidence = {"schemaVersion": 1, "executableSha256": "abc", "snapshots": [{
            "pid": 98765, "scenario": "onboarding", "modules": [
                "C:\\isolated\\Nickel.exe", "C:\\Windows\\System32\\WINTRUST.dll"
            ]
        }]}
        self.assertEqual(audit_windows.audit_modules(evidence, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"), [])

    def test_component_boundaries_still_reject_forbidden_engines(self):
        names = ["rustsidecar.exe", "nickel-rust.dll", "librust_runtime.dll", "libnode.dll", "node.exe",
                 "Qt6Quick.dll", "Qt6Qml.dll", "Qt6WebEngineCore.dll", "WebView2Loader.dll",
                 "nickel_tauri.dll", "JavaScriptCore.dll", "libjavascript.dll"]
        for name in names:
            with self.subTest(name=name):
                self.assertIsNotNone(audit_windows.FORBIDDEN.search(name))
                self.assertTrue(audit_windows.check_imports([name.lower()], []))
                evidence = {"schemaVersion": 1, "executableSha256": "abc", "snapshots": [{
                    "pid": 98765, "scenario": "onboarding", "modules": [
                        "C:\\isolated\\Nickel.exe", "C:\\Windows\\System32\\" + name
                    ], "children": [{"purpose": "java-probe", "executable": "C:\\probe\\" + name}]
                }]}
                self.assertTrue(audit_windows.audit_modules(evidence, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"))

    def test_prefixed_webview_names_rejected_as_import_module_and_child(self):
        names = ["msedgewebview2.exe", "msedgewebview2.dll", "EmbeddedWebView.dll",
                 "Microsoft.Web.WebView2.Core.dll", "CustomWebEngine.dll"]
        for name in names:
            with self.subTest(name=name):
                self.assertIsNotNone(audit_windows.FORBIDDEN.search(name))
                self.assertTrue(audit_windows.check_imports([name.lower()], []))
                self.assertTrue(audit_windows.check_imports([], [name.lower()]))
                base = {"schemaVersion": 1, "executableSha256": "abc", "snapshots": [{
                    "pid": 98765, "scenario": "onboarding", "modules": [
                        "C:\\isolated\\Nickel.exe", "C:\\Windows\\System32\\WINTRUST.dll"
                    ]
                }]}
                self.assertEqual(audit_windows.audit_modules(base, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"), [])
                module = json.loads(json.dumps(base))
                module["snapshots"][0]["modules"].append("C:\\Windows\\System32\\" + name)
                self.assertTrue(audit_windows.audit_modules(module, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"))
                child = json.loads(json.dumps(base))
                child["snapshots"][0]["children"] = [{"purpose": "browser", "executable": "C:\\probe\\" + name}]
                self.assertTrue(audit_windows.audit_modules(child, "C:\\isolated\\Nickel.exe", "abc", "C:\\Windows"))




if __name__ == "__main__":
    unittest.main(verbosity=2)
