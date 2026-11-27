"""Test candidate CI binding with inert fixtures, never a launcher acceptance run."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import ci_evidence as ci


def ctest_inventory():
    return {"kind": "ctestInfo", "version": {"major": 1, "minor": 0}, "tests": [
        {"name": "fixture.one", "command": [sys.executable, "-c", "pass"], "properties": []},
        {"name": "fixture.two", "command": [sys.executable, "-c", "pass"], "properties": []},
    ]}


class CiEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pd-ci-contract-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.source.mkdir()
        self.build.mkdir()
        self.reply = self.build / ".cmake/api/v1/reply"
        self.reply.mkdir(parents=True)
        self.prefix = self.root / "candidate"
        self.prefix.mkdir()

    def json(self, path, document):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document), encoding="ascii")

    def file_api(self, target_count=1, name="renamed-pd-fixture.exe"):
        target = {"type": "EXECUTABLE", "name": "unchanged-internal-target", "nameOnDisk": name,
                  "sources": [{"path": "native/app/main.cpp"}],
                  "artifacts": [{"path": "native/app/" + name}],
                  "install": {"destinations": [{"path": "."}]}}
        references = []
        for index in range(target_count):
            filename = f"target-{index}.json"
            self.json(self.reply / filename, target)
            references.append({"jsonFile": filename})
        self.json(self.reply / "codemodel.json", {"kind": "codemodel", "version": {"major": 2},
            "paths": {"source": str(self.source), "build": str(self.build)},
            "configurations": [{"name": "Release", "targets": references}]})
        self.json(self.reply / "index-2026.json", {"reply": {ci.CLIENT: {
            "codemodel-v2": {"kind": "codemodel", "jsonFile": "codemodel.json"}}}})
        return target

    def junit(self, cases=None):
        path = self.root / "ctest.xml"
        if cases is None:
            cases = '<testcase name="fixture.two" status="run"/><testcase name="fixture.one" status="run"/>'
        path.write_text("<testsuite>" + cases + "</testsuite>", encoding="ascii")
        return path

    def test_application_uses_real_file_api_name_not_internal_target(self):
        self.file_api()
        application = ci.cmake_application(self.build, self.source)
        self.assertEqual(application["name"], "unchanged-internal-target")
        self.assertEqual(application["nameOnDisk"], "renamed-pd-fixture.exe")
        self.assertEqual(Path(application["artifact"]), self.build / "native/app/renamed-pd-fixture.exe")

    def test_missing_or_ambiguous_application_is_rejected(self):
        for count in (0, 2):
            with self.subTest(count=count):
                self.file_api(count)
                with self.assertRaises(ci.EvidenceError):
                    ci.cmake_application(self.build, self.source)

    def test_wrong_source_or_reply_escape_is_rejected(self):
        self.file_api()
        with self.assertRaises(ci.EvidenceError):
            ci.cmake_application(self.build, self.root)
        with self.assertRaises(ci.EvidenceError):
            ci.api_child(self.reply, "../../target.json")

    def test_installed_application_must_match_built_bytes(self):
        self.file_api()
        built = self.build / "native/app/renamed-pd-fixture.exe"
        built.parent.mkdir(parents=True)
        built.write_bytes(b"inert fixture, not PE")
        installed = self.prefix / built.name
        shutil.copyfile(built, installed)
        (self.build / "install_manifest.txt").write_text(str(installed) + "\n")
        binary, evidence = ci.installed_application(self.build, self.source, self.prefix)
        self.assertEqual(binary, installed)
        self.assertEqual(evidence["builtBinary"]["sha256"], evidence["installedBinary"]["sha256"])
        installed.write_bytes(b"other generation")
        with self.assertRaises(ci.EvidenceError):
            ci.installed_application(self.build, self.source, self.prefix)

    def test_install_manifest_cannot_escape_candidate_prefix(self):
        self.file_api()
        outside = self.root / "renamed-pd-fixture.exe"
        outside.write_bytes(b"inert")
        (self.build / "install_manifest.txt").write_text(str(outside))
        with self.assertRaises(ci.EvidenceError):
            ci.installed_application(self.build, self.source, self.prefix)

    def test_complete_junit_matches_inventory_in_any_order(self):
        self.assertEqual(ci.verify_ctest(ctest_inventory(), self.junit()), ["fixture.one", "fixture.two"])

    def test_missing_extra_duplicate_failed_or_skipped_test_is_rejected(self):
        cases = [
            '<testcase name="fixture.one"/>',
            '<testcase name="fixture.one"/><testcase name="fixture.one"/>',
            '<testcase name="fixture.one"/><testcase name="fixture.two"/><testcase name="extra"/>',
            '<testcase name="fixture.one"><failure/></testcase><testcase name="fixture.two"/>',
            '<testcase name="fixture.one"><skipped/></testcase><testcase name="fixture.two"/>',
            '<testcase name="fixture.one" status="notrun"/><testcase name="fixture.two"/>',
        ]
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ci.EvidenceError):
                ci.verify_ctest(ctest_inventory(), self.junit(value))

    def test_empty_unresolved_or_disabled_inventory_is_rejected(self):
        unresolved = ctest_inventory()
        unresolved["tests"][0].pop("command")
        disabled = ctest_inventory()
        disabled["tests"][0]["properties"] = [{"name": "DISABLED", "value": True}]
        for document in ({"kind": "ctestInfo", "version": {"major": 1}, "tests": []}, unresolved, disabled):
            with self.subTest(document=document), self.assertRaises(ci.EvidenceError):
                ci.test_names(document)

    def test_preparation_refuses_an_existing_build(self):
        output = self.root / "prepare.json"
        with self.assertRaises(ci.EvidenceError):
            ci.prepare(self.build, output)
        fresh = self.root / "fresh"
        ci.prepare(fresh, output)
        self.assertTrue((fresh / ".cmake/api/v1/query" / ci.CLIENT / "codemodel-v2").is_file())
        self.assertFalse(json.loads(output.read_text())["releaseAccepted"])

    def test_raw_command_log_drift_is_rejected(self):
        log = self.root / "command.log"
        log.write_bytes(b"actual inert output\n")
        evidence = self.root / "command.json"
        self.json(evidence, {"argv": ["ctest.exe", "--no-tests=error"], "complete": True, "returnCode": 0,
                             "log": str(log), "logSha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        ci.checked_command(evidence, "ctest", ("--no-tests=error",))
        log.write_bytes(b"changed")
        with self.assertRaises(ci.EvidenceError):
            ci.checked_command(evidence, "ctest")

    def test_ctest_binding_requires_exact_tree_serial_unfiltered_execution(self):
        inventory = self.root / "inventory.json"
        binding = self.root / "binding.json"
        evidence = self.root / "command.json"
        junit = self.junit()
        self.json(inventory, ctest_inventory())
        self.json(binding, {"kind": "native-ci-configured-ctest-inventory", "sourceRef": "fixture-ref",
                           "build": str(self.build), "inventory": ci.file_ref(inventory),
                           "testNames": ["fixture.one", "fixture.two"]})
        log = self.root / "command.log"
        log.write_bytes(b"inert execution evidence fixture\n")
        argv = ["ctest.exe", "--test-dir", str(self.build), "-C", "Release", "--parallel", "1",
                "--no-tests=error", "--output-junit", str(junit)]
        record = {"complete": True, "returnCode": 0, "cwd": str(self.root), "argv": argv,
                  "log": str(log), "logSha256": hashlib.sha256(log.read_bytes()).hexdigest()}
        options = argparse.Namespace(build=self.build, inventory=inventory, binding=binding,
                                     evidence=evidence, junit=junit, output=self.root / "verified.json")
        with patch.dict("os.environ", {"PD_SOURCE_REF": "fixture-ref"}):
            for extra in (["-R", "fixture"], ["-Rfixture"], ["-LEother"], ["--show-only=json-v1"],
                          ["--exclude-regex=other"], ["-C", "Debug"], ["--parallel", "4"]):
                wrong = copy.deepcopy(record)
                wrong["argv"] += extra
                self.json(evidence, wrong)
                with self.subTest(extra=extra), self.assertRaises(ci.EvidenceError):
                    ci.ctest_command(options)
            self.json(evidence, record)
            ci.ctest_command(options)
            wrapped = {**record, "argv": ["/usr/bin/xvfb-run", "-a", *argv]}
            self.json(evidence, wrapped)
            options.output = self.root / "verified-xvfb.json"
            ci.ctest_command(options)
        self.assertEqual(json.loads(options.output.read_text())["testCount"], 2)
        self.assertEqual(ci.read_json(evidence)["argv"][0], "/usr/bin/xvfb-run")

    def test_unknown_or_ambiguous_command_wrapper_is_rejected(self):
        for argv in (["sh", "-c", "ctest --no-tests=error"],
                     ["xvfb-run", "-a", "sh", "-c", "ctest"],
                     ["xvfb-run", "--auto-servernum", "ctest", "--no-tests=error"],
                     ["ctest", 1]):
            with self.subTest(argv=argv), self.assertRaises(ci.EvidenceError):
                ci.command_argv({"argv": argv}, "ctest")

    def test_inventory_uses_the_actual_declared_vs_toolchain(self):
        options = argparse.Namespace(build=self.build, output=self.root / "inventory.json",
                                     binding=self.root / "binding.json", ctest="ctest")
        result = subprocess.CompletedProcess([], 0, json.dumps(ctest_inventory()).encode("ascii"), b"")
        with patch.dict("os.environ", {"NICKEL_VS_ROOT": str(self.root / "declared-vs")}), \
                patch("msvc.developer_environment", return_value={"PATH": "fixture-tools"}) as environment, \
                patch("shutil.which", return_value=str(self.root / "ctest.exe")), \
                patch("subprocess.run", return_value=result):
            ci.inventory_command(options)
            environment.assert_called_once_with(self.root / "declared-vs")
        self.assertEqual(ci.read_json(options.binding)["argv"][0], str(self.root / "ctest.exe"))

    def test_actual_cmake_file_api_and_ctest_fixture(self):
        cmake, ctest = shutil.which("cmake"), shutil.which("ctest")
        if cmake is None or ctest is None:
            self.skipTest("CMake and CTest are required for the actual API fixture")
        source = self.root / "actual-source"
        (source / "native/app").mkdir(parents=True)
        (source / "native/app/main.cpp").write_text("int main() { return 0; }\n", encoding="ascii")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(PdInertCiFixture LANGUAGES CXX)\n'
            'add_executable(internal-fixture native/app/main.cpp)\n'
            'set_target_properties(internal-fixture PROPERTIES OUTPUT_NAME actual-fixture)\n'
            'install(TARGETS internal-fixture RUNTIME DESTINATION .)\nenable_testing()\n'
            f'add_test(NAME fixture.one COMMAND "{Path(sys.executable).as_posix()}" -c "pass")\n'
            f'add_test(NAME fixture.two COMMAND "{Path(sys.executable).as_posix()}" -c "pass")\n', encoding="ascii")
        build = self.root / "actual-build"
        ci.prepare(build, self.root / "actual-preparation.json")
        commands = [
            [cmake, "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
            [cmake, "--build", str(build), "--parallel", "1"],
            [cmake, "--install", str(build), "--prefix", str(self.prefix)],
        ]
        for argv in commands:
            result = subprocess.run(argv, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        binary, application = ci.installed_application(build, source, self.prefix)
        self.assertTrue(binary.is_file())
        self.assertEqual(application["name"], "internal-fixture")
        result = subprocess.run([ctest, "--test-dir", str(build), "-C", "Release", "--show-only=json-v1"],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        inventory = json.loads(result.stdout)
        junit = self.root / "actual.xml"
        result = subprocess.run([ctest, "--test-dir", str(build), "-C", "Release", "--parallel", "1",
                                 "--no-tests=error", "--output-junit", str(junit)],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(ci.verify_ctest(inventory, junit)), 2)


class HookFreeGraphTests(unittest.TestCase):
    """Read-only contracts: all graph/receipt contents are supplied in memory."""

    def setUp(self):
        self.source = ci.ROOT
        self.build = self.source / ".snow/read-only-production-graph"
        self.cache = {"CMAKE_BUILD_TYPE": "Release", "NICKEL_BUILD_UI": "ON",
                      "NICKEL_BUILD_TESTS": "OFF", "NICKEL_TEST_TRANSPORT_FAULTS": "OFF",
                      "NICKEL_PRODUCTION_SMOKE_SAME_GRAPH": "OFF",
                      "CMAKE_HOME_DIRECTORY": str(self.source), "CMAKE_CACHEFILE_DIR": str(self.build),
                      "NICKEL_SHARED_NATIVE_ROOT": ""}
        self.commands = [{"directory": str(self.build), "file": str(self.source / "native/app" / name),
                          "command": 'cl /c "' + str(self.source / "native/app" / name) + '"'}
                         for name in ("main.cpp", "application_state.cpp", "service/trusted_launcher.cpp",
                                      "service/launcher_export_services.cpp")]

    def graph(self, cache=None, commands=None, drift=False):
        cache = self.cache if cache is None else cache
        commands = self.commands if commands is None else commands
        text = "\n".join(name + ":STRING=" + value for name, value in cache.items()) + "\n"
        refs = {self.build / "CMakeCache.txt": text.encode("utf-8"),
                self.build / "compile_commands.json": json.dumps(commands).encode("ascii")}
        reads = {}

        def reference(path):
            data = refs[path]
            reads[path] = reads.get(path, 0) + 1
            return {"path": str(path), "bytes": len(data),
                    "sha256": hashlib.sha256(data + (b"drift" if drift and reads[path] > 1 else b"")).hexdigest()}

        with patch.object(ci, "file_ref", side_effect=reference), \
                patch.object(Path, "read_text", return_value=text), \
                patch.object(ci, "read_json", return_value=commands), \
                patch.object(ci, "cmake_application", return_value={"artifact": str(self.build / "actual-name")}):
            return ci.hook_free_graph(self.build, self.source)

    def test_hook_free_graph_binds_actual_factory_sources(self):
        result = self.graph()
        self.assertEqual(result["compiledFileCount"], 4)
        self.assertEqual(result["build"], str(self.build))
        self.assertFalse(result["testsEnabled"])
        self.assertFalse(result["transportFaultsEnabled"])
        arguments = [{"directory": row["directory"], "file": row["file"], "arguments": ["cl", "/c", row["file"]]}
                     for row in self.commands]
        self.assertEqual(self.graph(commands=arguments)["compiledFileCount"], 4)

    def test_test_graph_or_another_source_build_cannot_be_deployed(self):
        for name, value in (("CMAKE_BUILD_TYPE", "Debug"), ("NICKEL_BUILD_UI", "OFF"),
                            ("NICKEL_BUILD_TESTS", "ON"), ("NICKEL_TEST_TRANSPORT_FAULTS", "ON"),
                            ("NICKEL_PRODUCTION_SMOKE_SAME_GRAPH", "ON"),
                            ("NICKEL_SHARED_NATIVE_ROOT", str(self.source / "native")),
                            ("CMAKE_HOME_DIRECTORY", str(self.build)), ("CMAKE_CACHEFILE_DIR", str(self.source))):
            with self.subTest(name=name), self.assertRaises(ci.EvidenceError):
                self.graph(cache={**self.cache, name: value})
        for name in self.cache:
            missing = {key: value for key, value in self.cache.items() if key != name}
            if name != "NICKEL_SHARED_NATIVE_ROOT":
                with self.subTest(missing=name), self.assertRaises(ci.EvidenceError):
                    self.graph(cache=missing)

    def test_hooks_missing_factory_and_malformed_commands_fail_closed(self):
        for name in ("NICKEL_TRANSPORT_TEST_HOOKS", "NICKEL_EXPORT_TEST_FAULTS"):
            for spelling in (" /D", " /D ", " -D", " -D "):
                commands = copy.deepcopy(self.commands)
                commands[0]["command"] += spelling + name + "=0"
                with self.subTest(hook=name, spelling=spelling), self.assertRaises(ci.EvidenceError):
                    self.graph(commands=commands)
            arguments = copy.deepcopy(self.commands)
            first = arguments[0]
            arguments[0] = {"directory": first["directory"], "file": first["file"],
                            "arguments": ["cl", "/c", first["file"], "/D" + name + "=0"]}
            with self.subTest(hook=name, form="arguments"), self.assertRaises(ci.EvidenceError):
                self.graph(commands=arguments)
        variants = [[], self.commands[:-1], [{**self.commands[0], "directory": str(self.source)}, *self.commands[1:]],
                    [{**self.commands[0], "arguments": ["cl", "/c"]}, *self.commands[1:]],
                    [{**self.commands[0], "command": ""}, *self.commands[1:]]]
        for commands in variants:
            with self.subTest(commands=commands), self.assertRaises(ci.EvidenceError):
                self.graph(commands=commands)

    def test_graph_drift_is_rejected(self):
        with self.assertRaises(ci.EvidenceError):
            self.graph(drift=True)

    def release(self, configure_extra=(), build_extra=(), binding=None, inventory_drift=False):
        source_ref = "a" * 40
        inventory = {"path": str(self.source / ".snow/source-inventory.json"), "sha256": "b" * 64,
                     "sourceCommit": source_ref, "verified": True}
        common = {"complete": True, "returnCode": 0, "cwd": str(self.source),
                  "buildInputInventory": inventory if binding is None else binding, "buildInputGate": "passed"}
        records = [{**common, "argv": ["cmake", "--preset", "native-release", "-B", str(self.build), *configure_extra]},
                   {**common, "argv": ["cmake", "--build", str(self.build), "--config", "Release",
                                       "--parallel", "1", "--", "-d", "keeprsp", *build_extra]}]
        options = argparse.Namespace(source=self.source, build=self.build, input_inventory=Path(inventory["path"]),
                                     configure_evidence=self.source / ".snow/configure.json",
                                     build_evidence=self.source / ".snow/build.json", output=self.source / ".snow/verified.json")
        inventories = [inventory, {**inventory, "sha256": "c" * 64} if inventory_drift else inventory]
        with patch.dict("os.environ", {"PD_SOURCE_REF": source_ref}), \
                patch("msvc.input_inventory", side_effect=inventories), \
                patch.object(ci, "read_json", return_value={"sourceRoot": str(self.source)}), \
                patch.object(ci, "checked_command", side_effect=records), \
                patch.object(ci, "hook_free_graph", return_value={"application": {"artifact": str(self.build / "actual-name")}}), \
                patch.object(ci, "file_ref", return_value={"sha256": "d" * 64, "bytes": 1}), \
                patch.object(ci, "write_json") as output:
            ci.verify_release_command(options)
            return output.call_args.args[1]

    def test_release_receipts_bind_same_inventory_without_accepting_a_release(self):
        result = self.release()
        self.assertEqual(result["kind"], "native-ci-hook-free-production-build")
        self.assertFalse(result["releaseAccepted"])
        self.assertIn("builtBinary", result["graph"]["application"])

    def test_release_rejects_ambiguous_filtered_or_unbound_commands(self):
        for extra in (("--target", "actual-name"), ("--config", "Debug"), ("--parallel", "4")):
            with self.subTest(extra=extra), self.assertRaises(ci.EvidenceError):
                self.release(build_extra=extra)
        with self.assertRaises(ci.EvidenceError):
            self.release(configure_extra=("--install",))
        with self.assertRaises(ci.EvidenceError):
            self.release(binding={})
        with self.assertRaises(ci.EvidenceError):
            self.release(inventory_drift=True)



if __name__ == "__main__":
    unittest.main()
