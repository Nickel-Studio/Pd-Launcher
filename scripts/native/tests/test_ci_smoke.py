"""Inert CI smoke producer fixtures, not production or cross-platform acceptance."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ci_smoke as smoke


class CiSmokeTests(unittest.TestCase):
    def test_source_inventory_excludes_generated_and_historical_inputs(self):
        self.assertTrue(smoke.selected_source("native/app/main.cpp"))
        self.assertTrue(smoke.selected_source("tests/native-production-smoke/matrix.json"))
        for name in ("native/tests/minecraft/.build-policy/test.cpp", "native/core/.deps/nlohmann/json.hpp",
                     "native/assets/brand/history/icon.png", "native/app/build.ninja", "docs/status.md"):
            self.assertFalse(smoke.selected_source(name), name)

    @unittest.skipUnless(shutil.which("git"), "Git is required for committed source byte fixtures")
    def test_committed_and_actual_checkout_bytes_are_bound_independently(self):
        with tempfile.TemporaryDirectory(prefix="pd-ci-source-") as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(["git", *args], cwd=root, stderr=subprocess.PIPE)
            git("init", "--quiet")
            git("config", "user.name", "CI Fixture")
            git("config", "user.email", "fixture@example.invalid")
            git("config", "core.autocrlf", "false")
            source = root / "native/app/main.cpp"
            source.parent.mkdir(parents=True)
            (root / ".gitattributes").write_bytes(b"*.cpp text eol=crlf\n")
            source.write_bytes(b"int main() {\n    return 0;\n}\n")
            git("add", "--", ".gitattributes", "native/app/main.cpp")
            git("commit", "--quiet", "-m", "Inert committed source fixture")
            commit = git("rev-parse", "HEAD").decode().strip()
            source.write_bytes(source.read_bytes().replace(b"\n", b"\r\n"))
            git("add", "--", "native/app/main.cpp")
            self.assertEqual(git("diff", "--cached", "--name-only"), b"")
            result = smoke.capture_sources(root, commit)
            self.assertEqual(result["sourceCommit"], commit)
            self.assertEqual(result["checkoutByteDifferences"], ["native/app/main.cpp"])
            actual = next(row for row in result["files"] if row["path"] == "native/app/main.cpp")
            committed = next(row for row in result["committedFiles"] if row["path"] == "native/app/main.cpp")
            self.assertNotEqual(actual["sha256"], committed["sha256"])
            self.assertFalse(result["completeReleaseBuildInputs"])
            with self.assertRaises(smoke.EvidenceError):
                smoke.capture_sources(root, "0" * 40)
            source.write_bytes(b"unexpected actual source\n")
            with self.assertRaises(smoke.EvidenceError):
                smoke.capture_sources(root, commit)

    def source_fixture(self, root):
        names = ("CMakeLists.txt", "matrix.json", "production_main.cpp", "suite_cases.hpp", "qt_probe.hpp", "same_graph_inputs.hpp")
        rows = []
        for name in names:
            file = root / "tests/native-production-smoke" / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(("inert-harness-" + name).encode("ascii"))
            reference = smoke.file_ref(file)
            rows.append({"path": file.relative_to(root).as_posix(), "bytes": reference["bytes"], "sha256": reference["sha256"]})
        return {"sourceCommit": "a" * 40, "files": rows}

    def test_preparation_registers_the_complete_fresh_same_graph_tuple(self):
        with tempfile.TemporaryDirectory(prefix="pd-ci-plan-") as directory:
            root = Path(directory)
            source = self.source_fixture(root)
            metadata = root / "native/metadata.json"
            metadata.parent.mkdir(parents=True)
            metadata.write_text('{"version":"0.1.0"}', encoding="ascii")
            received = {}
            def generate(path, version, deep, source_commit, source_sha256, harness_sha256):
                self.assertEqual(version, "0.1.0")
                self.assertTrue(deep)
                received.update(sourceCommit=source_commit, sourceSha256=source_sha256, harnessSha256=harness_sha256)
                path.mkdir(parents=True)
                (path / "fixture-manifest.json").write_text(json.dumps({
                    "kind": "synthetic-production-fixtures", "owner": "production-smoke",
                    "fixtureRole": "immutable-template", "sourceCommit": source_commit,
                    "sourceGenerationManifestSha256": source_sha256,
                    "harnessManifestSha256": harness_sha256}, sort_keys=True), encoding="ascii")
                return {"root": str(path)}
            generator = SimpleNamespace(generate=generate)
            output = root / ".snow/ci-smoke-plan.json"
            with patch.object(smoke, "capture_sources", return_value=source), patch.object(smoke, "smoke_tools", return_value=(generator, None)):
                plan = smoke.prepare(root, output, "a" * 40)
                self.assertEqual(received["sourceCommit"], "a" * 40)
                self.assertEqual(received["sourceSha256"], plan["sourceManifest"]["sha256"])
                self.assertEqual(received["harnessSha256"], plan["harnessManifest"]["sha256"])
                arguments = plan["configureArguments"]
                self.assertIn("-DNICKEL_PRODUCTION_SMOKE_SAME_GRAPH=ON", arguments)
                self.assertIn("-DNICKEL_PRODUCTION_SMOKE_REGISTER_TEST=ON", arguments)
                self.assertIn("-DNICKEL_PRODUCTION_SMOKE_SOURCE_COMMIT=" + "a" * 40, arguments)
                self.assertIn("-DNICKEL_PRODUCTION_SMOKE_SOURCE_SHA256=" + plan["sourceManifest"]["sha256"], arguments)
                self.assertIn("-DNICKEL_PRODUCTION_SMOKE_HARNESS_SHA256=" + plan["harnessManifest"]["sha256"], arguments)
                self.assertNotEqual(plan["sourceManifest"]["sha256"], plan["harnessManifest"]["sha256"])
                self.assertFalse(Path(plan["verifiedFreeze"]).exists())
                self.assertFalse(Path(plan["caseOutput"]).exists())
                with self.assertRaises(smoke.EvidenceError):
                    smoke.prepare(root, output, "a" * 40)

    def test_dependency_closure_requires_one_real_consumer(self):
        targets = {"smoke": {"name": smoke.TARGET, "dependencies": [{"id": "app"}]},
                   "app": {"name": "nickel_app_services", "dependencies": [{"id": "foundation"}]},
                   "foundation": {"name": "nickel_foundation"}}
        self.assertEqual(smoke.dependency_closure(targets, smoke.TARGET), set(targets))
        with self.assertRaises(smoke.EvidenceError):
            smoke.dependency_closure(targets, "invented_target")

    def test_actual_artifact_discovery_accepts_platform_names_not_windows_suffixes(self):
        with tempfile.TemporaryDirectory(prefix="pd-ci-artifact-") as directory:
            build = Path(directory)
            for name in ("pd-launcher.exe", "pd-launcher", "libnickel_app_services.a", "nickel_app_services.lib"):
                (build / name).write_bytes(b"inert-unit-fixture")
                target = {"nameOnDisk": name, "artifacts": [{"path": name}]}
                self.assertEqual(smoke.target_artifact(build, target), build / name)
            with self.assertRaises(smoke.EvidenceError):
                smoke.target_artifact(build, {"nameOnDisk": "missing", "artifacts": [{"path": "missing"}]})

    def test_target_artifact_discovery_is_limited_to_the_built_graph(self):
        self.assertEqual(smoke.TARGET, "nickel_production_smoke_existing_matrix")

    def test_native_target_platform_must_match_the_actual_host(self):
        for host, expected in (("win32", "windows"), ("linux", "linux")):
            with patch.object(smoke.sys, "platform", host):
                self.assertEqual(smoke.target_platform(), expected)
                self.assertEqual(smoke.target_platform(expected), expected)
                with self.assertRaises(smoke.EvidenceError):
                    smoke.target_platform("linux" if expected == "windows" else "windows")
        with patch.object(smoke.sys, "platform", "darwin"):
            with self.assertRaises(smoke.EvidenceError):
                smoke.target_platform()

    def test_publication_uses_complete_owner_verifier_and_rejects_changed_sources(self):
        platforms = (("win32", "windows", "nickel_app_services.lib", "pd-launcher.exe", smoke.TARGET + ".exe"),
                     ("linux", "linux", "libnickel_app_services.a", "pd-launcher", smoke.TARGET))
        for host, platform, library_name, app_name, smoke_name in platforms:
            with self.subTest(platform=platform), patch.object(smoke.sys, "platform", host):
                with tempfile.TemporaryDirectory(prefix="pd-ci-publish-") as directory:
                    root = Path(directory).resolve()
                    source = self.source_fixture(root)
                    source_path = root / ".snow/source.json"
                    smoke.write_json(source_path, source)
                    harness_path = root / ".snow/harness.json"
                    smoke.write_json(harness_path, {"files": []})
                    fixture_path = root / ".snow/fixture.json"
                    smoke.write_json(fixture_path, {"kind": "synthetic-production-fixtures"})
                    build = root / ".snow/build"
                    build.mkdir()
                    for name in (library_name, app_name, smoke_name):
                        (build / name).write_bytes(("inert-artifact-" + name).encode("ascii"))
                    smoke.write_json(build / "compile_commands.json", [{"file": "inert-unit-fixture-only"}])
                    (build / "build.ninja").write_text("inert-link-fixture-only", encoding="ascii")
                    inventory = root / ".snow/inventory.json"
                    smoke.write_json(inventory, {"kind": "ctestInfo", "tests": []})
                    plan_path = root / ".snow/plan.json"
                    freeze_path = root / ".snow/production-smoke/ci/verified-inputs.json"
                    plan = {"kind": "native-ci-same-graph-smoke-plan", "sourceRoot": str(root), "sourceCommit": "a" * 40,
                            "targetPlatform": platform, "harnessRoot": str(root), "ownerRoot": str(root),
                            "sourceManifest": smoke.file_ref(source_path), "harnessManifest": smoke.file_ref(harness_path),
                            "fixtureManifest": smoke.file_ref(fixture_path), "matrix": str(root / "tests/native-production-smoke/matrix.json"),
                            "verifiedFreeze": str(freeze_path), "caseOutput": str(freeze_path.parent / "cases.json")}
                    smoke.write_json(plan_path, plan)
                    command_path = root / ".snow/build-command.json"
                    smoke.write_json(command_path, {"complete": True, "returnCode": 0})
                    targets = {"app": {"name": "nickel_app_services", "type": "STATIC_LIBRARY", "nameOnDisk": library_name,
                                       "artifacts": [{"path": library_name}]},
                               "smoke": {"name": smoke.TARGET, "type": "EXECUTABLE", "nameOnDisk": smoke_name,
                                         "artifacts": [{"path": smoke_name}], "dependencies": [{"id": "app"}]}}
                    verifier = Mock()
                    def prepare(args):
                        self.assertEqual(args.target_platform, platform)
                        self.assertEqual(args.harness_root, str(root))
                        self.assertEqual(args.ctest_inventory, str(inventory))
                        ledger = smoke.read_json(Path(args.artifact_manifest))
                        complete = smoke.read_json(Path(args.complete_marker))
                        self.assertEqual(ledger["targetPlatform"], platform)
                        self.assertEqual(complete["targetPlatform"], platform)
                        self.assertEqual(args.source_sha256, plan["sourceManifest"]["sha256"])
                        self.assertEqual(args.artifact_sha256, smoke.file_ref(Path(args.artifact_manifest))["sha256"])
                        self.assertEqual(args.compile_sha256, smoke.file_ref(Path(args.compile_commands))["sha256"])
                        self.assertEqual(args.link_sha256, smoke.file_ref(Path(args.link_recipe))["sha256"])
                        result = {"kind": "verified-same-graph-production-inputs", "artifactManifest": args.artifact_manifest,
                                  "completeMarker": args.complete_marker, "copiedLibraryBytes": 0, "productionPass": False,
                                  "artifactReferences": ledger["artifacts"], "ownerFixture": True}
                        smoke.write_json(Path(args.output), result)
                        return result
                    verifier.prepare.side_effect = prepare
                    with patch.object(smoke, "checked_command", return_value={}), \
                         patch.object(smoke.subprocess, "check_output", return_value=b"a" * 40 + b"\n"), \
                         patch.object(smoke, "cmake_application", return_value={"artifact": str(build / app_name)}), \
                         patch.object(smoke, "codemodel_targets", return_value=targets), \
                         patch.object(smoke, "smoke_tools", return_value=(None, verifier)):
                        freeze = smoke.publish(root, build, plan_path, inventory, command_path)
                        self.assertEqual(freeze["kind"], "verified-same-graph-production-inputs")
                        self.assertFalse(freeze["productionPass"])
                        self.assertEqual(freeze["copiedLibraryBytes"], 0)
                        self.assertEqual(freeze, smoke.read_json(freeze_path))
                        verifier.prepare.assert_called_once()
                        verifier.validate_compile_commands.assert_not_called()
                        self.assertEqual([row["role"] for row in freeze["artifactReferences"]],
                                         ["production-library", "production-executable", "smoke-executable"])
                        complete = smoke.read_json(Path(freeze["completeMarker"]))
                        self.assertTrue(complete["publicationComplete"])
                        self.assertFalse(complete["runtimeAccepted"])
                        changed = root / "tests/native-production-smoke/production_main.cpp"
                        changed.write_bytes(b"changed-source-must-be-rejected")
                        with self.assertRaises(smoke.EvidenceError):
                            smoke.publish(root, build, plan_path, inventory, command_path)


if __name__ == "__main__":
    unittest.main()
