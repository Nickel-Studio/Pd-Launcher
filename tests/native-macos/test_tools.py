#!/usr/bin/env python3
"""Host-independent fixture-wheel gates; never Apple compilation or runtime evidence."""
from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


api = load("fixture_delivery_api", "scripts/native/macos-delivery.py")
tools = load("fixture_test_tools", "scripts/native/macos-delivery.test-tools.py")


class FixtureToolGates(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="macos-fixture-tools-", dir=Path(__file__).parent)
        self.root = Path(self.temporary.name)
        self.site = self.root / "site"
        self.site.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def wheel(self, entries, version="2.23", metadata=True):
        archive = self.root / "synthetic.whl"
        with zipfile.ZipFile(archive, "w") as output:
            for name, content in entries:
                output.writestr(name, content)
            if metadata:
                output.writestr("pycparser-2.23.dist-info/METADATA", "Metadata-Version: 2.1\nName: pycparser\nVersion: " + version + "\n")
        return archive, {"name": "pycparser", "version": "2.23", "bytes": archive.stat().st_size, "sha256": api.digest(archive)}

    def reviewed_wheel(self, name, version, requirements, filename=None):
        archive = self.root / (filename or (name + ".whl"))
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr(name + "/__init__.py", b"synthetic fixture package")
            output.writestr(name + "-" + version + ".dist-info/METADATA",
                            "Metadata-Version: 2.1\nName: " + name + "\nVersion: " + version + "\n" +
                            "".join("Requires-Dist: " + item + "\n" for item in requirements))
        return archive, {"name": name, "version": version, "bytes": archive.stat().st_size, "sha256": api.digest(archive)}


    def test_current_regular_cp312_pins_are_exact_and_dual_native(self):
        pins = tools.read_pins(ROOT / "scripts/native/macos-delivery.test-tools.json", api)
        self.assertEqual(pins["supportedPythonMinors"], [12])
        self.assertEqual(len(pins["sources"]), 4)
        for architecture in api.ARCHITECTURES:
            selected = tools.select_wheels(pins, architecture, 12, "CPython", api)
            self.assertEqual({item["name"]: item["version"] for item in selected}, tools.VERSIONS)
            self.assertTrue(all(item["role"] == "independent-test-fixture-tool-not-product-dependency" for item in selected))
            cffi = next(item for item in selected if item["name"] == "cffi")
            self.assertIn(architecture, cffi["archive"])
            self.assertIn("cp312-cp312", cffi["archive"])
        crypto = next(item for item in pins["sources"] if item["name"] == "cryptography")
        self.assertEqual(crypto["bytes"], 8008324)
        self.assertEqual(crypto["sha256"], "3e4a1a3232eef2e6c732827d5722db29a0cc8b27af2a4d865b094cf954be9ca1")

    def test_unsupported_interpreters_and_incomplete_or_duplicate_sets_fail(self):
        pins = tools.read_pins(ROOT / "scripts/native/macos-delivery.test-tools.json", api)
        for architecture, minor, implementation in [("universal2", 12, "CPython"), ("arm64", 13, "CPython"), ("arm64", 12, "PyPy")]:
            with self.assertRaises(api.DeliveryError):
                tools.select_wheels(pins, architecture, minor, implementation, api)
        for sources in (pins["sources"][:-1], pins["sources"] + pins["sources"][:1]):
            broken = copy.deepcopy(pins)
            broken["sources"] = sources
            with self.assertRaises(api.DeliveryError):
                tools.select_wheels(broken, "x86_64", 12, "CPython", api)

    def test_live_pin_hash_drift_is_rejected(self):
        source = self.root / "changed-pins.json"
        source.write_bytes((ROOT / "scripts/native/macos-delivery.test-tools.json").read_bytes() + b" ")
        with self.assertRaises(api.DeliveryError):
            tools.read_pins(source, api)

    def test_official_pin_role_transport_and_archive_name_cannot_drift(self):
        pins = tools.read_pins(ROOT / "scripts/native/macos-delivery.test-tools.json", api)
        for key, value in [("role", "product-dependency"), ("url", "https://files.pythonhosted.org:443/a.whl"),
                           ("archive", "../escape.whl"), ("version", "0.0"), ("sha256", "0" * 63), ("bytes", 0)]:
            changed = copy.deepcopy(pins)
            changed["sources"][0][key] = value
            with self.assertRaises(api.DeliveryError):
                tools.select_wheels(changed, "x86_64", 12, "CPython", api)

    def test_native_regular_cp312_identity_is_mandatory_before_download(self):
        valid = {"implementation": "CPython", "major": 3, "minor": 12, "architecture": "arm64", "freeThreaded": False}
        tools.validate_interpreter(valid, "arm64", api)
        for key, value in [("implementation", "PyPy"), ("major", 4), ("minor", 13),
                           ("architecture", "x86_64"), ("freeThreaded", True), ("freeThreaded", None)]:
            changed = dict(valid, **{key: value})
            with self.assertRaises(api.DeliveryError):
                tools.validate_interpreter(changed, "arm64", api)
        target = self.root / "environment"
        with mock.patch.object(api, "apple_environment", return_value=({"PATH": "synthetic"}, {})), \
                mock.patch.object(tools.shutil, "which", return_value=sys.executable), \
                mock.patch.object(api.Recorder, "run", return_value=json.dumps(dict(valid, freeThreaded=True))), \
                mock.patch.object(api, "https_download") as download:
            with self.assertRaises(api.DeliveryError):
                tools.install("arm64", target, self.root / "downloads", self.root / "evidence.json", {}, api)
            download.assert_not_called()
        self.assertFalse(target.exists())

    def test_build_fixture_interpreter_is_explicit_and_environment_is_clean(self):
        options = SimpleNamespace(test_python=Path(sys.executable))
        environment = {"PYTHONHOME": "bad", "PYTHONPATH": "bad", "PYTHONUSERBASE": "bad", "PATH": "keep"}
        self.assertEqual(api.fixture_python(options, environment), Path(sys.executable).absolute())
        self.assertEqual(environment, {"PATH": "keep", "PYTHONNOUSERSITE": "1", "PYTHONDONTWRITEBYTECODE": "1"})
        for supplied in (None, self.root / "missing-python"):
            with self.assertRaises(api.DeliveryError):
                api.fixture_python(SimpleNamespace(test_python=supplied), {})

    def test_actual_qt_configs_bind_every_consumer_to_one_prefix(self):
        prefix = self.root / "qt-prefix"
        relatives = ["lib/cmake/Qt6/Qt6Config.cmake", "lib/cmake/Qt6Core/Qt6CoreConfig.cmake"]
        for relative in relatives:
            path = prefix / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# Synthetic config, never an actual Qt installation\n", encoding="ascii")
        options = api.qt_cmake_options(prefix, ("Core",))
        self.assertEqual(options, [f"-DQt6_DIR={prefix / 'lib/cmake/Qt6'}", f"-DQt6Core_DIR={prefix / 'lib/cmake/Qt6Core'}"])
        for modules in (("Gui",), ("../Gui",)):
            with self.assertRaises(api.DeliveryError):
                api.qt_cmake_options(prefix, modules)
        with self.assertRaises(api.DeliveryError):
            api.qt_cmake_options(self.root / "absent-prefix", ("Core",))

    def test_environment_evidence_rejects_identity_interpreter_and_package_drift(self):
        root = self.root / "environment"
        python = root / "bin/python"
        python.parent.mkdir(parents=True)
        python.write_bytes(b"synthetic interpreter, never executed")
        site = root / "lib/python3.12/site-packages"
        site.mkdir(parents=True)
        package = site / "package.py"
        package.write_bytes(b"synthetic package")
        manifest = self.root / "prefix-manifest.json"
        api.write_json(manifest, {"schema": 1, "architecture": "arm64", "inputPinsSha256": tools.INPUT_SHA256,
                                 "files": [{"path": "package.py", "bytes": package.stat().st_size, "sha256": api.digest(package)}]})
        evidence = self.root / "environment-evidence.json"
        value = {"complete": True, "kind": "isolated-independent-fixture-python", "architecture": "arm64",
                 "inputPinsSha256": tools.INPUT_SHA256, "productRuntimeDependency": False,
                 "sourceInterpreter": {"implementation": "CPython", "major": 3, "minor": 12, "architecture": "arm64", "freeThreaded": False},
                 "environmentRoot": str(root), "python": str(python), "pythonSha256": api.digest(python),
                 "sitePackages": str(site), "prefixManifest": str(manifest), "prefixManifestSha256": api.digest(manifest)}
        api.write_json(evidence, value)
        self.assertEqual(tools.verify_environment(evidence, "arm64", api), python)
        with self.assertRaises(api.DeliveryError):
            tools.verify_environment(evidence, "x86_64", api)
        for key, altered in [("complete", False), ("inputPinsSha256", "0" * 64), ("pythonSha256", "0" * 64),
                             ("sitePackages", str(self.site)), ("productRuntimeDependency", True)]:
            api.write_json(evidence, dict(value, **{key: altered}))
            with self.assertRaises(api.DeliveryError):
                tools.verify_environment(evidence, "arm64", api)
        api.write_json(evidence, value)
        (site / "unlisted.pth").write_bytes(b"synthetic startup hook")
        with self.assertRaises(api.DeliveryError):
            tools.verify_environment(evidence, "arm64", api)

    def test_actual_wheel_metadata_and_bytes_are_checked(self):
        archive, entry = self.wheel([("pycparser/__init__.py", b"synthetic fixture package")])
        observed = tools.unpack_wheel(archive, entry, self.site, api)
        self.assertEqual(observed["version"], "2.23")
        self.assertEqual(observed["requiresDist"], [])
        self.assertTrue((self.site / "pycparser/__init__.py").is_file())
        with self.assertRaises(api.DeliveryError):
            tools.unpack_wheel(archive, entry, self.site, api)
        with self.assertRaises(api.DeliveryError):
            tools.unpack_wheel(archive, dict(entry, sha256="0" * 64), self.site, api)

    def test_wrong_or_missing_metadata_fails_before_extract(self):
        for version, metadata in [("0.0", True), ("2.23", False)]:
            archive, entry = self.wheel([("pycparser/__init__.py", b"synthetic")], version, metadata)
            with self.assertRaises(api.DeliveryError):
                tools.unpack_wheel(archive, entry, self.site, api)
            self.assertFalse((self.site / "pycparser/__init__.py").exists())

    def test_wheel_escape_duplicate_link_and_relocation_rejected(self):
        linked = zipfile.ZipInfo("pycparser/link")
        linked.create_system = 3
        linked.external_attr = (stat.S_IFLNK | 0o777) << 16
        cases = [[("../escape.py", b"bad")], [("pkg/A.py", b"one"), ("pkg/a.py", b"two")],
                 [(linked, b"../outside")], [("pycparser-2.23.data/purelib/a.py", b"bad")],
                 [("unsafe.pth", b"import bad")], [("sitecustomize.py", b"bad")]]
        for entries in cases:
            archive, entry = self.wheel(entries)
            with self.assertRaises(api.DeliveryError):
                tools.unpack_wheel(archive, entry, self.site, api)
            self.assertEqual(list(self.site.iterdir()), [])

    def test_unreviewed_or_malformed_dependency_metadata_rejected(self):
        for dependency in ["typing-extensions", "bcrypt", "other-package", "malformed ???"]:
            archive = self.root / "synthetic.whl"
            with zipfile.ZipFile(archive, "w") as wheel:
                wheel.writestr("pycparser/__init__.py", b"synthetic")
                wheel.writestr("pycparser-2.23.dist-info/METADATA", "Metadata-Version: 2.1\nName: pycparser\nVersion: 2.23\nRequires-Dist: " + dependency + "\n")
            entry = {"name": "pycparser", "version": "2.23", "bytes": archive.stat().st_size, "sha256": api.digest(archive)}
            with self.assertRaisesRegex(api.DeliveryError, "reviewed CPython 3.12 closure"):
                tools.unpack_wheel(archive, entry, self.site, api)
            self.assertEqual(list(self.site.iterdir()), [])

    def test_approved_dependency_markers_are_preserved_without_extra_tools(self):
        requirements = ['cffi>=2.0.0; platform_python_implementation != "PyPy"',
                        'typing-extensions>=4.13.2; python_full_version < "3.11"',
                        'bcrypt>=3.1.5; extra == "ssh"']
        archive = self.root / "synthetic.whl"
        with zipfile.ZipFile(archive, "w") as wheel:
            wheel.writestr("cryptography/__init__.py", b"synthetic")
            wheel.writestr("cryptography-48.0.1.dist-info/METADATA", "Metadata-Version: 2.1\nName: cryptography\nVersion: 48.0.1\n" +
                           "".join("Requires-Dist: " + item + "\n" for item in requirements))
        entry = {"name": "cryptography", "version": "48.0.1", "bytes": archive.stat().st_size, "sha256": api.digest(archive)}
        observed = tools.unpack_wheel(archive, entry, self.site, api)
        self.assertEqual(observed["requiresDist"], requirements)
        self.assertEqual(set(tools.VERSIONS), {"cryptography", "cffi", "pycparser"})

    def test_single_quoted_markers_are_canonicalized_but_requirements_stay_exact(self):
        requirements = ["cffi>=2.0.0 ; platform_python_implementation != 'PyPy'",
                        "typing-extensions>=4.13.2 ; python_full_version < '3.11'",
                        "bcrypt>=3.1.5 ; extra == 'ssh'"]
        self.assertEqual(tools.canonical_requirement(requirements[0]),
                         tools.canonical_requirement('cffi>=2.0.0 ; platform_python_implementation != "PyPy"'))
        archive, entry = self.reviewed_wheel("cryptography", "48.0.1", requirements)
        observed = tools.unpack_wheel(archive, entry, self.site, api)
        self.assertEqual(observed["requiresDist"], requirements)
        reordered = self.root / "reordered-site"
        reordered.mkdir()
        archive, entry = self.reviewed_wheel("cryptography", "48.0.1", list(reversed(requirements)), "reordered.whl")
        self.assertEqual(tools.unpack_wheel(archive, entry, reordered, api)["requiresDist"], list(reversed(requirements)))
        cffi_site = self.root / "cffi-site"
        cffi_site.mkdir()
        cffi_archive, cffi_entry = self.reviewed_wheel("cffi", "2.1.1",
                                                       ['pycparser; implementation_name != "PyPy"'], "cffi.whl")
        self.assertEqual(tools.unpack_wheel(cffi_archive, cffi_entry, cffi_site, api)["requiresDist"],
                         ['pycparser; implementation_name != "PyPy"'])

    def test_quote_canonicalization_never_accepts_a_changed_closure(self):
        base = ["cffi>=2.0.0 ; platform_python_implementation != 'PyPy'",
                "typing-extensions>=4.13.2 ; python_full_version < '3.11'",
                "bcrypt>=3.1.5 ; extra == 'ssh'"]
        cases = {
            "extra-requirement": base + ["requests>=2.32 ; python_full_version >= '3.12'"],
            "wrong-version": [base[0].replace("2.0.0", "1.0.0"), base[1], base[2]],
            "changed-marker": [base[0], base[1].replace("< '3.11'", "< '3.12'"), base[2]],
            "dropped-requirement": [base[0], base[1]],
            "changed-extra": [base[0], base[1], base[2].replace("'ssh'", "'sshclient'")],
            "added-marker-disjunct": [base[0], base[1], base[2].replace("extra == 'ssh'", "extra == 'ssh' or extra == 'ssh2'")],
        }
        expected = [item.replace("'", '"') for item in base]
        for label, requirements in cases.items():
            with self.subTest(case=label):
                archive, entry = self.reviewed_wheel("cryptography", "48.0.1", requirements, "rejected-" + label + ".whl")
                with self.assertRaisesRegex(api.DeliveryError, "reviewed CPython 3.12 closure"):
                    tools.unpack_wheel(archive, entry, self.site, api)
                self.assertEqual(list(self.site.iterdir()), [])
                self.assertNotEqual(tools.reviewed_closure(requirements), tools.reviewed_closure(expected))


    @unittest.skipIf(os.name == "nt", "Unprivileged Windows fixture cannot create symlinks")
    def test_linked_site_parent_and_venv_interpreter_are_distinct(self):
        archive, entry = self.wheel([("pycparser/__init__.py", b"synthetic")])
        actual = self.root / "actual-site"
        actual.mkdir()
        (self.site / "pycparser").symlink_to(actual, target_is_directory=True)
        with self.assertRaises(api.DeliveryError):
            tools.unpack_wheel(archive, entry, self.site, api)
        self.assertEqual(list(actual.iterdir()), [])
        python = self.root / "venv-python"
        python.symlink_to(sys.executable)
        self.assertEqual(api.fixture_python(SimpleNamespace(test_python=python), {}), python)
        self.assertNotEqual(python, python.resolve())

    def test_non_apple_install_does_not_download_or_create_environment(self):
        target = self.root / "environment"
        with mock.patch.object(api, "apple_environment", side_effect=api.DeliveryError("Real Darwin required")), mock.patch.object(api, "https_download") as download:
            with self.assertRaises(api.DeliveryError):
                tools.install("arm64", target, self.root / "downloads", self.root / "evidence.json", {}, api)
            download.assert_not_called()
        self.assertFalse(target.exists())

    def tool_wheel(self, members, declared, name="cmake-wheel", tool="cmake"):
        archive = self.root / (name + ".whl")
        with zipfile.ZipFile(archive, "w") as output:
            for member, content in members:
                output.writestr(member, content)
        entry = {"name": name, "version": "0", "archive": archive.name, "url": "https://example.invalid/" + archive.name,
                 "sha256": api.digest(archive), "bytes": archive.stat().st_size, "binaryDirectory": declared}
        return archive, entry

    def test_pinned_tool_wheel_binary_directories_match_real_archive_layouts(self):
        inputs = api.validate_inputs(ROOT / "scripts/native/macos-delivery.inputs.json")
        declared = {entry["name"]: entry["binaryDirectory"] for entry in inputs["tools"]["sources"]}
        # Actual wheel members: cmake/data/bin/cmake and ninja-1.13.0.data/scripts/ninja.
        self.assertEqual(declared, {"cmake-wheel": "cmake/data/bin", "ninja-wheel": "ninja-1.13.0.data/scripts"})

    def test_data_bin_and_data_scripts_layouts_install_without_native_execution(self):
        for name, tool, declared, member in [
                ("cmake-wheel", "cmake", "cmake/data/bin", "cmake/data/bin/cmake"),
                ("ninja-wheel", "ninja", "ninja-1.13.0.data/scripts", "ninja-1.13.0.data/scripts/ninja")]:
            with self.subTest(layout=declared):
                archive, entry = self.tool_wheel([(member, b"synthetic tool bytes, never executed"),
                                                  (tool + "/__init__.py", b"synthetic package")], declared, name, tool)
                root = self.root / (name + "-root")
                root.mkdir()
                record = api.install_tool_wheel(root, archive, entry)
                binary = root / member
                self.assertEqual(record, {"name": name, "binary": str(binary.resolve()),
                                          "binarySha256": api.digest(binary), "wheelSha256": entry["sha256"]})
                self.assertTrue(binary.is_file())

    def test_non_executable_basename_lookalikes_do_not_create_ambiguity(self):
        # The real cmake wheel also carries .../bash-completion/completions/cmake.
        archive, entry = self.tool_wheel(
            [("cmake/data/bin/cmake", b"real tool candidate"),
             ("cmake/data/share/bash-completion/completions/cmake", b"completion script")], "cmake/data/bin")
        root = self.root / "lookalike-root"
        root.mkdir()
        record = api.install_tool_wheel(root, archive, entry)
        self.assertEqual(Path(record["binary"]), (root / "cmake/data/bin/cmake").resolve())

    def test_wrong_declared_directory_fails_before_extraction(self):
        for declared, member in [("synthetic-tool/data/bin", "synthetic-tool-1.13.0.data/scripts/synthetic-tool"),
                                 ("synthetic-tool-1.13.0.data/scripts", "synthetic-tool/data/bin/synthetic-tool")]:
            with self.subTest(declared=declared):
                archive, entry = self.tool_wheel([(member, b"synthetic")], declared, "synthetic-tool-wheel", "synthetic-tool")
                root = self.root / ("wrong-" + declared.replace("/", "-"))
                root.mkdir()
                with self.assertRaisesRegex(api.DeliveryError, "contradicts its declared directory"):
                    api.install_tool_wheel(root, archive, entry)
                self.assertEqual(list(root.iterdir()), [])

    def test_missing_and_duplicate_tool_binaries_fail_closed(self):
        missing, missing_entry = self.tool_wheel([("cmake/__init__.py", b"synthetic")], "cmake/data/bin")
        missing_root = self.root / "missing-root"
        missing_root.mkdir()
        with self.assertRaisesRegex(api.DeliveryError, "binary path missing"):
            api.install_tool_wheel(missing_root, missing, missing_entry)
        self.assertEqual(list(missing_root.iterdir()), [])
        duplicate, duplicate_entry = self.tool_wheel(
            [("cmake/data/bin/cmake", b"first"), ("cmake-3.31.6.data/scripts/cmake", b"second")], "cmake/data/bin")
        duplicate_root = self.root / "duplicate-root"
        duplicate_root.mkdir()
        with self.assertRaisesRegex(api.DeliveryError, "duplicated"):
            api.install_tool_wheel(duplicate_root, duplicate, duplicate_entry)
        self.assertEqual(list(duplicate_root.iterdir()), [])

    def test_hash_byte_symlink_and_unsafe_declarations_remain_rejected(self):
        archive, entry = self.tool_wheel([("cmake/data/bin/cmake", b"synthetic")], "cmake/data/bin")
        rejections = [dict(entry, sha256="0" * 64), dict(entry, bytes=entry["bytes"] + 1), dict(entry, bytes=True),
                      dict(entry, binaryDirectory="../escape"), dict(entry, binaryDirectory=""),
                      dict(entry, binaryDirectory=None), dict(entry, name="cmake")]
        for changed in rejections:
            with self.subTest(changed=changed):
                with self.assertRaises(api.DeliveryError):
                    api.install_tool_wheel(self.root / "reject-root", archive, changed)
        linked = zipfile.ZipInfo("cmake/data/bin/cmake")
        linked.create_system = 3
        linked.external_attr = (stat.S_IFLNK | 0o777) << 16
        link_archive = self.root / "cmake-wheel-link.whl"
        with zipfile.ZipFile(link_archive, "w") as output:
            output.writestr(linked, "../../outside")
        linked_entry = dict(entry, archive=link_archive.name, sha256=api.digest(link_archive),
                            bytes=link_archive.stat().st_size)
        with self.assertRaisesRegex(api.DeliveryError, "Linked tool wheel"):
            api.install_tool_wheel(self.root / "linked-root", link_archive, linked_entry)

    def test_setup_tools_installs_declared_layouts_and_stays_apple_only(self):
        with mock.patch.object(api.sys, "platform", "win32"):
            with self.assertRaisesRegex(api.DeliveryError, "Apple-host-only"):
                api.setup_tools(SimpleNamespace(tools_root=self.root / "tools", downloads=self.root / "downloads"),
                                {"tools": {"sources": []}}, {"tools": []})
        downloads = self.root / "downloads"
        downloads.mkdir()
        cmake, cmake_entry = self.tool_wheel([("cmake/data/bin/cmake", b"synthetic cmake")], "cmake/data/bin")
        ninja, ninja_entry = self.tool_wheel([("ninja-1.13.0.data/scripts/ninja", b"synthetic ninja")],
                                             "ninja-1.13.0.data/scripts", "ninja-wheel", "ninja")
        for archive in (cmake, ninja):
            archive.replace(downloads / archive.name)
        inputs = {"tools": {"sources": [cmake_entry, ninja_entry]}}
        with mock.patch.object(api.sys, "platform", "darwin"):
            installed = api.setup_tools(SimpleNamespace(tools_root=self.root / "tools", downloads=downloads), inputs,
                                        {"tools": [dict(cmake_entry), dict(ninja_entry)]})
        self.assertFalse(installed["appleRuntimeExecuted"])
        self.assertFalse(installed["productDelivered"])
        self.assertEqual(installed["pathDirectories"], [str(self.root / "tools/cmake/data/bin"),
                                                        str(self.root / "tools/ninja-1.13.0.data/scripts")])
        self.assertEqual([Path(record["binary"]).relative_to(self.root / "tools").as_posix()
                          for record in installed["records"]],
                         ["cmake/data/bin/cmake", "ninja-1.13.0.data/scripts/ninja"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
