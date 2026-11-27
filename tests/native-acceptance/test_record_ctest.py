"""Negative tests for the acceptance recorder; these are not product tests."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import record_ctest


class RecorderTests(unittest.TestCase):
    def scratch(self):
        return tempfile.TemporaryDirectory(prefix=".recorder-tools-", dir=Path(__file__).parent)

    def test_stable_snapshot_and_mutation(self):
        with self.scratch() as directory:
            root = Path(directory)
            (root / "thing.cpp").write_text("int value = 1;\n", encoding="ascii")
            (root / "CMakeLists.txt").write_text("project(Test)\n", encoding="ascii")
            (root / ".build-cache").mkdir()
            (root / ".build-cache" / "ignore.cpp").write_text("generated", encoding="ascii")
            first = record_ctest.source_manifest(root)
            self.assertEqual(first, record_ctest.source_manifest(root))
            self.assertEqual(set(first["files"]), {"thing.cpp", "CMakeLists.txt"})
            (root / "thing.cpp").write_text("int value = 2;\n", encoding="ascii")
            self.assertNotEqual(first["sha256"], record_ctest.source_manifest(root)["sha256"])

    def test_fixture_metadata_and_generator_inputs_are_fingerprinted(self):
        with self.scratch() as directory:
            root = Path(directory)
            (root / "thing.cpp").write_text("int value = 1;\n", encoding="ascii")
            for name in ["fixture.json", "metadata.json", "heads.cjs", "fixture.mjs", "BuildInfo.hpp.in"]:
                with self.subTest(name=name):
                    path = root / name
                    path.write_text("original", encoding="ascii")
                    before = record_ctest.source_manifest(root)
                    self.assertIn(name, before["files"])
                    path.write_text("changed", encoding="ascii")
                    self.assertNotEqual(before["sha256"], record_ctest.source_manifest(root)["sha256"])

    def test_no_sources_never_passes(self):
        with self.scratch() as directory:
            with self.assertRaises(ValueError):
                record_ctest.source_manifest(Path(directory))
            (Path(directory) / "fixture.json").write_text("{}", encoding="ascii")
            with self.assertRaises(ValueError):
                record_ctest.source_manifest(Path(directory))

    def test_inventory_requires_executable_and_enabled_tests(self):
        valid = {"tests": [{"name": "actual", "command": ["actual.exe"], "properties": []}]}
        self.assertEqual(record_ctest.inventory_names(json.dumps(valid).encode()), ["actual"])
        for tests in [[], [{"name": "actual"}], [{"name": "actual", "command": ["actual.exe"],
                                                      "properties": [{"name": "DISABLED", "value": True}]}]]:
            with self.subTest(tests=tests), self.assertRaises(ValueError):
                record_ctest.inventory_names(json.dumps({"tests": tests}).encode())

    def test_junit_requires_exact_executed_set(self):
        with self.scratch() as directory:
            path = Path(directory) / "ctest.xml"
            path.write_text('<testsuite><testcase name="actual" status="run" time="1"/></testsuite>', encoding="ascii")
            self.assertFalse(record_ctest.junit_evidence(path, ["actual"])["failures"])
            self.assertTrue(record_ctest.junit_evidence(path, ["different"])["failures"])
            path.write_text('<testsuite/>', encoding="ascii")
            self.assertTrue(record_ctest.junit_evidence(path, ["actual"])["failures"])

    def test_skipped_failure_error_never_passes(self):
        with self.scratch() as directory:
            path = Path(directory) / "ctest.xml"
            for element in ["skipped", "failure", "error"]:
                with self.subTest(element=element):
                    path.write_text(f'<testsuite><testcase name="actual"><{element}/></testcase></testsuite>', encoding="ascii")
                    observed = record_ctest.junit_evidence(path, ["actual"])
                    self.assertTrue(observed["failures"])
                    self.assertEqual(observed["scenarios"][0]["status"], "failed-or-unexecuted")

    def test_unexecuted_status_is_never_labelled_passed(self):
        with self.scratch() as directory:
            path = Path(directory) / "ctest.xml"
            path.write_text('<testsuite><testcase name="actual" status="notrun"/></testsuite>', encoding="ascii")
            observed = record_ctest.junit_evidence(path, ["actual"])
            self.assertTrue(observed["failures"])
            self.assertEqual(observed["scenarios"][0]["status"], "failed-or-unexecuted")

    def test_command_records_exact_streams_and_exit(self):
        with self.scratch() as directory:
            root = Path(directory)
            code = "import os,sys; os.write(1,b'actual-out\\n'); os.write(2,b'actual-err\\n'); sys.exit(7)"
            record, stdout = record_ctest.record_command([sys.executable, "-c", code], root, root)
            self.assertEqual(record["exitCode"], 7)
            self.assertEqual(stdout, b"actual-out\n")
            self.assertEqual((root / "stderr.log").read_bytes(), b"actual-err\n")
            self.assertEqual(record["stdoutSha256"], hashlib.sha256(stdout).hexdigest())
            self.assertEqual(record["stderrSha256"], hashlib.sha256(b"actual-err\n").hexdigest())
            self.assertEqual(json.loads((root / "command.json").read_text()), record)
            self.assertFalse((root / "pending.json").exists())

    def test_interruption_preserves_streamed_bytes_without_final_exit(self):
        with self.scratch() as directory:
            root = Path(directory)

            def interrupt(command, **kwargs):
                self.assertNotIn("capture_output", kwargs)
                kwargs["stdout"].write(b"unfinished-output\n")
                kwargs["stderr"].write(b"unfinished-error\n")
                kwargs["stdout"].flush()
                kwargs["stderr"].flush()
                self.assertEqual((root / "stdout.log").read_bytes(), b"unfinished-output\n")
                self.assertEqual((root / "stderr.log").read_bytes(), b"unfinished-error\n")
                raise subprocess.TimeoutExpired(command, 1)

            with patch.object(record_ctest.subprocess, "run", side_effect=interrupt):
                with self.assertRaises(subprocess.TimeoutExpired):
                    record_ctest.record_command([sys.executable, "-c", "pass"], root, root)
            self.assertFalse((root / "command.json").exists())
            pending = json.loads((root / "pending.json").read_text())
            self.assertEqual(pending["status"], "running-or-interrupted")
            self.assertNotIn("exitCode", pending)
            self.assertEqual((root / "stdout.log").read_bytes(), b"unfinished-output\n")
            self.assertEqual((root / "stderr.log").read_bytes(), b"unfinished-error\n")

    def test_existing_command_evidence_is_never_overwritten(self):
        with self.scratch() as directory:
            root = Path(directory)
            command = [sys.executable, "-c", "print('first-run')"]
            record_ctest.record_command(command, root, root)
            before = {path.name: path.read_bytes() for path in root.iterdir()}
            with self.assertRaises(FileExistsError):
                record_ctest.record_command(command, root, root)
            self.assertEqual(before, {path.name: path.read_bytes() for path in root.iterdir()})


    def suite_fixture(self, root):
        workspace, source, build = root / "workspace", root / "source", root / "build"
        harness = workspace / "tests" / "native-acceptance"
        harness.mkdir(parents=True)
        (harness / "CMakeLists.txt").write_text("project(Acceptance)\n", encoding="ascii")
        wrapper = workspace / "scripts" / "native" / "msvc.py"
        wrapper.parent.mkdir(parents=True)
        wrapper.write_text("# controlled wrapper fixture\n", encoding="ascii")
        source.mkdir()
        (source / "actual.cpp").write_text("int actual = 1;\n", encoding="ascii")
        build.mkdir()
        (build / "CMakeCache.txt").write_text("NICKEL_SOURCE_ROOT:PATH=" + str(source) + "\n", encoding="utf-8")
        (build / "actual.exe").write_bytes(b"controlled executable fixture; never run")
        return workspace, source, build

    def controlled_ctest(self, build):
        rounds = []

        def command(command, cwd, destination):
            record = {"command": command, "exitCode": 0}
            if "--show-only=json-v1" in command:
                return record, json.dumps({"tests": [{"name": "actual", "command": ["actual.exe"]}]}).encode()
            rounds.append(len(rounds) + 1)
            Path(command[command.index("--output-junit") + 1]).write_text(
                '<testsuite><testcase name="actual" status="run" time="1"/></testsuite>', encoding="ascii")
            log = build / "Testing" / "Temporary" / "LastTest.log"
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(f"1/1 Testing: actual\nOutput:\nround {rounds[-1]}\n<end of output>\n", encoding="ascii")
            return record, b"controlled fixture output\n"

        return command

    def test_full_round_golden_json_is_not_truncated(self):
        with self.scratch() as directory:
            root = Path(directory)
            log = root / "build" / "Testing" / "Temporary" / "LastTest.log"
            log.parent.mkdir(parents=True)
            golden = {"schemaVersion": 1, "kind": "real-foundation-golden", "passed": 86, "failed": 0,
                      "scenarios": [{"name": f"controlled/{index}", "status": "passed"} for index in range(86)]}
            payload = json.dumps(golden, indent=2).encode()
            raw = b"1/1 Testing: native_foundation_golden\nOutput:\n" + payload + b"\n<end of output>\n"
            log.write_bytes(raw)
            evidence = root / "round"
            evidence.mkdir()
            result = record_ctest.preserve_round_output(root / "build", evidence, ["native_foundation_golden"])
            self.assertGreater(len(payload), 1024)
            self.assertEqual((evidence / "golden-result.json").read_bytes(), payload)
            self.assertEqual((evidence / "ctest-full.log").read_bytes(), raw)
            self.assertEqual(result["golden"]["passed"], 86)
            self.assertEqual(result["golden"]["sha256"], hashlib.sha256(payload).hexdigest())
            with self.assertRaises(FileExistsError):
                record_ctest.preserve_round_output(root / "build", evidence, ["native_foundation_golden"])
            self.assertEqual((evidence / "ctest-full.log").read_bytes(), raw)

    def test_missing_truncated_or_false_golden_evidence_never_passes(self):
        valid = {"schemaVersion": 1, "kind": "real-foundation-golden", "passed": 1, "failed": 0,
                 "scenarios": [{"name": "controlled", "status": "passed"}]}
        payloads = [b'{"passed":', json.dumps({**valid, "passed": 2}).encode(),
                    json.dumps({**valid, "passed": True}).encode(),
                    json.dumps({**valid, "failed": 1}).encode(),
                    json.dumps({**valid, "scenarios": [{"name": "controlled", "status": "skipped"}]}).encode(),
                    json.dumps({**valid, "passed": 2, "scenarios": valid["scenarios"] * 2}).encode()]
        with self.scratch() as directory:
            root = Path(directory)
            log = root / "build" / "Testing" / "Temporary" / "LastTest.log"
            log.parent.mkdir(parents=True)
            for index, payload in enumerate(payloads):
                with self.subTest(index=index):
                    log.write_bytes(b"1/1 Testing: native_foundation_golden\nOutput:\n" + payload + b"\n<end of output>\n")
                    evidence = root / f"round-{index}"
                    evidence.mkdir()
                    with self.assertRaises(ValueError):
                        record_ctest.preserve_round_output(root / "build", evidence, ["native_foundation_golden"])
                    self.assertFalse((evidence / "golden-result.json").exists())
            evidence = root / "missing"
            evidence.mkdir()
            log.unlink()
            with self.assertRaises(ValueError):
                record_ctest.preserve_round_output(root / "build", evidence, ["actual"])

    def test_full_log_inventory_must_match(self):
        with self.scratch() as directory:
            root = Path(directory)
            log = root / "build" / "Testing" / "Temporary" / "LastTest.log"
            log.parent.mkdir(parents=True)
            log.write_bytes(b"1/1 Testing: another\nOutput:\nactual output\n<end of output>\n")
            evidence = root / "round"
            evidence.mkdir()
            with self.assertRaises(ValueError):
                record_ctest.preserve_round_output(root / "build", evidence, ["actual"])
            self.assertEqual((evidence / "ctest-full.log").read_bytes(), log.read_bytes())

    def test_execution_manifest_fingerprints_harness_tools_and_artifacts(self):
        with self.scratch() as directory:
            root = Path(directory)
            workspace, _, build = self.suite_fixture(root)
            original = record_ctest.execution_manifest(workspace, build)
            self.assertIn("python", original["tools"])
            self.assertIn("wrapper", original["tools"])
            self.assertIn("actual.exe", original["buildFiles"])
            for path in [workspace / "scripts" / "native" / "msvc.py", build / "actual.exe",
                         workspace / "tests" / "native-acceptance" / "CMakeLists.txt"]:
                before = record_ctest.execution_manifest(workspace, build)
                path.write_bytes(path.read_bytes() + b"\nchanged\n")
                self.assertNotEqual(before, record_ctest.execution_manifest(workspace, build))

    def test_wrong_source_cache_rejected_before_any_evidence_write(self):
        with self.scratch() as directory:
            root = Path(directory)
            workspace, _, build = self.suite_fixture(root)
            wrong = root / "wrong-source"
            wrong.mkdir()
            (wrong / "actual.cpp").write_text("int actual = 2;\n", encoding="ascii")
            output = root / "evidence"
            with patch.object(record_ctest, "record_command") as command:
                with self.assertRaises(ValueError):
                    record_ctest.run_suite(workspace, wrong, build, output, 1, 1)
                command.assert_not_called()
            self.assertFalse(output.exists())

    def test_every_round_is_preserved_before_ctest_overwrites_log(self):
        with self.scratch() as directory:
            root = Path(directory)
            workspace, source, build = self.suite_fixture(root)
            output = root / "evidence"
            with patch.object(record_ctest, "record_command", side_effect=self.controlled_ctest(build)):
                result = record_ctest.run_suite(workspace, source, build, output, 1, 2)
            self.assertEqual(result["gate"], "passed-executed-scenarios")
            self.assertTrue(result["sourceUnchanged"])
            self.assertTrue(result["executionUnchanged"])
            self.assertEqual(len(result["rounds"]), 2)
            self.assertIn("round 1", (output / "round-01" / "ctest-full.log").read_text())
            self.assertIn("round 2", (output / "round-02" / "ctest-full.log").read_text())
            self.assertNotEqual(result["rounds"][0]["fullLog"]["sha256"], result["rounds"][1]["fullLog"]["sha256"])

    def test_changed_execution_inputs_fail_even_when_ctest_exits_zero(self):
        with self.scratch() as directory:
            root = Path(directory)
            workspace, source, build = self.suite_fixture(root)
            controlled = self.controlled_ctest(build)

            def mutate(command, cwd, destination):
                result = controlled(command, cwd, destination)
                if "--show-only=json-v1" not in command:
                    (build / "actual.exe").write_bytes(b"changed executable fixture")
                return result

            with patch.object(record_ctest, "record_command", side_effect=mutate):
                result = record_ctest.run_suite(workspace, source, build, root / "evidence", 1, 1)
            self.assertEqual(result["gate"], "failed")
            self.assertFalse(result["executionUnchanged"])
            self.assertIn("Harness, tools or build artifacts changed during acceptance", result["failures"])



if __name__ == "__main__":
    unittest.main(verbosity=2)
