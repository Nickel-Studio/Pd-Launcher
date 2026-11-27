"""Native build-tool safety tests use inert bytes and synthetic paths only."""
from __future__ import annotations

import ast
import importlib.util
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import audit_binary
import msvc


def pe_fixture(imports: list[str], delayed: list[str] | None = None) -> bytes:
    delayed = delayed or []
    image = bytearray(8192)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 128)
    image[128:132] = b"PE\0\0"
    struct.pack_into("<HH", image, 132, 0x8664, 1)
    struct.pack_into("<H", image, 148, 240)
    optional = 152
    struct.pack_into("<H", image, optional, 0x20B)
    struct.pack_into("<Q", image, optional + 24, 0x140000000)
    struct.pack_into("<I", image, optional + 60, 512)
    struct.pack_into("<I", image, optional + 108, 16)
    section = optional + 240
    struct.pack_into("<IIII", image, section + 8, 7168, 0x1000, 7168, 512)
    strings = 4096
    for index, name in enumerate(imports):
        raw = name.encode("ascii") + b"\0"
        image[strings:strings + len(raw)] = raw
        struct.pack_into("<IIIII", image, 512 + index * 20, 0, 0, 0, strings + 0x1000 - 512, 0)
        strings += len(raw)
    if imports:
        struct.pack_into("<II", image, optional + 112 + 8, 0x1000, (len(imports) + 1) * 20)
    for index, name in enumerate(delayed):
        raw = name.encode("ascii") + b"\0"
        image[strings:strings + len(raw)] = raw
        struct.pack_into("<IIIIIIII", image, 1536 + index * 32, 1, strings + 0x1000 - 512, 0, 0, 0, 0, 0, 0)
        strings += len(raw)
    if delayed:
        struct.pack_into("<II", image, optional + 112 + 13 * 8, 0x1400, (len(delayed) + 1) * 32)
    return bytes(image)


class BuildToolTests(unittest.TestCase):
    def test_source_lock_has_exact_versions_hashes_and_https(self):
        manifest = json.loads((HERE / "deps-lock.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["windowsRuntime"], "MultiThreaded")
        names = [source["name"] for source in manifest["sources"]]
        self.assertEqual(len(names), len(set(names)))
        for source in manifest["sources"]:
            self.assertRegex(source["sha256"], r"^[a-f0-9]{64}$")
            self.assertTrue(source["url"].startswith("https://"))
            self.assertNotIn("latest", source["url"])
            self.assertEqual(Path(source["archive"]).name, source["archive"])
            self.assertEqual(Path(source["sourceDirectory"]).name, source["sourceDirectory"])

    def test_build_scripts_do_not_enable_shell_for_external_commands(self):
        for filename in ("msvc.py", "bootstrap-deps.py", "audit_binary.py"):
            tree = ast.parse((HERE / filename).read_text(encoding="utf-8"))
            for call in (node for node in ast.walk(tree) if isinstance(node, ast.Call)):
                for keyword in call.keywords:
                    if keyword.arg == "shell":
                        self.assertIsInstance(keyword.value, ast.Constant)
                        self.assertFalse(keyword.value.value)

    def test_pe_normal_and_delayed_imports(self):
        result = audit_binary.pe_imports(pe_fixture(["KERNEL32.dll"], ["CRYPT32.dll"]))
        self.assertEqual(result, [{"name": "kernel32.dll", "kind": "import"}, {"name": "crypt32.dll", "kind": "delay"}])

    def test_pe_delay_browser_is_visible(self):
        result = audit_binary.pe_imports(pe_fixture([], ["WebView2Loader.dll"]))
        self.assertTrue(audit_binary.FORBIDDEN.search(result[0]["name"]))

    def test_pe_truncated_invalid_header_rejected(self):
        for value in (b"", b"MZ", pe_fixture([])[:153], b"MZ" + b"\xff" * 256):
            with self.assertRaises(audit_binary.AuditError):
                audit_binary.pe_imports(value)

    def test_pe_path_import_rejected(self):
        with self.assertRaises(audit_binary.AuditError):
            audit_binary.pe_imports(pe_fixture(["../Qt6Core.dll"]))

    def test_windows_wrapper_uses_the_independent_parser(self):
        data = pe_fixture(["KERNEL32.dll"], ["CRYPT32.dll"])
        with patch.object(audit_binary.WINDOWS_AUDITOR, "PeImage", wraps=audit_binary.WINDOWS_AUDITOR.PeImage) as parser:
            audit_binary.pe_imports(data)
            parser.assert_called_once_with(data)
        self.assertFalse(hasattr(audit_binary, "PEImage"))

    def test_unknown_import_fails_even_when_present_in_system_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            system = root / "System32"
            system.mkdir()
            executable = root / "launcher.exe"
            executable.write_bytes(pe_fixture(["not-a-system-component.dll"]))
            (system / "not-a-system-component.dll").write_bytes(pe_fixture([]))
            result = audit_binary.windows_audit(executable, windows_root=str(root))
            self.assertEqual(result["artifactAudit"]["importGate"], "failed")
            self.assertEqual(result["runtimeGate"], "unverified")
            self.assertFalse(result["importClosureComplete"])

    def test_no_webview_and_dynamic_crt_name_gates(self):
        for name in ("WebView2Loader.dll", "Qt6WebEngineCore.dll", "Qt6Qml.dll", "node.dll", "tauri.dll"):
            self.assertTrue(audit_binary.FORBIDDEN.search(name), name)
        for name in ("vcruntime140.dll", "msvcp140.dll", "Qt6Widgets.dll", "ucrtbase.dll", "vcomp140.dll"):
            self.assertTrue(audit_binary.DYNAMIC_QT_CRT.fullmatch(name), name)
        self.assertFalse(audit_binary.DYNAMIC_QT_CRT.fullmatch("kernel32.dll"))

    def test_system_trust_names_are_not_runtime_component_names(self):
        for name in ("wintrust.dll", "hvsifiletrust.dll", "api-ms-win-security-trustee-l1-1-0.dll", "ext-ms-onecore-shellchromeapi-l1-1-0.dll"):
            self.assertFalse(audit_binary.FORBIDDEN.search(name), name)

    def test_system_optional_imports_are_outside_release_closure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            system = root / "System32"
            system.mkdir()
            executable = root / "launcher.exe"
            executable.write_bytes(pe_fixture(["KERNEL32.dll"]))
            (system / "kernel32.dll").write_bytes(pe_fixture([], ["optional-os-library.dll"]))
            result = audit_binary.windows_audit(executable, windows_root=str(root))
            self.assertTrue(result["importClosureComplete"])
            self.assertEqual(len(result["nodes"]), 2)
            self.assertEqual(result["runtimeGate"], "unverified")

    def test_qt_computed_cache_gate_and_recovery_inputs(self):
        spec = importlib.util.spec_from_file_location("nickel_dependency_bootstrap_test", HERE / "bootstrap-deps.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        builder = module.Builder.__new__(module.Builder)
        builder.evidence = {}
        features = {"static": "ON", "shared": "OFF", "static_runtime": "ON",
                    "dynamicgl": "OFF", "opengl": "OFF", "network": "OFF",
                    "testlib": "ON", "gui": "ON", "widgets": "ON", "precompile_header": "OFF"}
        with tempfile.TemporaryDirectory() as directory, patch.object(builder, "save_evidence"):
            build = Path(directory)
            cache = build / "CMakeCache.txt"
            cache.write_text("\n".join(f"QT_FEATURE_{key}:INTERNAL={value}" for key, value in features.items()), encoding="ascii")
            builder.verify_qt_cache(build)
            for feature in ("network", "dynamicgl", "testlib"):
                invalid = dict(features)
                invalid[feature] = "OFF" if features[feature] == "ON" else "ON"
                cache.write_text("\n".join(f"QT_FEATURE_{key}:INTERNAL={value}" for key, value in invalid.items()), encoding="ascii")
                with self.assertRaisesRegex(RuntimeError, "Qt computed features"):
                    builder.verify_qt_cache(build)
        with patch.object(builder, "cmake_project") as configure:
            builder.build_qt()
            name, definitions = configure.call_args_list[0].args
            self.assertEqual(name, "qtbase")
            for required in ("-UQT_FEATURE_*", "-UQT_KNOWN_FEATURES", "-DFEATURE_testlib=ON", "-DINPUT_opengl=no"):
                self.assertIn(required, definitions)

    def test_static_zlib_checks_generic_and_per_configuration_locations(self):
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("CMake is not on PATH")
        helper = (HERE.parents[1] / "cmake" / "NickelStaticDependencies.cmake").as_posix()
        cases = (
            ("generic-static", {"IMPORTED_LOCATION": "zlibstatic.lib"}, True),
            ("release-static", {"IMPORTED_CONFIGURATIONS": "RELEASE", "IMPORTED_LOCATION_RELEASE": "zlibstatic.lib"}, True),
            ("release-import", {"IMPORTED_LOCATION_RELEASE": "zlib.lib"}, False),
            ("debug-import", {"IMPORTED_LOCATION_RELEASE": "zlibstatic.lib", "IMPORTED_LOCATION_DEBUG": "zlib.lib"}, False),
            ("explicit-implib", {"IMPORTED_LOCATION_RELEASE": "zlibstatic.lib", "IMPORTED_IMPLIB_RELEASE": "zlib.lib"}, False),
            ("missing-location", {}, False),
            ("missing-file", {"IMPORTED_LOCATION_RELEASE": "missing/zlibstatic.lib"}, False),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, properties, accepted in cases:
                with self.subTest(name=name):
                    source = root / name
                    source.mkdir()
                    (source / "zlibstatic.lib").write_bytes(b"inert static-location fixture")
                    (source / "zlib.lib").write_bytes(b"inert import-location fixture")
                    lines = ["cmake_minimum_required(VERSION 3.25)", "project(StaticPolicy LANGUAGES NONE)",
                             f'include("{helper}")', "add_library(Fixture::Zlib UNKNOWN IMPORTED)"]
                    for key, value in properties.items():
                        location = value if key == "IMPORTED_CONFIGURATIONS" else (source / value).as_posix()
                        lines.append(f'set_property(TARGET Fixture::Zlib PROPERTY {key} "{location}")')
                    lines.append("nickel_require_windows_static_zlib(Fixture::Zlib)")
                    (source / "CMakeLists.txt").write_text("\n".join(lines), encoding="utf-8")
                    result = subprocess.run([cmake, "-S", str(source), "-B", str(root / (name + "-build"))],
                                            capture_output=True, text=True, timeout=30, check=False)
                    self.assertEqual(result.returncode == 0, accepted, result.stdout + result.stderr)


    def test_unsafe_msvc_path_rejected_before_batch_execution(self):
        if sys.platform != "win32":
            self.skipTest("MSVC is Windows-only")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bad&environment"
            script = root / "Common7" / "Tools" / "VsDevCmd.bat"
            script.parent.mkdir(parents=True)
            script.write_text("not executable", encoding="ascii")
            with patch.object(msvc.subprocess, "run", side_effect=AssertionError("Must not spawn")):
                with self.assertRaises(ValueError):
                    msvc.developer_environment(root)

    def test_msvc_prefix_uses_measured_locale_and_ignores_depth(self):
        spec = importlib.util.spec_from_file_location("nickel_include_prefix_test", HERE / "msvc-includes.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        marker = "nickel_include_prefix_marker.h"
        for prefix in ("Note: including file:", "\u6ce8\u610f: \u5305\u542b\u6587\u4ef6:"):
            for depth in (1, 3, 8):
                output = f"{prefix}{' ' * depth}C:\\owned build\\{marker}\r\n".encode("utf-8")
                self.assertEqual(module.include_prefix(output, marker), prefix + " ")
        output = f"Note: including file:   \\\\server\\owned\\{marker}\r\n".encode("ascii")
        self.assertEqual(module.include_prefix(output, marker), "Note: including file: ")

    def test_msvc_prefix_rejects_missing_or_ambiguous_output(self):
        spec = importlib.util.spec_from_file_location("nickel_include_prefix_failure_test", HERE / "msvc-includes.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        marker = "nickel_include_prefix_marker.h"
        line = f"Note: including file: C:\\owned\\{marker}\n".encode("ascii")
        for output in (b"", b"compiler failed", line + line):
            with self.assertRaises(RuntimeError):
                module.include_prefix(output, marker)

    def test_production_entry_uses_real_services_without_startup_bypass(self):
        root = HERE.parents[1]
        entry = (root / "native" / "app" / "main.cpp").read_text(encoding="utf-8")
        build = (root / "native" / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("nickel::PathContext::system()", entry)
        self.assertIn("QCoreApplication::arguments()", entry)
        self.assertIn("nickel::app::AppServices services", entry)
        self.assertIn("nickel::ui::MainWindow window(services)", entry)
        for bypass in ("FinishOnboarding", "AcceptEula", "emit startupReady", "TestServices", "MockServices"):
            self.assertNotIn(bypass, entry)
        for target in ("Nickel::CommunityQt", "Nickel::AppServices", "Qt6::QWindowsIntegrationPlugin"):
            self.assertIn(target, build)
        self.assertIn("Missing production application source", build)
        self.assertNotIn("file(GLOB", build)

    def test_logged_commands_preserve_native_bytes_exit_and_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for code in (0, 7):
                log = root / f"command-{code}.log"
                evidence = root / f"command-{code}.json"
                command = [sys.executable, "-B", "-c",
                           "import sys; sys.stdout.buffer.write(b'native-output\\n'); "
                           "sys.stdout.buffer.flush(); sys.stderr.buffer.write(b'native-error\\n'); "
                           f"sys.stderr.buffer.flush(); sys.exit({code})"]
                environment = dict(msvc.os.environ)
                with patch.object(sys, "stdout", io.StringIO()):
                    actual = msvc.run_logged(command, environment, log, evidence)
                self.assertEqual(actual, code)
                self.assertEqual(log.read_bytes(), b"native-output\nnative-error\n")
                record = json.loads(evidence.read_text(encoding="utf-8"))
                self.assertEqual(record["returnCode"], code)
                self.assertTrue(record["complete"])
                self.assertEqual(record["logSha256"], audit_binary.sha256(log))
                self.assertEqual(record["argv"], command)
                with patch.object(msvc.subprocess, "run", side_effect=AssertionError("Must not spawn")):
                    with self.assertRaises(ValueError):
                        msvc.run_logged(command, environment, log, evidence)

    def test_logged_commands_do_not_overwrite_inert_existing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "existing.json"
            report.write_text("inert existing evidence", encoding="ascii")
            with patch.object(msvc.subprocess, "run", side_effect=AssertionError("Must not spawn")):
                with self.assertRaises(ValueError):
                    msvc.run_logged([sys.executable], {}, root / "new.log", report)
            self.assertEqual(report.read_text(encoding="ascii"), "inert existing evidence")




if __name__ == "__main__":
    unittest.main()
