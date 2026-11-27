#!/usr/bin/env python3
"""Synthetic install/ABI/symbol gates. These tests never attest an Apple build."""
from __future__ import annotations

import ast
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("macos_install_api", ROOT / "scripts/native/macos-delivery.py")
api = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(api)
UUID = "12345678-1234-4321-ABCD-123456789ABC"
MAGIC = b"\xcf\xfa\xed\xfe"


class SyntheticRecorder:
    def __init__(self, output, initial):
        self.output = output
        self.record = {"commands": [], "complete": False, **initial}
        self.environment = []

    def save(self):
        pass

    def finish(self, **values):
        self.record.update(complete=True, **values)

    def run(self, argv, cwd, environment, timeout=30):
        self.record["commands"].append({"argv": argv, "cwd": str(cwd), "log": "synthetic.log", "logSha256": "a" * 64})
        self.environment.append(dict(environment))
        if "--install" in argv:
            build = Path(argv[argv.index("--install") + 1])
            prefix = Path(argv[argv.index("--prefix") + 1])
            prefix.mkdir()
            original = build / "Real Pd.app"
            shutil.copytree(original, prefix / original.name)
            files = [path for path in (prefix / original.name).rglob("*") if path.is_file()]
            (build / "install_manifest.txt").write_text("\n".join(str(path) for path in files), encoding="utf-8")
        elif argv[0] == "ditto":
            Path(argv[-1]).write_bytes(b"synthetic-package-not-a-real-app")
        return ""


class SyntheticSymbolRecorder(SyntheticRecorder):
    def __init__(self, architecture="arm64", mismatch=False, skeleton=False):
        super().__init__(None, {})
        self.architecture = architecture
        self.mismatch = mismatch
        self.skeleton = skeleton

    def run(self, argv, cwd, environment, timeout=30):
        super().run(argv, cwd, environment, timeout)
        if "dsymutil" in argv:
            dwarf = Path(argv[-1]) / "Contents/Resources/DWARF" / Path(argv[2]).name
            dwarf.parent.mkdir(parents=True)
            dwarf.write_bytes(b"synthetic-dwarf" * 128)
        if "--uuid" in argv:
            architecture = "x86_64" if self.mismatch and str(argv[-1]).endswith(".dSYM") else self.architecture
            return "UUID: " + UUID + " (" + architecture + ") " + str(argv[-1]) + "\n"
        if "--debug-info" in argv:
            return "DW_TAG_compile_unit\nDW_AT_name (\"actual-fixture-source.cpp\")\n" if not self.skeleton else "UUID-only skeleton\n"
        return ""


class InstallDeliveryGates(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="install-graph-unit-", dir=Path(__file__).parent)
        self.root = Path(self.temp.name).absolute()
        self.build = self.root / "app-build"
        self.build.mkdir()
        self.original = self.bundle(self.build / "Real Pd.app")
        self.prefix = self.root / "product-install"
        self.qt = self.root / "qt-prefix"
        self.file(self.qt / "bin/macdeployqt", b"synthetic-deployer")
        self.file(self.build / "cmake_install.cmake", b'include("app/cmake_install.cmake")\n')
        self.file(self.build / "app/cmake_install.cmake", b'include("qt_deploy_app.cmake")\n')
        self.file(self.build / "app/qt_deploy_app.cmake", b"# synthetic Qt deploy graph\n")
        self.file(self.build / "compile_commands.json", b"[]\n")
        self.inputs = api.validate_inputs(ROOT / "scripts/native/macos-delivery.inputs.json")

    def tearDown(self):
        self.temp.cleanup()

    def file(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o644)
        return path

    def bundle(self, path, identifier="studio.nickel.launcher", executable="pd-launcher"):
        self.file(path / "Contents/Info.plist", plistlib.dumps({
            "CFBundleIdentifier": identifier, "CFBundleExecutable": executable,
            "CFBundleVersion": "1.0.0", "CFBundleShortVersionString": "1.0.0", "LSMinimumSystemVersion": "12.0"}))
        binary = self.file(path / "Contents/MacOS" / executable, MAGIC + b"synthetic-binary-not-compiled")
        binary.chmod(0o755)
        return path

    def install(self, second=None):
        self.prefix.mkdir()
        installed = self.bundle(self.prefix / "Real Pd.app")
        paths = list(installed.rglob("*"))
        if second:
            paths.extend(self.bundle(self.prefix / second).rglob("*"))
        self.manifest([path for path in paths if path.is_file()])
        return installed

    def manifest(self, paths):
        return self.file(self.build / "install_manifest.txt", ("\n".join(str(path) for path in paths) + "\n").encode("utf-8"))

    def options(self):
        qt_evidence = self.root / "qt.json"
        api.write_json(qt_evidence, {"complete": True, "architecture": "arm64", "qtVersion": "6.8.3", "prefix": str(self.qt)})
        binary = self.original / "Contents/MacOS/pd-launcher"
        build_evidence = self.root / "app.json"
        source = self.root / "source"
        source.mkdir(exist_ok=True)
        api.write_json(build_evidence, {"complete": True, "kind": "production-source-build", "architecture": "arm64",
            "buildRoot": str(self.build), "sourceRoot": str(source), "bundle": str(self.original), "executable": str(binary),
            "resolvedSourceCommit": "b" * 40, "executableSha256": api.digest(binary),
            "compileCommandsSha256": api.digest(self.build / "compile_commands.json"),
            "installationGraph": api.generated_installation_graph(self.build), "qtEvidenceSha256": api.digest(qt_evidence)})
        return SimpleNamespace(arch="arm64", build_evidence=build_evidence, qt_evidence=qt_evidence,
            install_root=self.prefix, symbols_root=self.root / "symbols", archive=self.root / "package.zip",
            output=self.root / "deployment.json")

    def test_generated_graph_preserves_actual_script_bytes(self):
        graph = api.generated_installation_graph(self.build)
        output = self.root / "raw-install-graph"
        api.preserve_installation_graph(self.build, graph, output)
        self.assertEqual(len(graph["files"]), 3)
        for item in graph["files"]:
            self.assertEqual(api.digest(output / item["path"]), item["sha256"])
        with self.assertRaises(api.DeliveryError):
            api.preserve_installation_graph(self.build, graph, output)

    def test_missing_deploy_graph_and_changed_bytes_rejected(self):
        graph = api.generated_installation_graph(self.build)
        self.file(self.build / "app/qt_deploy_app.cmake", b"changed\n")
        with self.assertRaisesRegex(api.DeliveryError, "changed"):
            api.preserve_installation_graph(self.build, graph, self.root / "preserved")
        with patch.object(api, "MAX_SMALL_INPUT", 1), self.assertRaises(api.DeliveryError):
            api.generated_installation_graph(self.build)

    def test_unique_bundle_selected_from_manifest_not_filename_guess(self):
        installed = self.install()
        unused = self.bundle(self.prefix / "unlisted.app")
        actual, receipt = api.installed_production_bundle(self.build, self.prefix, self.original)
        self.assertEqual(actual, installed)
        self.assertNotEqual(actual, unused)
        self.assertEqual(receipt["productionIdentity"]["CFBundleExecutable"], "pd-launcher")
        self.assertEqual(receipt["sha256"], api.digest(self.build / "install_manifest.txt"))

    def test_probe_identity_not_product_and_ambiguous_product_rejected(self):
        self.install(second="duplicate.app")
        with self.assertRaisesRegex(api.DeliveryError, "exactly one"):
            api.installed_production_bundle(self.build, self.prefix, self.original)
        probe = self.bundle(self.prefix / "Probe.app", identifier="studio.nickel.launcher.macos-probe")
        self.manifest([path for path in probe.rglob("*") if path.is_file()])
        with self.assertRaisesRegex(api.DeliveryError, "exactly one"):
            api.installed_production_bundle(self.build, self.prefix, self.original)

    def test_manifest_empty_missing_relative_escape_and_external_rejected(self):
        installed = self.install()
        external = self.file(self.root / "outside.txt", b"external")
        for entries in ([], ["relative.app/Contents/Info.plist"], [self.prefix / "missing"], [external],
                        [self.prefix / ".." / "outside.txt"]):
            with self.subTest(entries=entries):
                self.manifest(entries)
                with self.assertRaises(api.DeliveryError):
                    api.installed_production_bundle(self.build, self.prefix, self.original)
        self.manifest([installed / "Contents/Info.plist"])
        with self.assertRaisesRegex(api.DeliveryError, "absent"):
            api.installed_production_bundle(self.build, self.prefix, self.original)

    def test_manifest_executable_must_have_listed_plist_and_matching_version(self):
        installed = self.install()
        self.manifest([installed / "Contents/MacOS/pd-launcher"])
        with self.assertRaisesRegex(api.DeliveryError, "absent"):
            api.installed_production_bundle(self.build, self.prefix, self.original)
        info_path = installed / "Contents/Info.plist"
        info = plistlib.loads(info_path.read_bytes())
        info["CFBundleVersion"] = "another-build"
        info_path.write_bytes(plistlib.dumps(info))
        self.manifest([info_path, installed / "Contents/MacOS/pd-launcher"])
        with self.assertRaisesRegex(api.DeliveryError, "exactly one"):
            api.installed_production_bundle(self.build, self.prefix, self.original)

    @unittest.skipIf(os.name == "nt", "Windows symlink privilege is not assumed")
    def test_linked_install_bundle_and_deploy_script_rejected(self):
        self.prefix.mkdir()
        linked = self.prefix / "linked.app"
        linked.symlink_to(self.original, target_is_directory=True)
        self.manifest([linked / "Contents/Info.plist", linked / "Contents/MacOS/pd-launcher"])
        with self.assertRaises(api.DeliveryError):
            api.installed_production_bundle(self.build, self.prefix, self.original)
        linked_script = self.build / "linked-deploy.cmake"
        linked_script.symlink_to(self.build / "cmake_install.cmake")
        with self.assertRaises(api.DeliveryError):
            api.generated_installation_graph(self.build)

    def test_deploy_installs_same_build_before_macdeployqt_and_never_mutates_build(self):
        options = self.options()
        with patch.object(api, "apple_environment", return_value=({"DESTDIR": "evil", "CMAKE_INSTALL_MODE": "SYMLINK"}, {})), \
             patch.object(api.shutil, "which", return_value="cmake"), patch.object(api, "Recorder", SyntheticRecorder), \
             patch.object(api, "audit_bundle", return_value={"nodes": []}), \
             patch.object(api, "deployed_plugin_evidence", return_value=[]), patch.object(api, "deployed_symbol_evidence", return_value=[]):
            receipt = api.deploy(options, self.inputs)
        commands = receipt["commands"]
        self.assertEqual(commands[0]["argv"], ["cmake", "--install", str(self.build), "--prefix", str(self.prefix), "--config", "Release"])
        self.assertEqual(Path(commands[1]["argv"][1]), self.prefix / self.original.name)
        self.assertNotEqual(Path(commands[1]["argv"][1]), self.original)
        self.assertEqual(api.digest(self.original / "Contents/MacOS/pd-launcher"), receipt["builtExecutableSha256"])
        self.assertFalse(receipt["appleRuntimeExecuted"])
        self.assertFalse(receipt["productDelivered"])

    def test_install_and_compile_graph_change_fail_before_install_command(self):
        options = self.options()
        self.file(self.build / "cmake_install.cmake", b"changed")
        with patch.object(api, "apple_environment", return_value=({}, {})), \
             self.assertRaisesRegex(api.DeliveryError, "install graph changed"):
            api.deploy(options, self.inputs)
        options = self.options()
        self.file(self.build / "compile_commands.json", b"changed compilation")
        with patch.object(api, "apple_environment", return_value=({}, {})), \
             self.assertRaisesRegex(api.DeliveryError, "compilation commands changed"):
            api.deploy(options, self.inputs)

    def test_fresh_prefix_required_and_never_inside_source_or_qt(self):
        options = self.options()
        for root in (self.build / "installed", self.qt / "installed", self.root / "source" / "installed", self.build):
            options.install_root = root
            with patch.object(api, "apple_environment", return_value=({}, {})), \
                 self.assertRaisesRegex(api.DeliveryError, "fresh and outside"):
                api.deploy(options, self.inputs)

    def test_plugin_inventory_includes_real_imageformats_and_svg_icon(self):
        installed = self.install()
        plugins = ["platforms/libqcocoa.dylib", "iconengines/libqsvgicon.dylib"] + [
            "imageformats/lib" + name + ".dylib" for name in ("qico", "qjpeg", "qgif", "qsvg", "qwebp", "qtiff", "qicns", "qwbmp")]
        nodes = []
        for plugin in plugins:
            self.file(self.qt / "plugins" / plugin, MAGIC + b"source")
            path = self.file(installed / "Contents/PlugIns" / plugin, MAGIC + b"deployed")
            nodes.append({"path": path.relative_to(installed).as_posix(), "sha256": api.digest(path), "architecture": ["arm64"]})
        audit = {"architecture": "arm64", "nodes": nodes}
        records = api.deployed_plugin_evidence(installed, self.qt, audit, self.inputs["policy"])
        self.assertEqual(len(records), len(plugins))
        self.assertTrue(any(item["plugin"] == "iconengines/libqsvgicon.dylib" for item in records))
        self.assertTrue(all(not item["runtimePluginLoadExecuted"] for item in records))
        nodes[0]["architecture"] = ["x86_64"]
        with self.assertRaisesRegex(api.DeliveryError, "architecture"):
            api.deployed_plugin_evidence(installed, self.qt, audit, self.inputs["policy"])

    def test_absent_source_imageformat_plugin_is_not_a_pass(self):
        installed = self.install()
        (self.qt / "plugins/imageformats").mkdir(parents=True)
        (self.qt / "plugins/iconengines").mkdir()
        with self.assertRaisesRegex(api.DeliveryError, "missing required"):
            api.deployed_plugin_evidence(installed, self.qt, {"nodes": []}, self.inputs["policy"])

    def test_symbol_uuid_and_architecture_match_unstripped_source(self):
        installed = self.install()
        recorder = SyntheticSymbolRecorder()
        # Symbol matching is synthetic; NTFS/DrvFS permission bits are not an Apple bundle audit.
        inventory = lambda bundle: ([], [bundle / "Contents/MacOS/pd-launcher"])
        with patch.object(api, "bundle_inventory", side_effect=inventory):
            records = api.deployed_symbol_evidence(installed, self.original, self.qt, self.root / "symbols", recorder, {})
        self.assertEqual(records[0]["debugSourceImage"], str(self.original / "Contents/MacOS/pd-launcher"))
        self.assertEqual(records[0]["actualCompilationUnits"], 1)
        self.assertEqual(records[0]["uuids"], [(UUID, "arm64")])
        dsym = next(item for item in recorder.record["commands"] if "dsymutil" in item["argv"])
        self.assertEqual(Path(dsym["argv"][2]), self.original / "Contents/MacOS/pd-launcher")

    def test_uuid_only_skeleton_and_wrong_symbol_architecture_rejected(self):
        installed = self.install()
        inventory = lambda bundle: ([], [bundle / "Contents/MacOS/pd-launcher"])
        for label, recorder in (("skeleton", SyntheticSymbolRecorder(skeleton=True)), ("wrong-arch", SyntheticSymbolRecorder(mismatch=True))):
            with self.subTest(label=label), patch.object(api, "bundle_inventory", side_effect=inventory), self.assertRaises(api.DeliveryError):
                api.deployed_symbol_evidence(installed, self.original, self.qt, self.root / label, recorder, {})

    def test_symbols_require_nonempty_architecture_pairs(self):
        self.assertEqual(api.macho_uuid_pairs("UUID: " + UUID.lower() + " (x86_64) binary"), {(UUID, "x86_64")})
        for text in ("", "UUID: fake (arm64)", "UUID: " + UUID + " (arm64e)"):
            with self.subTest(text=text), self.assertRaises(api.DeliveryError):
                api.macho_uuid_pairs(text)

    def test_system_abi_versions_and_platform_are_actual_load_commands(self):
        text = "Pd:\n\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 1900.150.0)\n"
        abi = api.parse_otool_abi_versions(text)
        self.assertEqual(abi[0]["compatibilityVersion"], "1.0.0")
        self.assertEqual(abi[0]["currentVersion"], "1900.150.0")
        self.assertTrue(api.system_library(abi[0]["name"]))
        with self.assertRaises(api.DeliveryError):
            api.parse_otool_abi_versions(text.replace("1900.150.0", "unknown"))
        load = api.parse_load_commands("cmd LC_BUILD_VERSION\nplatform MACOS\nminos 12.0\nsdk 15.5\n")
        self.assertEqual(load["platforms"], ["MACOS"])
        self.assertEqual(load["sdkVersions"], ["15.5"])

    def test_cli_and_ci_have_explicit_install_contract(self):
        result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/native/macos-delivery.py"), "deploy", "--help"],
                                capture_output=True, text=True, check=True, timeout=15)
        self.assertIn("--install-root", result.stdout)
        ci = (ROOT / "scripts/native/macos-delivery.ci.py").read_text(encoding="utf-8-sig")
        self.assertIn('install_root=options.work_root / "product-install"', ci)
        tree = ast.parse((ROOT / "scripts/native/macos-delivery.py").read_text(encoding="utf-8-sig"))
        build_app = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "build_app")
        text = ast.get_source_segment((ROOT / "scripts/native/macos-delivery.py").read_text(encoding="utf-8-sig"), build_app)
        self.assertIn('"buildRoot": str(build)', text)
        self.assertIn('installationGraph=generated_installation_graph(build)', text)
        self.assertIn('"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"', text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
