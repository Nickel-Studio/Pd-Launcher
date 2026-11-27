#!/usr/bin/env python3
"""Tests of audit tools only; synthetic inputs cannot accept a product release."""
from __future__ import annotations

import ast
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import audit_support as support
import build_checkpoint
import build_provenance
import run_audit
import source_audit
import verify_snapshot
import windows_deployment_audit


class OwnedTest(unittest.TestCase):
    def setUp(self):
        support.capacity(support.OWNED, 1024 * 1024)
        base = support.OWNED / "scratch"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="tools-", dir=base)
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)


class AtomicOutputTests(OwnedTest):
    def test_low_space_never_truncates_existing_evidence(self):
        path = self.root / "result.json"
        path.write_bytes(b"preserve-existing-evidence")
        with mock.patch.object(support.shutil, "disk_usage", return_value=SimpleNamespace(free=0)):
            with self.assertRaises(support.AuditSafetyError):
                support.atomic_json(path, {"passed": True})
        self.assertEqual(path.read_bytes(), b"preserve-existing-evidence")
        self.assertEqual(list(self.root.glob(".audit-write-*")), [])

    def test_replace_failure_preserves_evidence_and_own_temp_is_removed(self):
        path = self.root / "result.json"
        path.write_bytes(b"old")
        with mock.patch.object(support.os, "replace", side_effect=OSError("simulated replace failure")):
            with self.assertRaises(OSError):
                support.atomic_json(path, {"new": True})
        self.assertEqual(path.read_bytes(), b"old")
        self.assertEqual(list(self.root.glob(".audit-write-*")), [])

    def test_atomic_output_is_json(self):
        path = self.root / "result.json"
        support.atomic_json(path, {"uint64": 18446744073709551615, "passed": False})
        self.assertEqual(json.loads(path.read_text())["uint64"], 18446744073709551615)
        self.assertFalse(json.loads(path.read_text())["passed"])

    def test_outside_owner_and_traversal_are_rejected(self):
        for path in [support.OWNED.parent / "outside-result.json", self.root / ".." / "escape.json"]:
            with self.subTest(path=str(path)), self.assertRaises(support.AuditSafetyError):
                support.owned_output(path)

    def test_worker_lock_refuses_parallel_and_is_removed(self):
        with mock.patch.object(support, "OWNED", self.root):
            with support.single_worker():
                with self.assertRaises(support.AuditSafetyError):
                    with support.single_worker():
                        self.fail("second worker must never run")
            self.assertFalse((self.root / "scratch/.audit-worker.lock").exists())


class SnapshotTests(OwnedTest):
    def snapshot(self):
        mirror = self.root / "mirror"
        source = mirror / "native/core/config/sample.cpp"
        source.parent.mkdir(parents=True)
        source.write_text("int owned() { return 1; }\n", encoding="ascii")
        records = {"native/core/config/sample.cpp": {"owner": "foundation", "sha256": support.sha256(source)}}
        payload = {"schemaVersion": 1, "kind": "ignored-owner-dependency-snapshot", "createdUnix": 123,
                   "sourceSetSha256": "f" * 64, "snapshotSha256": verify_snapshot.records_hash(records),
                   "files": records}
        manifest = mirror / "snapshot.json"
        manifest.write_text(json.dumps(payload), encoding="ascii")
        return source, manifest, payload

    def test_exact_synthetic_record_set_verifies_only_sources(self):
        _, manifest, payload = self.snapshot()
        report = verify_snapshot.verify(manifest, support.sha256(manifest))
        self.assertTrue(report["verified"])
        self.assertFalse(report["completeProductVerified"])
        self.assertEqual(report["fileCount"], 1)
        self.assertFalse(report["precisionRefresh"])
        payload.update(sourceSetSha256=None, refreshSetSha256=payload["snapshotSha256"],
                       previousManifestSha256="e" * 64, refreshedScopes=["native/core/config"],
                       refreshedOwners=["foundation"],
                       frozenSource={"manifest": str(manifest), "manifestSha256": "d" * 64})
        manifest.write_text(json.dumps(payload), encoding="ascii")
        with self.assertRaises(support.AuditSafetyError):
            verify_snapshot.verify(manifest)
        report = verify_snapshot.verify(manifest, support.sha256(manifest))
        self.assertTrue(report["verified"])
        self.assertTrue(report["precisionRefresh"])
        self.assertIsNone(report["sourceSetSha256"])
        self.assertFalse(report["completeProductVerified"])
        payload["refreshSetSha256"] = "0" * 64
        manifest.write_text(json.dumps(payload), encoding="ascii")
        report = verify_snapshot.verify(manifest, support.sha256(manifest))
        self.assertFalse(report["verified"])
        self.assertIn("refresh-set-hash-mismatch", {row["kind"] for row in report["errors"]})
        for value in (False, 1, [], {}):
            payload["sourceSetSha256"] = value
            manifest.write_text(json.dumps(payload), encoding="ascii")
            with self.subTest(sourceSetSha256=value), self.assertRaises(support.AuditSafetyError):
                verify_snapshot.verify(manifest, support.sha256(manifest))

    def test_changed_source_and_unlisted_member_fail_closed(self):
        source, manifest, _ = self.snapshot()
        source.write_text("changed", encoding="ascii")
        extra = source.parent / "unlisted.hpp"
        extra.write_text("#pragma once", encoding="ascii")
        report = verify_snapshot.verify(manifest)
        self.assertFalse(report["verified"])
        self.assertEqual({r["kind"] for r in report["errors"]}, {"file-hash-mismatch", "unlisted-native-file"})

    def test_wrong_frozen_manifest_is_rejected(self):
        _, manifest, _ = self.snapshot()
        with self.assertRaises(support.AuditSafetyError):
            verify_snapshot.verify(manifest, "0" * 64)

    def test_traversal_keys_are_rejected(self):
        for key in ["../escape", "/absolute", "C:/drive", "native\\file", "native//file", "native/./file"]:
            with self.subTest(key=key), self.assertRaises(support.AuditSafetyError):
                verify_snapshot.relative_key(key)

    def test_record_set_hash_mismatch_is_not_success(self):
        _, manifest, payload = self.snapshot()
        payload["snapshotSha256"] = "0" * 64
        manifest.write_text(json.dumps(payload))
        report = verify_snapshot.verify(manifest)
        self.assertFalse(report["verified"])
        self.assertIn("record-set-hash-mismatch", {r["kind"] for r in report["errors"]})


class BuildProvenanceTests(OwnedTest):
    def build(self, extra: str = ""):
        native = self.root / "mirror" / "native"
        native.mkdir(parents=True)
        (native / "sample.cpp").write_text("int sample() { return 1; }\n", encoding="ascii")
        build = self.root / "run" / "build"
        build.mkdir(parents=True)
        (build.parent / ".nickel-portability-owned.json").write_text(
            json.dumps({"owner": str(support.OWNED)}), encoding="ascii")
        (build / "CMakeCache.txt").write_text(
            "NICKEL_NATIVE_ROOT:PATH=" + str(native) + "\n"
            + "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(support.OWNED) + "\n"
            + "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_GENERATOR:INTERNAL=Ninja\n"
            + "NICKEL_PORTABILITY_ALL_CORE:BOOL=ON\n" + extra, encoding="utf-8")
        source = run_audit.inventory(native)["contentSha256"]
        return build, native, source

    def record(self, build, native, source):
        return build_provenance.record(build, native, source, "a" * 64, str(native), str(support.OWNED))

    def validate(self, build, native, source, manifest="a" * 64):
        return build_provenance.validate(build, native, source, manifest, str(native), str(support.OWNED))

    def test_exact_inputs_verify_identity_not_product(self):
        build, native, source = self.build()
        path = self.record(build, native, source)
        report = self.validate(build, native, source)
        self.assertTrue(report["verified"])
        self.assertFalse(json.loads(path.read_bytes())["completeProductVerified"])
        self.assertEqual(report["sha256"], support.sha256(path))

    def test_missing_record_and_wrong_source_or_manifest_are_rejected(self):
        build, native, source = self.build()
        with self.assertRaises(support.AuditSafetyError):
            self.validate(build, native, source)
        self.record(build, native, source)
        for current, manifest in [("b" * 64, "a" * 64), (source, "b" * 64)]:
            with self.subTest(source=current, manifest=manifest), self.assertRaises(support.AuditSafetyError):
                self.validate(build, native, current, manifest)

    def test_changed_cache_or_probe_does_not_reuse_objects(self):
        build, native, source = self.build()
        self.record(build, native, source)
        with mock.patch.object(build_provenance, "audit_inputs", return_value={"changed": "b" * 64}):
            with self.assertRaises(support.AuditSafetyError):
                self.validate(build, native, source)
        cache = build / "CMakeCache.txt"
        cache.write_text(cache.read_text() + "# identity changed\n", encoding="utf-8")
        with self.assertRaises(support.AuditSafetyError):
            self.validate(build, native, source)

    def test_owner_and_non_object_marker_fail_closed(self):
        build, native, source = self.build()
        marker = build.parent / ".nickel-portability-owned.json"
        for payload in [{"owner": str(support.OWNED.parent)}, {}, [], {"owner": None}, {"owner": "relative-owner"}]:
            marker.write_text(json.dumps(payload), encoding="ascii")
            with self.subTest(payload=payload), self.assertRaises(support.AuditSafetyError):
                self.record(build, native, source)
        self.assertFalse((build / build_provenance.PROVENANCE_NAME).exists())

    def test_historical_workaround_and_duplicate_cache_identity_are_rejected(self):
        build, native, source = self.build()
        cache = build / "CMakeCache.txt"
        normal = cache.read_text(encoding="utf-8")
        for extra in ["NICKEL_INSTANCES_TOML_INCLUDE_DIR:PATH=/usr/include/toml++\n",
                      "NICKEL_INSTANCES_TOML_INCLUDE_DIR:PATH=/usr/include/toml++/\n",
                      "NICKEL_NATIVE_ROOT:STRING=other-source\n"]:
            cache.write_text(normal + extra, encoding="utf-8")
            with self.subTest(extra=extra), self.assertRaises(support.AuditSafetyError):
                self.record(build, native, source)
        self.assertFalse((build / build_provenance.PROVENANCE_NAME).exists())

    def test_wrong_generator_configuration_and_source_home_are_rejected(self):
        build, native, source = self.build()
        cache = build / "CMakeCache.txt"
        normal = cache.read_text(encoding="utf-8")
        for current, changed in [("=Release", "=Debug"), ("=Ninja", "=Unix Makefiles"),
                                 ("CMAKE_HOME_DIRECTORY:INTERNAL=" + str(support.OWNED),
                                  "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(self.root))]:
            cache.write_text(normal.replace(current, changed), encoding="utf-8")
            with self.subTest(changed=changed), self.assertRaises(support.AuditSafetyError):
                self.record(build, native, source)

    def test_existing_record_is_not_overwritten(self):
        build, native, source = self.build()
        path = self.record(build, native, source)
        before = path.read_bytes()
        with self.assertRaises(support.AuditSafetyError):
            self.record(build, native, source)
        self.assertEqual(path.read_bytes(), before)

    def test_outside_owner_build_is_rejected(self):
        with self.assertRaises(support.AuditSafetyError):
            build_provenance.build_identity(support.OWNED.parent / "foreign-build", self.root,
                                            str(self.root), str(support.OWNED))

    def test_checkpoint_low_capacity_never_spawns_wsl_or_creates_evidence(self):
        output = self.root / "must-not-exist.json"
        argv = ["build_checkpoint.py", "--build-dir", str(self.root / "absent-build"),
                "--native-root", str(self.root), "--snapshot-manifest", str(self.root / "snapshot.json"),
                "--expected-manifest-sha256", "a" * 64, "--output", str(output)]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(
                build_checkpoint, "build_capacity", side_effect=support.AuditSafetyError("capacity")), mock.patch.object(
                build_checkpoint, "wsl_path") as guest_path, mock.patch.object(build_checkpoint, "execute") as spawn:
            with mock.patch.object(sys, "stderr", __import__("io").StringIO()):
                self.assertEqual(build_checkpoint.main(), 2)
            guest_path.assert_not_called()
            spawn.assert_not_called()
        self.assertFalse(output.exists())


class WindowsDeploymentTests(OwnedTest):
    @classmethod
    def setUpClass(cls):
        root = support.OWNED.parents[1] / ".snow" / "native-shared"
        cls.parser = root / "tests/native-acceptance/audit_windows.py"
        fixture = root / "tests/native-acceptance/test_tools.py"
        manifest = json.loads((root / "snapshot.json").read_bytes())
        records = manifest["files"]
        cls.parser_sha = records["tests/native-acceptance/audit_windows.py"]["sha256"]
        if support.sha256(cls.parser) != cls.parser_sha or support.sha256(fixture) != records[
                "tests/native-acceptance/test_tools.py"]["sha256"]:
            raise support.AuditSafetyError("Frozen acceptance parser or fixture hash mismatch")
        tree = ast.parse(fixture.read_text(encoding="utf-8"), filename=str(fixture))
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "sample_pe")
        namespace = {"struct": __import__("struct")}
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(fixture), "exec"), namespace)
        cls.sample_pe = staticmethod(namespace["sample_pe"])

    def binary(self, normal=None, delay=None):
        path = self.root / "synthetic-pe-never-run.exe"
        path.write_bytes(self.sample_pe(normal or ["kernel32.dll"], delay))
        return path

    def audit(self, path, modules=None, expected_modules=None):
        return windows_deployment_audit.audit(path, support.sha256(path), self.parser, self.parser_sha,
                                               modules, expected_modules, r"C:\Windows" if modules else None)

    def test_import_only_does_not_accept_static_crt_or_complete_deployment(self):
        report = self.audit(self.binary())
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["importGate"], "passed")
        self.assertEqual(report["runtimeGate"], "unverified")
        self.assertEqual(report["staticCrtGate"], "unverified")
        self.assertEqual(report["deploymentGate"], "incomplete")
        self.assertFalse(report["passed"])
        self.assertFalse(report["completeProductVerified"])

    def test_delay_import_and_dynamic_crt_are_not_hidden(self):
        for dependency in ["Qt6Core.dll", "vcruntime140.dll", "WebView2Loader.dll"]:
            path = self.binary(delay=[dependency])
            with self.subTest(dependency=dependency):
                report = self.audit(path)
                self.assertEqual(report["importGate"], "failed")
                self.assertEqual(report["deploymentGate"], "failed")
                self.assertTrue(report["failures"])
                self.assertFalse(report["completeProductVerified"])

    def test_wrong_binary_or_parser_hash_is_rejected_before_parser_load(self):
        path = self.binary()
        with mock.patch.object(windows_deployment_audit, "load_parser") as loader:
            with self.assertRaises(support.AuditSafetyError):
                windows_deployment_audit.audit(path, "0" * 64, self.parser, self.parser_sha)
            loader.assert_not_called()
        with self.assertRaises(support.AuditSafetyError):
            windows_deployment_audit.audit(path, support.sha256(path), self.parser, "0" * 64)

    def test_missing_modules_and_hash_pair_are_rejected(self):
        path = self.binary()
        for modules, digest in [(self.root / "absent-modules.json", None), (None, "a" * 64)]:
            with self.subTest(modules=modules), self.assertRaises(support.AuditSafetyError):
                windows_deployment_audit.audit(path, support.sha256(path), self.parser, self.parser_sha,
                                                 modules, digest, r"C:\Windows")

    def test_malformed_modules_fail_closed(self):
        path = self.binary()
        module_path = self.root / "modules.json"
        invalid = [[], {"snapshots": {}}, {"snapshots": [None]},
                   {"snapshots": [{"scenario": "synthetic", "pid": True, "modules": []}]},
                   {"snapshots": [{"scenario": "synthetic", "pid": 98765, "modules": [None]}]},
                   {"snapshots": [{"scenario": "synthetic", "pid": 98765, "modules": [], "children": [None]}]}]
        for value in invalid:
            module_path.write_text(json.dumps(value), encoding="ascii")
            with self.subTest(value=value), self.assertRaises(support.AuditSafetyError):
                self.audit(path, module_path, support.sha256(module_path))

    def test_matching_modules_only_pass_named_parser_scenarios(self):
        path = self.binary()
        module_path = self.root / "modules.json"
        value = {"schemaVersion": 1, "executableSha256": support.sha256(path), "snapshots": [{
            "pid": 98765, "scenario": "synthetic-parser-scenario-not-product-run",
            "modules": [str(path), r"C:\Windows\System32\kernel32.dll"]}]}
        module_path.write_text(json.dumps(value), encoding="ascii")
        report = self.audit(path, module_path, support.sha256(module_path))
        self.assertEqual(report["runtimeGate"], "passed-observed-scenarios")
        self.assertEqual(report["systemModuleIdentityGate"], "unverified")
        self.assertEqual(report["noExtractionGate"], "unverified")
        self.assertEqual(report["deploymentGate"], "incomplete")
        self.assertFalse(report["completeProductVerified"])
        value["executableSha256"] = "0" * 64
        module_path.write_text(json.dumps(value), encoding="ascii")
        self.assertEqual(self.audit(path, module_path, support.sha256(module_path))["runtimeGate"], "failed")

    def test_unexplained_modules_and_bad_system_root_are_rejected(self):
        path = self.binary()
        module_path = self.root / "modules.json"
        value = {"schemaVersion": 1, "executableSha256": support.sha256(path), "snapshots": [{
            "pid": 98765, "scenario": "synthetic-parser-scenario-not-product-run",
            "modules": [str(path), str(self.root / "Qt6Core.dll")]}]}
        module_path.write_text(json.dumps(value), encoding="ascii")
        self.assertEqual(self.audit(path, module_path, support.sha256(module_path))["runtimeGate"], "failed")
        for root in ["", "Windows", r"C:\Windows\..\Temp"]:
            with self.subTest(root=root), self.assertRaises(support.AuditSafetyError):
                windows_deployment_audit.audit(path, support.sha256(path), self.parser, self.parser_sha,
                                                 module_path, support.sha256(module_path), root)

    def test_existing_report_is_preserved_without_reading_or_launching_binary(self):
        output = self.root / "preserved.json"
        output.write_bytes(b"existing-evidence")
        argv = ["windows_deployment_audit.py", "--executable", str(self.root / "absent.exe"),
                "--expected-executable-sha256", "a" * 64, "--parser", str(self.parser),
                "--expected-parser-sha256", self.parser_sha, "--output", str(output)]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(windows_deployment_audit, "audit") as audit:
            with mock.patch.object(sys, "stderr", __import__("io").StringIO()):
                self.assertEqual(windows_deployment_audit.main(), 2)
            audit.assert_not_called()
        self.assertEqual(output.read_bytes(), b"existing-evidence")

    def test_successful_pe_read_still_returns_nonpassing_cli_status(self):
        path = self.binary()
        output = self.root / "partial-audit.json"
        argv = ["windows_deployment_audit.py", "--executable", str(path),
                "--expected-executable-sha256", support.sha256(path), "--parser", str(self.parser),
                "--expected-parser-sha256", self.parser_sha, "--output", str(output)]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(sys, "stdout", __import__("io").StringIO()):
            self.assertEqual(windows_deployment_audit.main(), 1)
        self.assertFalse(json.loads(output.read_bytes())["passed"])
        self.assertFalse(json.loads(output.read_bytes())["completeProductVerified"])


class SourceBoundaryTests(unittest.TestCase):
    def test_comment_does_not_create_forbidden_dependency(self):
        report = source_audit.analyze("ui/safe.cpp", "// no WebView2 or QJSEngine\nint owned = 1;\n")
        self.assertEqual(report["findings"], [])

    def test_real_forbidden_components_are_detected(self):
        for text in ["#include <QJSEngine>", "target_link_libraries(app Qt6::WebEngineWidgets)",
                     "QQuickWindow view;", "v8::Isolate* engine;"]:
            with self.subTest(text=text):
                report = source_audit.analyze("ui/unsafe.cpp", text)
                self.assertTrue(any(r["kind"] == "forbidden-runtime-or-build" for r in report["findings"]))

    def test_std_core_qt_candidate_is_visible_for_target_review(self):
        report = source_audit.analyze("core/documents/qt_body.hpp", "#include <QTextDocument>\n")
        self.assertTrue(report["findings"])

    def test_win32_public_header_and_unguarded_include_are_visible(self):
        report = source_audit.analyze("platform/api.hpp", "#include <windows.h>\nHANDLE leaked();\n")
        self.assertTrue(report["publicHeaderCandidates"])
        self.assertTrue(any(r["kind"] == "unguarded-win32-include" for r in report["findings"]))


class RunnerTests(OwnedTest):
    def test_actual_qt_gate_matches_product_network_off(self):
        source = run_audit.qt_gate_source()
        self.assertIn("Qt6 6.8.3 EXACT", source)
        self.assertIn("Widgets Svg Test", source)
        self.assertNotIn("Network", source)

    def test_local_real_command_records_stdout_stderr_and_failure(self):
        with mock.patch.object(run_audit, "OWNED", self.root):
            result = run_audit.execute([sys.executable, "-c", "import sys; print('owned'); print('error',file=sys.stderr); sys.exit(7)"],
                                       self.root, timeout=10)
        self.assertEqual(result["exitCode"], 7)
        self.assertFalse(result["passed"])
        self.assertIn("owned", result["stdout"])
        self.assertIn("error", result["stderr"])
        self.assertEqual(result["stdoutSha256"], hashlib.sha256((self.root / result["stdoutLog"]).read_bytes()).hexdigest())

    def test_missing_command_saves_nonpassing_status(self):
        with mock.patch.object(run_audit, "OWNED", self.root):
            result = run_audit.execute([str(self.root / "nonexistent-command.exe")], self.root, timeout=5)
        self.assertEqual(result["state"], "SUPERVISION_FAILED")
        self.assertFalse(result["passed"])
        self.assertIsNone(result["exitCode"])

    def test_capacity_failure_occurs_before_spawn_or_output(self):
        with mock.patch.object(run_audit, "OWNED", self.root), mock.patch.object(run_audit, "capacity", side_effect=support.AuditSafetyError("disk")):
            with mock.patch.object(run_audit.subprocess, "Popen") as spawn:
                with self.assertRaises(support.AuditSafetyError):
                    run_audit.execute(["must-not-start"], self.root)
                spawn.assert_not_called()
        self.assertFalse((self.root / "evidence").exists())

    def test_all_owned_python_source_parses(self):
        for path in support.OWNED.glob("*.py"):
            with self.subTest(path=path.name):
                ast.parse(path.read_text(encoding="utf-8"), filename=str(path))


if __name__ == "__main__":
    unittest.main(verbosity=2)
