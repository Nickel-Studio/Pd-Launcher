"""Small deterministic tool tests. Synthetic bytes are never production acceptance."""
from __future__ import annotations

import ast
import copy
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import stat
import sys
import types
import unittest
from unittest import mock
import zipfile

sys.dont_write_bytecode = True
HERE = Path(__file__).absolute().parent
sys.path.insert(0, str(HERE))
import delivery_smoke as smoke
import windows_runtime as runtime
import prepare_linked_candidate as candidate

OUTPUT = None
COUNTER = 0


def directory(label: str) -> Path:
    global COUNTER
    COUNTER += 1
    result = OUTPUT / (label + "-" + str(COUNTER))
    smoke.capacity(1024 * 1024)
    result.mkdir(exist_ok=False)
    return result


def input_file(folder: Path, name: str, data: bytes) -> Path:
    result = folder / name
    smoke.write_new(result, data)
    return result


def package_fixture() -> dict:
    root = directory("synthetic-package")
    install = root / "install"
    install.mkdir(exist_ok=False)
    executable = input_file(install, "pd-launcher.exe", b"TOOL-TEST-ONLY-NOT-A-PE\n")
    notice = input_file(install, "LICENSE", b"TOOL-TEST-LICENSE-NOT-PRODUCTION\n")
    raw = input_file(root, "fixture-raw.log", b"SYNTHETIC TOOL TEST, never a real cmake install\n")
    files = [dict(row, role="application" if row["distributionPath"] == "pd-launcher.exe" else "license-material")
             for row in smoke.tree_files(install)]
    projections = [{key: row[key] for key in ("distributionPath", "bytes", "sha256")} for row in files]
    provenance = root / "synthetic-install-receipt.json"
    commit = "1" * 40
    smoke.write_json(provenance, {"kind": "actual-cmake-install", "sourceCommit": commit,
                                  "executableSha256": smoke.digest(executable), "files": projections,
                                  "rawEvidence": [smoke.file_ref(raw)], "syntheticToolFixture": True})
    archive = root / "synthetic-program.zip"
    smoke.capacity(1024 * 1024)
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED) as stream:
        stream.write(executable, "pd-launcher.exe")
        stream.write(notice, "LICENSE")
    return {"schemaVersion": 1, "kind": "native-release", "scope": "full-launcher", "product": "Palladium",
            "platform": "windows", "architecture": "x64", "configuration": "Release", "releaseId": "SYNTHETIC-TOOL-TEST",
            "version": "0.0.0", "sourceCommit": commit, "executable": smoke.file_ref(executable),
            "install": {"path": install.relative_to(smoke.ROOT).as_posix(), "files": files, "provenance": smoke.file_ref(provenance)},
            "programPackage": dict(smoke.file_ref(archive), scope="program-runtime-only", compression={"format": "zip"}),
            "deployment": {"windowsSingleExe": True, "selfExtractsRuntime": False, "fileSetComplete": True,
                           "externalRequirements": [{"kind": "system", "name": "Windows", "minimum": "10"}]},
            "syntheticToolFixture": True}


class ToolTests(unittest.TestCase):
    def test_source_syntax_complete(self):
        for name in ("delivery_smoke.py", "windows_runtime.py", "prepare_linked_candidate.py", "test_delivery_smoke.py"):
            source = (HERE / name).read_text(encoding="utf-8")
            ast.parse(source, filename=name)

    def test_windows_unsafe_names(self):
        for name in (None, "", "../x", "/x", "D:/x", "a\\b", "a//b", "a/./b", "a:stream", "nul.exe", "a/COM1", "a./b", "a /b", "a\x00b"):
            with self.subTest(name=name), self.assertRaises(smoke.SmokeError):
                smoke.safe_name(name)
        self.assertEqual(smoke.safe_name("LICENSES/Qt.txt"), "LICENSES/Qt.txt")

    def test_current_capacity_is_real_d(self):
        result = smoke.capacity(1024)
        self.assertEqual(result["actualWriteMount"].casefold(), "d:\\")
        self.assertTrue(result["actualWriteVolume"].startswith("\\\\?\\Volume{"))
        self.assertGreaterEqual(result["drives"]["C"]["availableBytes"], smoke.C_RESERVE)
        self.assertGreaterEqual(result["drives"]["D"]["availableBytes"], smoke.D_RESERVE + 1024)

    def test_reference_requires_independent_hash(self):
        folder = directory("reference")
        path = input_file(folder, "input.bin", b"owned input")
        row = smoke.file_ref(path)
        self.assertEqual(smoke.reference(row), path)
        row["sha256"] = "0" * 64
        with self.assertRaises(smoke.SmokeError):
            smoke.reference(row)

    def test_reference_missing_is_not_run(self):
        folder = directory("missing")
        name = (folder / "absent.bin").relative_to(smoke.ROOT).as_posix()
        with self.assertRaises(smoke.NotRun):
            smoke.reference({"path": name, "sha256": "1" * 64, "bytes": 1})

    def test_owned_outputs_are_not_overwritten(self):
        folder = directory("new-output")
        path = input_file(folder, "preserve.bin", b"keep original")
        with self.assertRaises(FileExistsError):
            smoke.write_new(path, b"replacement")
        self.assertEqual(path.read_bytes(), b"keep original")

    def test_duplicate_json_is_not_a_receipt(self):
        folder = directory("duplicate-json")
        path = input_file(folder, "input.json", b'{"status":"PASS","status":"FAIL"}')
        with self.assertRaises(smoke.SmokeError):
            smoke.read_json(path)

    def test_hardlink_is_not_independent_distribution(self):
        folder = directory("hardlink")
        path = input_file(folder, "first", b"independent bytes")
        smoke.capacity(1024)
        os.link(path, folder / "second")
        try:
            with self.assertRaises(smoke.SmokeError):
                smoke.tree_files(folder)
        finally:
            # This exact link was created by this test; original input and all directories remain.
            (folder / "second").unlink()

    def test_existing_worker_is_not_stolen(self):
        folder = directory("lease")
        worker = smoke.Worker()
        worker.path = folder / "foreign-lease.json"
        input_file(folder, "foreign-lease.json", b"unknown owner token")
        with self.assertRaises(smoke.NotRun):
            worker.__enter__()
        self.assertEqual(worker.path.read_bytes(), b"unknown owner token")

    def test_package_contents_are_observed_not_inferred(self):
        fixture = package_fixture()
        install, archive, records = smoke.package_contract(fixture)
        self.assertEqual(records, smoke.zip_records(archive))
        self.assertEqual(records, smoke.tree_files(install))
        self.assertEqual(len(records), 2)

    def test_install_extra_file_fails(self):
        fixture = package_fixture()
        install = smoke.inside(fixture["install"]["path"])
        input_file(install, "unlisted.txt", b"extra")
        with self.assertRaises(smoke.SmokeError):
            smoke.package_contract(fixture)

    def test_install_missing_file_fails(self):
        fixture = package_fixture()
        fixture["install"]["files"].append({"distributionPath": "missing.txt", "bytes": 1,
                                            "sha256": "0" * 64, "role": "data-resource"})
        with self.assertRaises(smoke.SmokeError):
            smoke.package_contract(fixture)

    def test_no_installed_license_is_not_run(self):
        fixture = package_fixture()
        fixture["install"]["files"] = [row for row in fixture["install"]["files"] if row["role"] == "application"]
        with self.assertRaises(smoke.NotRun):
            smoke.package_contract(fixture)

    def test_copying_qt_prefix_is_rejected(self):
        fixture = package_fixture()
        fixture["install"]["files"].append({"distributionPath": "Qt6Core.dll", "role": "data-resource",
                                            "bytes": 1, "sha256": "1" * 64})
        with self.assertRaises(smoke.SmokeError):
            smoke.package_contract(fixture)

    def test_runtime_zip_must_match_actual_install(self):
        fixture = package_fixture()
        folder = directory("wrong-zip")
        archive = folder / "wrong.zip"
        smoke.capacity(1024)
        with zipfile.ZipFile(archive, "x") as stream:
            stream.writestr("pd-launcher.exe", "different generation")
        fixture["programPackage"].update(smoke.file_ref(archive))
        with self.assertRaises(smoke.SmokeError):
            smoke.package_contract(fixture)

    def test_zip_traversal_is_rejected(self):
        folder = directory("traversal")
        archive = folder / "traversal.zip"
        smoke.capacity(1024)
        with zipfile.ZipFile(archive, "x") as stream:
            stream.writestr("../escape", "x")
        with self.assertRaises(smoke.SmokeError):
            smoke.zip_records(archive)

    def test_zip_case_collision_is_rejected(self):
        folder = directory("collision")
        archive = folder / "collision.zip"
        smoke.capacity(1024)
        with zipfile.ZipFile(archive, "x") as stream:
            stream.writestr("LICENSE", "x")
            stream.writestr("license", "x")
        with self.assertRaises(smoke.SmokeError):
            smoke.zip_records(archive)

    def test_zip_symlink_is_rejected(self):
        folder = directory("zip-link")
        archive = folder / "link.zip"
        smoke.capacity(1024)
        item = zipfile.ZipInfo("linked")
        item.create_system = 3
        item.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(archive, "x") as stream:
            stream.writestr(item, "outside")
        with self.assertRaises(smoke.SmokeError):
            smoke.zip_records(archive)

    def test_zip_uncompressed_quota_is_bounded(self):
        folder = directory("zip-quota")
        archive = folder / "quota.zip"
        smoke.capacity(1024)
        with zipfile.ZipFile(archive, "x") as stream:
            stream.writestr("large", "0123456789")
        with mock.patch.object(smoke, "MAX_PACKAGE", 5), self.assertRaises(smoke.SmokeError):
            smoke.zip_records(archive)

    def test_historical_ni_cannot_pass_even_when_renamed(self):
        fixture = package_fixture()
        fixture["executable"]["sha256"] = smoke.HISTORICAL_NI_SHA
        with self.assertRaises(smoke.SmokeError):
            smoke.identity_contract(fixture)

    def test_component_and_legacy_metadata_are_not_pd(self):
        fixture = package_fixture()
        for field, value in (("product", "Nickel"), ("kind", "component-probe"), ("scope", "fixture"), ("configuration", "Debug")):
            mutated = copy.deepcopy(fixture)
            mutated[field] = value
            with self.subTest(field=field), self.assertRaises(smoke.SmokeError):
                smoke.identity_contract(mutated)

    def test_external_node_runtime_is_rejected(self):
        fixture = package_fixture()
        fixture["deployment"]["externalRequirements"] = [{"kind": "shared-installed-runtime", "name": "Node.js", "minimum": "20"}]
        with self.assertRaises(smoke.SmokeError):
            smoke.identity_contract(fixture)

    def test_existing_acceptance_parser_rejects_synthetic_exe(self):
        fixture = package_fixture()
        modules = smoke.load_dependencies()
        executable = smoke.reference(fixture["executable"])
        with self.assertRaises(modules["acceptance"].PeFormatError):
            modules["acceptance"].audit(executable)

    def test_visual_receipt_needs_same_actual_exe(self):
        fixture = package_fixture()
        folder = directory("visual-binding")
        path = folder / "different-exe.json"
        smoke.write_json(path, {"sourceCommit": fixture["sourceCommit"], "executableSha256": "0" * 64,
                                "origin": "actual-production-executable", "actualExecutableObserved": True})
        self.assertEqual(smoke.receipt_binding(smoke.file_ref(path), fixture, "ui-resume")["status"], "FAIL")

    def test_fixture_visual_cannot_be_promoted(self):
        fixture = package_fixture()
        folder = directory("fixture-visual")
        path = folder / "fixture.json"
        smoke.write_json(path, {"sourceCommit": fixture["sourceCommit"], "executableSha256": fixture["executable"]["sha256"],
                                "origin": "fixture", "actualExecutableObserved": False, "status": "PASS"})
        self.assertEqual(smoke.receipt_binding(smoke.file_ref(path), fixture, "ui-resume")["status"], "NOT_RUN")

    def test_native_window_is_not_error_dialog_or_old_ni(self):
        base = {"title": "Palladium", "class": "Qt683QWindowIcon", "visible": True, "responsive": True,
                "windowOwner": 0, "rect": [0, 0, 1080, 720]}
        self.assertTrue(runtime.native_main(base))
        for changes in ({"title": "Nickel"}, {"title": "Palladium could not start"}, {"responsive": False},
                        {"visible": False}, {"class": "Chrome_WidgetWin_1"}, {"windowOwner": 42}):
            self.assertFalse(runtime.native_main({**base, **changes}))

    def test_close_refuses_other_pid(self):
        windows = runtime.Windows.__new__(runtime.Windows)
        windows.pid, windows.birth = 700001, 123
        windows.live_identity = mock.Mock(return_value={})
        windows.user = mock.Mock()
        with self.assertRaises(smoke.SmokeError):
            windows.close({"pid": 700002, "creationTime100ns": 123, "hwnd": 1})
        windows.user.PostMessageW.assert_not_called()

    def test_live_identity_refuses_pid_birth_reuse(self):
        windows = runtime.Windows.__new__(runtime.Windows)
        windows.pid, windows.birth = 700001, 123
        windows.process = types.SimpleNamespace(poll=lambda: None, process=types.SimpleNamespace(_handle=1))
        windows.observer = types.SimpleNamespace(identity=mock.Mock(side_effect=[{"creationTime100ns": 123, "executable": "owned"},
                                                                                {"creationTime100ns": 124, "executable": "owned"}]))
        with self.assertRaises(smoke.SmokeError):
            windows.live_identity()

    def test_clean_environment_does_not_inherit_developer_prefix(self):
        folder = directory("environment")
        with mock.patch.dict(os.environ, {"PATH": "D:\\Qt\\bin;C:\\Node", "QT_PLUGIN_PATH": "D:\\Qt\\plugins",
                                          "JAVA_HOME": "C:\\Java", "NICKEL_NATIVE_PREFIX": "C:\\buildcache"}):
            environment = runtime.clean_environment(folder, Path("C:/Windows"))
        self.assertNotIn("QT_PLUGIN_PATH", environment)
        self.assertNotIn("JAVA_HOME", environment)
        self.assertNotIn("NICKEL_NATIVE_PREFIX", environment)
        self.assertNotIn("Qt", environment["PATH"])
        self.assertNotIn("Node", environment["PATH"])

    def test_no_receipt_never_launches_a_fixture(self):
        worker = types.SimpleNamespace(keep=False)
        context = mock.MagicMock()
        context.__enter__.return_value = worker
        with mock.patch.object(smoke, "Worker", return_value=context), mock.patch.object(runtime, "observe") as observe:
            result = smoke.run(None, None, launch=True, seconds=3)
        observe.assert_not_called()
        self.assertEqual(result["status"], "NOT_RUN")
        self.assertFalse(result["productionExecutableObserved"])
        self.assertIn("No precise new Pd", result["gates"]["sameGeneration"]["detail"])

    def test_status_missing_is_not_success(self):
        self.assertEqual(smoke.status({}), "NOT_RUN")
        self.assertEqual(smoke.status({"runtime": {"status": "NOT_RUN"}, "source": {"status": "PASS"}}), "NOT_RUN")
        self.assertEqual(smoke.status({"runtime": {"status": "NOT_RUN"}, "hash": {"status": "FAIL"}}), "FAIL")


    def test_immutable_pd_review_does_not_use_old_checkout_metadata(self):
        result = smoke.source_review("64687e17b33091822452a1d716b489ba5c950c6a")
        self.assertTrue(result["nativePdMetadata"])
        self.assertEqual(result["displayName"], "Palladium")
        self.assertEqual(result["executableName"], "pd-launcher")
        metadata = next(row for row in result["files"] if row["path"] == "native/metadata.json")
        self.assertEqual(metadata["sha256"], "26c708fe385388fe0faffb2908c0ad3b9dcff451f2a845d2ebe2e3fc2169eddb")

    def test_release_collector_never_claims_clean_machine(self):
        fixture = package_fixture()
        folder = directory("collector")
        for name in ("launch-environment.json", "observed-modules.json", "observed-windows.json", "runtime-receipt.json"):
            smoke.write_json(folder / name, {"syntheticToolFixture": True})
        smoke.write_json(folder / "runtime-module-audit.json", {"runtimeGate": "passed-observed-scenarios", "failures": []})
        row = smoke.release_collector_evidence(fixture, {"productionExecutableObserved": False}, folder)
        result = smoke.read_json(smoke.reference(row))
        self.assertNotEqual(result["origin"], "actual-clean-windows-host-run")
        self.assertFalse(result["cleanWindowsHostProven"])
        self.assertEqual(result["status"], "pending")
        self.assertTrue(all(check["status"] == "pending" for check in result["checks"]))

    def test_candidate_raw_input_is_limited_to_explicit_roots(self):
        folder = directory("candidate-input")
        path = input_file(folder, "raw.json", b"candidate-tool-test")
        row = {"path": str(path), "bytes": path.stat().st_size, "sha256": smoke.digest(path)}
        self.assertEqual(candidate.observed_reference(row, (folder,))["sha256"], row["sha256"])
        with self.assertRaises(smoke.SmokeError):
            candidate.observed_reference(row, (folder / "other-root",))
        with self.assertRaises(smoke.SmokeError):
            candidate.observed_reference(dict(row, sha256="0" * 64), (folder,))

    def test_candidate_requires_exact_absolute_physical_d_path(self):
        for name in (None, "", "relative.json", "C:/Windows", "D:/a/../b", "D:/a:stream"):
            with self.subTest(name=name), self.assertRaises(smoke.SmokeError):
                candidate.absolute_input(name)

    def test_candidate_identity_refuses_legacy_and_wrong_generation(self):
        commit = "1" * 40
        executable = {"role": "production-executable", "physicalPath": "D:/pd-launcher.exe", "sha256": "2" * 64}
        document = {"schemaVersion": 1, "kind": "builder-linked-pd-candidate", "sourceCommit": commit,
                    "artifacts": [executable]}
        self.assertEqual(candidate.candidate_identity(document, commit), executable)
        for changed in (dict(document, sourceCommit="3" * 40), dict(document, kind="native-release"),
                        dict(document, artifacts=[dict(executable, sha256=smoke.HISTORICAL_NI_SHA)])):
            with self.assertRaises(smoke.SmokeError):
                candidate.candidate_identity(changed, commit)

    def test_candidate_source_subset_never_claims_final_release_source(self):
        commit, tree, blob = "1" * 40, "2" * 40, "3" * 40
        files = [{"path": "LICENSE", "mode": "100644", "gitObject": blob, "bytes": 3, "sha256": "4" * 64}]
        binding = {"sourceCommit": commit, "sourceTree": tree, "sourceSnapshotSha256": smoke.canonical(files)}
        capture = {"schemaVersion": 1, "kind": "builder-candidate-committed-source-capture", "sourceCommit": commit,
                   "sourceTree": tree, "sourceSnapshotSha256": smoke.canonical(files), "compositeSource": False,
                   "missingInputs": [], "files": files, "fileCount": 1, "fileBytes": 3, "exclusions": []}
        raw = ("100644 blob " + blob + "\tLICENSE\0" + "100644 blob " + "5" * 40 + "\tnative/app/main.cpp\0").encode("ascii")
        with mock.patch.object(smoke, "git_read", side_effect=[(tree + "\n").encode("ascii"), raw]):
            result = candidate.capture_selection(binding, capture)
        self.assertFalse(result["allRepositorySourcesIncluded"])
        self.assertFalse(result["finalReleaseSourcePackage"])
        self.assertEqual(result["otherUnselectedSourcePaths"], ["native/app/main.cpp"])

    def test_candidate_missing_receipt_never_launches_or_copies_runtime(self):
        folder = directory("candidate-absent")
        args = types.SimpleNamespace(linked_receipt=str(folder / "absent.json"), expected_receipt_sha256="1" * 64,
                                     expected_source_commit="2" * 40, expected_executable=str(folder / "pd-launcher.exe"),
                                     expected_executable_sha256="3" * 64, expected_executable_bytes=1)
        worker = mock.MagicMock()
        with mock.patch.object(smoke, "Worker", return_value=worker), mock.patch.object(runtime, "observe") as observe:
            result = candidate.prepare(args)
        observe.assert_not_called()
        self.assertEqual(result["status"], "NOT_RUN")
        self.assertFalse(result["nativeLaunchAttempted"])
        receipt = smoke.read_json(smoke.reference(result["receipt"]))
        self.assertEqual(receipt["copiedExecutableBytes"], 0)
        self.assertEqual(receipt["copiedRuntimeBytes"], 0)
        self.assertFalse(receipt["secondSourceRootCreated"])
        self.assertFalse(receipt["productionExecutableObserved"])

    def test_candidate_source_zip_cannot_write_outside_owned_scope(self):
        selection = {"sourceCommit": "1" * 40, "capturedFileBytes": 0, "files": []}
        with mock.patch.object(candidate.zipfile, "ZipFile") as archive, mock.patch.object(candidate.subprocess, "Popen") as process:
            with self.assertRaises(smoke.SmokeError):
                candidate.source_zip(selection, smoke.ROOT)
            archive.assert_not_called()
            process.assert_not_called()

    def test_candidate_source_zip_checks_actual_blob_bytes(self):
        commit = "647b6c0ffdba6a20d13c03062a16c639674e3fdb"
        raw = smoke.git_read("show", commit + ":native/metadata.json")
        tree_row = smoke.git_read("ls-tree", "-z", commit, "--", "native/metadata.json").rstrip(b"\0")
        header, name = tree_row.split(b"\t", 1)
        mode, kind, blob = header.decode("ascii").split()
        self.assertEqual(kind, "blob")
        row = {"path": name.decode("utf-8"), "mode": mode, "gitObject": blob, "bytes": len(raw),
               "sha256": smoke.hashlib.sha256(raw).hexdigest()}
        selection = {"sourceCommit": commit, "capturedFileBytes": len(raw), "files": [row]}
        package = candidate.source_zip(selection, directory("candidate-source-zip"))
        self.assertEqual(package["memberCount"], 1)
        self.assertFalse(package["finalReleaseSourcePackage"])
        self.assertEqual(smoke.zip_records(smoke.reference(package))[0]["sha256"], row["sha256"])
        changed = copy.deepcopy(selection)
        changed["files"][0]["sha256"] = "0" * 64
        with self.assertRaises(smoke.SmokeError):
            candidate.source_zip(changed, directory("candidate-source-bad-sha"))




def main() -> int:
    global OUTPUT
    try:
        initial = smoke.capacity(4 * 1024 * 1024)
        with smoke.Worker():
            OUTPUT = smoke.fresh_directory("tool-selftest")
            log = io.StringIO()
            suite = unittest.defaultTestLoader.loadTestsFromTestCase(ToolTests)
            result = unittest.TextTestRunner(stream=log, verbosity=2).run(suite)
            smoke.write_new(OUTPUT / "tool-tests.log", log.getvalue().encode("utf-8"))
            receipt = {"schemaVersion": 1, "kind": "pd-delivery-tool-selftest", "createdUtc": datetime.now(timezone.utc).isoformat(),
                       "status": "PASS" if result.wasSuccessful() else "FAIL", "testsRun": result.testsRun,
                       "failures": len(result.failures), "errors": len(result.errors), "skipped": len(result.skipped),
                       "actualProductionExecutableObserved": False, "completeProductVerified": False,
                       "syntheticInputs": True, "claim": "Tool tests only. No Pd deployment/startup/feature/visual acceptance.",
                       "freshCapacity": initial, "finalCapacity": smoke.capacity(),
                       "testLog": smoke.file_ref(OUTPUT / "tool-tests.log"),
                       "sources": [smoke.file_ref(HERE / name) for name in
                                   ("delivery_smoke.py", "windows_runtime.py", "prepare_linked_candidate.py", "test_delivery_smoke.py", "dependency-lock.json")]}
            smoke.write_json(OUTPUT / "tool-selftest-receipt.json", receipt)
            print(log.getvalue(), end="")
            print(json.dumps({"status": receipt["status"], "toolTests": result.testsRun,
                              "productAcceptance": False, "receipt": smoke.file_ref(OUTPUT / "tool-selftest-receipt.json")}, indent=2))
            return 0 if result.wasSuccessful() else 1
    except smoke.NotRun as error:
        print(json.dumps({"status": "NOT_RUN", "productAcceptance": False, "detail": str(error)}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
