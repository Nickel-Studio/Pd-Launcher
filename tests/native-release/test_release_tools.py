"""Synthetic tooling regression only. No fixture or helper is a launcher result."""
from __future__ import annotations

import copy
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile

import evidence_common as common
import event_trace
import performance_runner as performance
import release_evidence as release
import source_audit as source
from test_performance_contract import synthetic_wgc_receipt
from windows_metrics import WindowsObserver, GpuObserver, machine_metadata, PROTECTED_PIDS


class EvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.local = common.inside(common.ROOT, "tests/native-release/_local/selftests")
        common.check_space(cls.local, 2 * 1024 * 1024)
        cls.local.mkdir(parents=True, exist_ok=True)
        os.environ.update({"TEMP": str(cls.local), "TMP": str(cls.local), "PYTHONDONTWRITEBYTECODE": "1"})

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="owned-evidence-", dir=self.local)
        self.root = Path(self.temp.name).resolve()

    def tearDown(self):
        self.temp.cleanup()

    def material(self, name: str, raw: bytes = b"synthetic-tool-fixture-only\n") -> dict:
        path = self.root / name
        common.write_new(path, raw)
        return common.file_ref(path, self.root)

    def manifest(self, kind: str = "native-release", *, product: str = "Nickel", metadata_display: str | None = None,
                 metadata_identity: dict | None = None) -> dict:
        native = kind == "native-release"
        current = native and release.canonical_product(product) == "Palladium"
        executable_name = "pd-launcher.exe" if current else "ni-launcher.exe"
        executable = self.material(executable_name, ("synthetic not executable: " + product + "\n").encode("utf-8"))
        files = [{**executable, "role": "application", "distributionPath": executable_name}]
        evidence = {}
        if native:
            for role in ("build-log", "link-command", "link-map", "dependency-inventory"):
                row = self.material(role + ".json")
                files.append({**row, "role": role, "distributionPath": "materials/" + role + ".json"})
                evidence[role] = row
            raw_origin = evidence["build-log"]
        else:
            raw_origin = self.material("legacy-build.raw.log")
        external = [{"kind": "system", "name": "Windows", "minimum": "synthetic-only"}]
        manifest = {"schemaVersion": 1, "kind": kind, "scope": "full-launcher", "product": product, "releaseId": "synthetic-tool-test-only",
                    "version": "test-not-a-release", "configuration": "Release", "platform": "windows", "architecture": "x64",
                    "sourceCommit": "1" * 40, "executable": executable, "files": files,
                    "programPackage": {**executable, "scope": "program-runtime-only", "compression": {"format": "raw", "algorithm": "none", "level": 0}},
                    "deployment": {"fileSetComplete": True, "windowsSingleExe": True, "selfExtractsRuntime": False,
                                   "externalRequirements": external}}
        binding = {"sourceCommit": manifest["sourceCommit"], "executableSha256": executable["sha256"], "configuration": "Release"}
        if native:
            actual_inputs = []
            for name, role in (("inputs/main.cpp", "compiled-source"), ("inputs/app.hpp", "included-header"),
                               ("inputs/qrc.cpp", "generated-input"), ("inputs/resource.txt", "resource-input"),
                               ("inputs/CMakeLists.txt", "build-configuration")):
                actual_inputs.append({**self.material(name), "sourceArchivePath": name, "role": role})
            snapshot = "2" * 64
            if current:
                display = "Palladium" if product == "pd-launcher" else product
                identity = {"schemaVersion": 1, "name": "pd-launcher", "executableName": "pd-launcher",
                            "displayName": metadata_display if metadata_display is not None else display,
                            "version": manifest["version"], "applicationId": "studio.nickel.launcher"}
                identity.update(metadata_identity or {})
                metadata = self.material("inputs/native/metadata.json", common.json_bytes(identity))
                actual_inputs.append({**metadata, "sourceArchivePath": "native/metadata.json", "role": "build-configuration"})
                manifest["productIdentityInput"] = {**metadata, "sourcePath": "native/metadata.json"}
                committed = [{"path": "native/metadata.json", "mode": "100644", "gitObject": "9" * 40,
                              "sha256": metadata["sha256"], "bytes": metadata["bytes"]}]
                snapshot = common.canonical_sha(committed)
            actual_inputs.sort(key=lambda row: row["path"])
            digest = common.canonical_sha(actual_inputs)
            source_path = self.root / "source.zip"
            common.check_space(source_path, 4096)
            with zipfile.ZipFile(source_path, "x", compression=zipfile.ZIP_STORED) as archive:
                for row in actual_inputs:
                    archive.writestr(row["sourceArchivePath"], common.reference(self.root, row).read_bytes())
            source_archive = {**common.file_ref(source_path, self.root), "format": "zip"}
            binding.update({"sourceSnapshotSha256": snapshot, "buildSourceSha256": digest})
            if current:
                binding["product"] = product
            consumption = self.material("consumption.json", common.json_bytes({"schemaVersion": 1,
                "kind": "actual-native-build-input-consumption", "origin": "actual-build-commands-depfiles-and-resource-compilation",
                **binding, "files": actual_inputs, "compilerDependencyClosureComplete": True, "generatedInputsComplete": True,
                "resourceInputsComplete": True, "rawEvidence": [evidence["build-log"], evidence["link-command"]]}))
            inventory = self.material("build-inputs.json", common.json_bytes({"schemaVersion": 1,
                "kind": "actual-native-build-source-inventory", "scope": "full-launcher", **binding,
                "fileSetComplete": True, "fileCount": len(actual_inputs), "files": actual_inputs,
                "consumptionEvidence": consumption, "correspondingSourceArchive": source_archive}))
            for row, role in ((inventory, "build-source-inventory"), (consumption, "build-input-consumption"), (source_archive, "corresponding-source")):
                files.append({**row, "role": role, "distributionPath": "materials/" + Path(row["path"]).name})
            manifest.update({"sourceSnapshotSha256": snapshot, "buildSourceSha256": digest,
                             "buildSourceInventory": inventory, "buildEvidence": evidence})
        runtime_files = [{key: files[0][key] for key in ("distributionPath", "sha256", "bytes")}]
        manifest["deployment"]["runtimeClosureEvidence"] = self.material("runtime-closure.json", common.json_bytes({
            "kind": "actual-program-runtime-closure", **binding, "files": runtime_files, "externalRequirements": external,
            "rawEvidence": [raw_origin]}))
        manifest["releaseProvenance"] = self.material("release-provenance.json", common.json_bytes({
            "kind": "actual-release-provenance", "origin": "actual-source-build", **binding, "rawEvidence": [raw_origin]}))
        return manifest


    def ci_snapshot_fixture(self) -> tuple[Path, dict, dict]:
        metadata = self.material("ci-git/metadata.json", common.json_bytes({"schemaVersion": 1,
                                 "displayName": "Palladium", "version": "synthetic-mapping-only"}))
        repository = common.reference(self.root, metadata).parent
        environment = os.environ.copy()
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                    "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT"):
            environment.pop(key, None)
        environment.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
                            "GIT_AUTHOR_NAME": "Synthetic Release Tools", "GIT_AUTHOR_EMAIL": "tools@example.invalid",
                            "GIT_COMMITTER_NAME": "Synthetic Release Tools", "GIT_COMMITTER_EMAIL": "tools@example.invalid"})

        def git(*args: str, raw: bytes = b"") -> bytes:
            return subprocess.run(["git", *args], cwd=repository, env=environment, input=raw,
                                  capture_output=True, check=True, timeout=10).stdout

        git("init", "--quiet", "--template=")
        code = git("hash-object", "-w", "--stdin", raw=b"// synthetic committed blob, not launcher code\n").decode().strip()
        empty = git("hash-object", "-w", "--stdin").decode().strip()
        identity = git("hash-object", "-w", "--stdin", raw=common.reference(self.root, metadata).read_bytes()).decode().strip()
        tree = git("mktree", raw=(f"100644 blob {code}\ta.cpp\n100644 blob {code}\tb.cpp\n"
                                 f"100644 blob {empty}\tempty.txt\n100644 blob {identity}\tmetadata.json\n").encode("ascii")).decode().strip()
        parent_tree = git("mktree").decode().strip()
        parent = git("commit-tree", parent_tree, "-m", "Synthetic public parent").decode().strip()
        original = git("commit-tree", tree, "-p", parent, "-m", "Synthetic original source").decode().strip()
        source_commit = git("commit-tree", tree, "-p", parent, "-m", "Synthetic public CI snapshot").decode().strip()
        raw_path = repository / "tree.raw.log"
        common.write_new(raw_path, git("--no-replace-objects", "ls-tree", "-r", "-z", tree))
        files = source.source_snapshot(repository, source_commit)
        mapping = {"schemaVersion": 1, "kind": "actual-git-source-snapshot-mapping",
                   "origin": "actual-git-commit-tree-and-blob-verification", "originalSourceCommit": original,
                   "sourceCommit": source_commit, "sourceParentCommit": parent, "sourceTree": tree,
                   "sourceSnapshotSha256": common.canonical_sha(files), "fileCount": len(files), "sourceFiles": files,
                   "rawEvidence": [common.file_ref(raw_path, repository)]}
        mapping_path = repository / "mapping.json"
        common.write_json(mapping_path, mapping)
        artifact = {"kind": "native-release", "product": "Palladium", "sourceCommit": source_commit,
                    "originalSourceCommit": original, "sourceTree": tree, "snapshotMapping": common.file_ref(mapping_path, repository),
                    "sourceSnapshotSha256": mapping["sourceSnapshotSha256"], "buildSourceSha256": "1" * 64,
                    "executable": {"sha256": "2" * 64}, "version": "synthetic-mapping-only"}
        return repository, artifact, mapping

    def test_paths_fail_closed(self):
        for value in (None, "", "../escape", "a/../b", "/etc/passwd", "C:\\secret", "a:stream", "a//b", "./b", "\\\\host\\share"):
            with self.subTest(value=value), self.assertRaises(common.EvidenceError):
                common.inside(self.root, value)

    def test_missing_and_hash_drift_differ(self):
        with self.assertRaises(common.MissingEvidence):
            common.reference(self.root, None)
        row = self.material("observed.json")
        self.assertEqual(common.reference(self.root, row), self.root / "observed.json")
        bad = {**row, "sha256": "0" * 64}
        with self.assertRaises(common.EvidenceError):
            common.reference(self.root, bad)

    def test_duplicate_nonfinite_json_rejected(self):
        for index, raw in enumerate((b'{"status":"passed","status":"failed"}', b'{"number":NaN}', b'{"number":Infinity}')):
            path = self.root / f"bad-{index}.json"
            common.write_new(path, raw)
            with self.assertRaises(common.EvidenceError):
                common.read_json(path)

    def test_outputs_owned_and_never_overwrite(self):
        with self.assertRaises(common.EvidenceError):
            common.new_output("native/build/foreign", self.root)
        path = common.new_output("tests/native-release/_local/run", self.root)
        with self.assertRaises(common.EvidenceError):
            common.new_output("tests/native-release/_local/run", self.root)
        common.write_new(path / "raw", b"first")
        with self.assertRaises(FileExistsError):
            common.write_new(path / "raw", b"second")
        self.assertEqual((path / "raw").read_bytes(), b"first")

    def test_disk_pressure_fails_before_create(self):
        with patch("evidence_common.shutil.disk_usage", return_value=type("Disk", (), {"free": 0})()):
            with self.assertRaises(common.EvidenceError):
                common.write_new(self.root / "never-created", b"sample")
        self.assertFalse((self.root / "never-created").exists())

    def test_worker_does_not_steal_lock(self):
        with common.SingleWorker(self.root):
            with self.assertRaises(FileExistsError):
                with common.SingleWorker(self.root):
                    self.fail("A second worker must not enter")
        self.assertFalse((self.root / "single-worker.lock").exists())

    def test_owner_classification_not_brand_only(self):
        self.assertEqual(source.classify("native/core/config/paths.cpp"), "foundation")
        self.assertEqual(source.classify("native/ui/MainWindow.cpp"), "ui-resume")
        self.assertEqual(source.classify("native/assets/brand/brand.qrc"), "brand-release")
        self.assertEqual(source.classify("tests/native-release/release_evidence.py"), "release-evidence")
        self.assertEqual(source.classify("native/metadata.json"), "build-resume")
        self.assertEqual(source.classify("native/tests/app/production_tests.cpp"), "application-wire")
        self.assertIsNone(source.classify("tests/removed-owner/audit_checkout.py"))
        self.assertEqual(source.classify("tests/native-storage/storage-audit.cjs"), "build-storage")
        self.assertIsNone(source.classify("native/metadata-other.json"))
        self.assertIsNone(source.classify("tests/native-storage-other/unknown.cpp"))
        for name in ("native/core/diagnostics/junk.cpp", "native/core/diagnostics/junk.hpp",
                     "tests/native-cleanup/cleanup_tests.cpp", "tests/native-cleanup/CMakeLists.txt",
                     "tests/native-cleanup/snapshot.cjs", "tests/native-cleanup/run.cjs"):
            self.assertEqual(source.classify(name), "cleanup-resume")
        self.assertEqual(source.classify("native/core/diagnostics/session.cpp"), "instances")
        self.assertIsNone(source.classify("tests/unassigned-new-owner/unknown.cpp"))
        # Removed legacy Rust/Tauri sources must never be classified as product source.
        self.assertIsNone(source.classify("src-tauri/src/store.rs"))

    def test_raw_delta_has_all_rename_ends(self):
        raw = b":100644 100644 " + b"a" * 40 + b" " + b"b" * 40 + b" R100\0native/ui/old.cpp\0native/ui/new.cpp\0"
        self.assertEqual(source.parse_raw(raw)[0]["paths"], ["native/ui/old.cpp", "native/ui/new.cpp"])
        self.assertEqual(source.parse_raw(b""), [])
        with self.assertRaises(common.EvidenceError):
            source.parse_raw(raw[:-1])
        with self.assertRaises(common.EvidenceError):
            source.parse_raw(b":100644 100644 a b R100\0one\0")

    def test_frozen_baseline_count_and_hash(self):
        row = self.material("legacy.rs")
        self.assertEqual(len(source.baseline_files({"files": {"legacy.rs": row}}, 1)), 1)
        with self.assertRaises(common.EvidenceError):
            source.baseline_files({"files": {"legacy.rs": row}}, 442)
        with self.assertRaises(common.EvidenceError):
            source.baseline_files({"files": {"legacy.rs": {"sha256": "guess"}}}, 1)

    def test_manifest_all_materials_measured(self):
        manifest = self.manifest()
        result = release.verify_manifest(self.root, manifest, expected_kind="native-release")
        self.assertEqual(result["runtimeDeployedBytes"], manifest["executable"]["bytes"])
        self.assertEqual(result["roles"]["application"], 1)
        self.assertEqual(len(result["ancillaryFiles"]), 7)
        self.assertEqual(result["buildInputs"]["fileCount"], 5)
        self.assertEqual(result["buildInputs"]["buildSourceSha256"], manifest["buildSourceSha256"])
        self.assertEqual(result["buildInputs"]["archiveMemberCount"], 5)
        for changes in ({"buildSourceInventory": None}, {"buildSourceSha256": None}, {"buildSourceSha256": "0" * 64}):
            with self.assertRaises(common.EvidenceError):
                release.verify_manifest(self.root, {**manifest, **changes})
        document = common.read_json(common.reference(self.root, manifest["buildSourceInventory"]))
        for field in ("executableSha256", "sourceSnapshotSha256", "sourceCommit"):
            wrong = {**document, field: "0" * len(document[field])}
            ref = self.material("wrong-" + field + ".json", common.json_bytes(wrong))
            with self.assertRaises(common.EvidenceError):
                release.verify_build_inputs(self.root, {**manifest, "buildSourceInventory": ref})
        consumption = common.read_json(common.reference(self.root, document["consumptionEvidence"]))
        consumption["generatedInputsComplete"] = False
        ref = self.material("incomplete-consumption.json", common.json_bytes(consumption))
        ref = self.material("incomplete-inventory.json", common.json_bytes({**document, "consumptionEvidence": ref}))
        with self.assertRaises(common.MissingEvidence):
            release.verify_build_inputs(self.root, {**manifest, "buildSourceInventory": ref})
        archive_path = self.root / "wrong-source.zip"
        common.check_space(archive_path, 4096)
        with zipfile.ZipFile(archive_path, "x", compression=zipfile.ZIP_STORED) as archive:
            for index, row in enumerate(document["files"]):
                archive.writestr(row["sourceArchivePath"], b"wrong but real bytes" if index == 0 else common.reference(self.root, row).read_bytes())
        wrong_archive = {**common.file_ref(archive_path, self.root), "format": "zip"}
        ref = self.material("wrong-archive-inventory.json", common.json_bytes({**document, "correspondingSourceArchive": wrong_archive}))
        altered = {**manifest, "buildSourceInventory": ref, "files": [row for row in manifest["files"] if row["role"] not in {"build-source-inventory", "corresponding-source"}]}
        altered["files"] += [{**ref, "role": "build-source-inventory", "distributionPath": "materials/wrong-archive-inventory.json"},
                             {**wrong_archive, "role": "corresponding-source", "distributionPath": "materials/wrong-source.zip"}]
        with self.assertRaisesRegex(common.EvidenceError, "does not reproduce"):
            release.verify_manifest(self.root, altered)
        legacy_root = self.root / "legacy"
        legacy_root.mkdir()
        previous_root = self.root
        try:
            self.root = legacy_root
            legacy = self.manifest("legacy-release")
            self.assertNotIn("buildSourceSha256", legacy)
            self.assertNotIn("buildEvidence", legacy)
            self.assertIsNone(release.verify_manifest(legacy_root, legacy, expected_kind="legacy-release")["buildInputs"])
            with self.assertRaises(common.EvidenceError):
                release.verify_manifest(legacy_root, legacy, expected_kind="native-release")
        finally:
            self.root = previous_root

    def test_actual_build_dirty_checkout_cannot_reuse_committed_snapshot(self):
        manifest = self.manifest()
        document = common.read_json(common.reference(self.root, manifest["buildSourceInventory"]))
        release.verify_manifest(self.root, manifest)
        source = next(row for row in document["files"] if row["role"] == "compiled-source")
        path = common.reference(self.root, source)
        original = path.read_bytes()
        common.check_space(path, len(original))
        path.write_bytes(original[:-1] + b"!")
        with self.assertRaisesRegex(common.EvidenceError, "hash drift"):
            release.verify_manifest(self.root, manifest)
        source.update(common.file_ref(path, self.root))
        digest = common.canonical_sha(document["files"])
        self.assertNotEqual(digest, manifest["buildSourceSha256"])
        document["buildSourceSha256"] = digest
        ref = self.material("dirty-rehashed-build-inputs.json", common.json_bytes(document))
        changed = {**manifest, "buildSourceSha256": digest, "buildSourceInventory": ref,
                   "files": [row for row in manifest["files"] if row["role"] != "build-source-inventory"]}
        changed["files"].append({**ref, "role": "build-source-inventory", "distributionPath": "materials/dirty-rehashed-build-inputs.json"})
        self.assertEqual(changed["sourceSnapshotSha256"], manifest["sourceSnapshotSha256"])
        with self.assertRaisesRegex(common.EvidenceError, "consumption evidence does not bind"):
            release.verify_manifest(self.root, changed)

    def test_actual_build_rehashed_reports_do_not_complete_consumption(self):
        manifest = self.manifest()
        schema = common.read_json(common.ROOT / "tests/native-release/artifact-manifest.schema.json")
        native_required = set(schema["allOf"][0]["then"]["required"])
        self.assertTrue({"buildSourceSha256", "buildSourceInventory", "sourceSnapshotSha256"} <= native_required)
        document = common.read_json(common.reference(self.root, manifest["buildSourceInventory"]))
        consumption = common.read_json(common.reference(self.root, document["consumptionEvidence"]))
        cases = [(field, {field: False}, common.MissingEvidence, "closure is incomplete") for field in
                 ("compilerDependencyClosureComplete", "generatedInputsComplete", "resourceInputsComplete")]
        cases += [("no-raw", {"rawEvidence": []}, common.MissingEvidence, "no raw"),
                  ("no-link", {"rawEvidence": [manifest["buildEvidence"]["build-log"]]}, common.EvidenceError, "final link command"),
                  ("listing-only", {"origin": "committed-source-inventory"}, common.EvidenceError, "listing alone"),
                  ("unconsumed-input", {"files": consumption["files"][:-1]}, common.EvidenceError, "does not bind")]
        for label, changes, error, message in cases:
            with self.subTest(label=label):
                observed = self.material(label + "-consumption.json", common.json_bytes({**consumption, **changes}))
                inventory = self.material(label + "-inputs.json", common.json_bytes({**document, "consumptionEvidence": observed}))
                changed = {**manifest, "buildSourceInventory": inventory,
                           "files": [row for row in manifest["files"] if row["role"] not in {"build-source-inventory", "build-input-consumption"}]}
                changed["files"] += [{**inventory, "role": "build-source-inventory", "distributionPath": "materials/" + label + "-inputs.json"},
                                     {**observed, "role": "build-input-consumption", "distributionPath": "materials/" + label + "-consumption.json"}]
                self.assertEqual(changed["sourceSnapshotSha256"], manifest["sourceSnapshotSha256"])
                with self.assertRaisesRegex(error, message):
                    release.verify_manifest(self.root, changed)


    def test_palladium_aliases_preserve_legacy_and_schema_identity(self):
        schema = common.read_json(common.ROOT / "tests/native-release/artifact-manifest.schema.json")
        self.assertEqual(schema["$id"], "nickel.native.release.artifact-manifest.v1")
        self.assertEqual(schema["properties"]["schemaVersion"], {"const": 1})
        self.assertIn("productIdentityInput", schema["allOf"][3]["then"]["required"])
        aliases = ("Palladium", "Pd", "\u94af", "pd-launcher")
        self.assertEqual(set(aliases), set(schema["allOf"][3]["if"]["properties"]["product"]["enum"]))
        self.assertEqual(set(aliases), set(schema["$defs"]["sourceMappingFields"]["properties"]["product"]["enum"]))
        self.assertEqual(set(aliases) | {"Nickel", "ni-launcher"}, set(schema["properties"]["product"]["enum"]))
        self.assertEqual({"Nickel", "ni-launcher"}, set(schema["allOf"][2]["then"]["properties"]["product"]["enum"]))
        base = self.root
        for index, product in enumerate(aliases):
            with self.subTest(product=product):
                path = base / str(index)
                path.mkdir()
                with patch.object(self, "root", path):
                    manifest = self.manifest(product=product)
                    result = release.verify_manifest(path, manifest, require_current_native=True)
                    self.assertEqual(result["canonicalProduct"], "Palladium")
                    self.assertTrue(result["eligibleForCurrentNativeRelease"])
                    identity = result["buildInputs"]["productIdentity"]
                    self.assertEqual(identity["displayName"], "Palladium" if product == "pd-launcher" else product)
                    self.assertEqual((identity["name"], identity["executableName"]), ("pd-launcher", "pd-launcher"))
                    self.assertEqual(manifest["files"][0]["distributionPath"], "pd-launcher.exe")
                    with self.assertRaisesRegex(common.EvidenceError, "legacy Release identity"):
                        release.verify_manifest(path, {**manifest, "kind": "legacy-release"})
        for wrong in (None, [], {}, "palladium", "Pd-launcher", "pd-launcher.exe", "\u9489"):
            with self.subTest(wrong=wrong), self.assertRaises(common.EvidenceError):
                release.canonical_product(wrong)

    def test_historical_native_and_label_only_rename_are_not_current(self):
        manifest = self.manifest()
        old_refs = copy.deepcopy([manifest["releaseProvenance"], manifest["buildSourceInventory"], manifest["executable"]])
        result = release.verify_manifest(self.root, manifest)
        self.assertEqual(result["artifactGeneration"], "historical-native-nickel")
        self.assertFalse(result["eligibleForCurrentNativeRelease"])
        with self.assertRaisesRegex(common.MissingEvidence, "Historical native Nickel"):
            release.verify_manifest(self.root, manifest, require_current_native=True)
        with self.assertRaisesRegex(common.MissingEvidence, "Release provenance lacks current Palladium product"):
            release.verify_manifest(self.root, {**manifest, "product": "Palladium"}, require_current_native=True)
        row = self.material("historical-manifest.json", common.json_bytes(manifest))
        output = self.root / "collector"
        output.mkdir()
        collected = release.collect(self.root, {"schemaVersion": 1, "artifactManifest": row}, output)
        self.assertEqual(collected["status"], "pending")
        self.assertFalse(collected["nativeReleaseReady"])
        self.assertTrue(all(item["status"] == "pending" for item in collected["gates"].values()))
        for reference in old_refs:
            self.assertEqual(common.file_ref(common.reference(self.root, reference), self.root), reference)

    def test_palladium_actual_generation_not_rehashed_old_reports(self):
        manifest = self.manifest(product="Palladium")
        release.verify_manifest(self.root, manifest, require_current_native=True)
        inventory = common.read_json(common.reference(self.root, manifest["buildSourceInventory"]))
        consumption = common.read_json(common.reference(self.root, inventory["consumptionEvidence"]))
        for label, document in (("inventory", inventory), ("consumption", consumption)):
            for field in ("product", "sourceCommit", "sourceSnapshotSha256", "buildSourceSha256", "executableSha256"):
                for missing in (True, False):
                    with self.subTest(label=label, field=field, missing=missing):
                        changed = copy.deepcopy(document)
                        if missing:
                            changed.pop(field)
                        else:
                            changed[field] = "Nickel" if field == "product" else "0" * len(document[field])
                        ref = self.material(f"{label}-{field}-{missing}.json", common.json_bytes(changed))
                        if label == "consumption":
                            ref = self.material(f"inputs-{field}-{missing}.json", common.json_bytes({**inventory, "consumptionEvidence": ref}))
                        error = common.MissingEvidence if missing else common.EvidenceError
                        with self.assertRaises(error):
                            release.verify_build_inputs(self.root, {**manifest, "buildSourceInventory": ref})
        for label, reference in (("provenance", manifest["releaseProvenance"]),
                                 ("closure", manifest["deployment"]["runtimeClosureEvidence"])):
            changed = common.read_json(common.reference(self.root, reference))
            changed["product"] = "Nickel"
            ref = self.material(label + "-rehashed-old-product.json", common.json_bytes(changed))
            altered = copy.deepcopy(manifest)
            if label == "provenance":
                altered["releaseProvenance"] = ref
            else:
                altered["deployment"]["runtimeClosureEvidence"] = ref
            with self.assertRaisesRegex(common.EvidenceError, "different Palladium product"):
                release.verify_manifest(self.root, altered, require_current_native=True)

    def test_palladium_metadata_requires_actual_consumed_new_bytes(self):
        manifest = self.manifest(product="Palladium", metadata_display="Nickel")
        with self.assertRaisesRegex(common.EvidenceError, "consumed product metadata is historical Nickel"):
            release.verify_manifest(self.root, manifest, require_current_native=True)
        unconsumed = self.material("unconsumed-pd-metadata.json", common.json_bytes({"schemaVersion": 1,
                                   "displayName": "Palladium", "version": manifest["version"]}))
        with self.assertRaisesRegex(common.EvidenceError, "actual consumed metadata input"):
            release.verify_manifest(self.root, {**manifest, "productIdentityInput": {**unconsumed, "sourcePath": "native/metadata.json"}},
                                    require_current_native=True)
        valid_root = self.root / "valid"
        valid_root.mkdir()
        with patch.object(self, "root", valid_root):
            current = self.manifest(product="Palladium")
            with self.assertRaisesRegex(common.EvidenceError, "metadata version differs"):
                release.verify_manifest(valid_root, {**current, "version": "different"}, require_current_native=True)
            no_path = {key: value for key, value in current["productIdentityInput"].items() if key != "sourcePath"}
            with self.assertRaises(common.EvidenceError):
                release.verify_manifest(valid_root, {**current, "productIdentityInput": no_path}, require_current_native=True)


    def test_palladium_consumed_executable_identity_is_not_label_only(self):
        base = self.root
        changes = [{field: value} for field in ("name", "executableName")
                   for value in (None, "", "ni-launcher", "pd-launcher.exe", "bin/pd-launcher")]
        changes.append({"displayName": "pd-launcher"})
        for index, change in enumerate(changes):
            with self.subTest(metadata=change):
                path = base / str(index)
                path.mkdir()
                with patch.object(self, "root", path):
                    manifest = self.manifest(product="pd-launcher", metadata_identity=change)
                    with self.assertRaises(common.EvidenceError):
                        release.verify_manifest(path, manifest, require_current_native=True)

    def test_palladium_windows_distribution_exe_matches_consumed_metadata(self):
        manifest = self.manifest(product="pd-launcher")
        inputs = common.read_json(common.reference(self.root, manifest["buildSourceInventory"]))["files"]
        for name in ("ni-launcher.exe", "Nickel.exe", "palladium-test.exe", "pd-launcher", "pd-launcher.exe.exe"):
            with self.subTest(distribution=name):
                wrong = copy.deepcopy(manifest)
                wrong["files"][0]["distributionPath"] = name
                with self.assertRaisesRegex(common.EvidenceError, "EXE deployment name"):
                    release.verify_product_identity(self.root, wrong, inputs)
        for name in ("PD-LAUNCHER.EXE", "bin/pd-launcher.exe"):
            with self.subTest(distribution=name):
                matching = copy.deepcopy(manifest)
                matching["files"][0]["distributionPath"] = name
                identity = release.verify_product_identity(self.root, matching, inputs)
                self.assertEqual(identity["executableName"], "pd-launcher")

    def test_palladium_source_binding_is_not_a_label_only_snapshot(self):
        artifact = self.manifest(product="Palladium")
        metadata = artifact["productIdentityInput"]
        files = [{"path": metadata["sourcePath"], "mode": "100644", "gitObject": "9" * 40,
                  "sha256": metadata["sha256"], "bytes": metadata["bytes"]}]
        document = {"kind": "native-final-source-audit", "candidate": artifact["sourceCommit"], "baselineCount": 442,
                    "sourceSnapshotSha256": artifact["sourceSnapshotSha256"], "sourceFiles": files, "status": "passed"}
        report = self.material("pd-source.json", common.json_bytes(document))
        self.assertEqual(release.source_gate(self.root, report, artifact)["status"], "passed")
        altered = {**artifact, "productIdentityInput": {**metadata, "sourcePath": "native/not-committed-metadata.json"}}
        with self.assertRaisesRegex(common.EvidenceError, "committed source snapshot"):
            release.source_gate(self.root, report, altered)
        files[0]["sha256"] = "0" * 64
        digest = common.canonical_sha(files)
        report = self.material("rehashed-old-source.json", common.json_bytes({**document, "sourceFiles": files, "sourceSnapshotSha256": digest}))
        with self.assertRaisesRegex(common.EvidenceError, "committed source snapshot"):
            release.source_gate(self.root, report, {**artifact, "sourceSnapshotSha256": digest})


    def test_ci_snapshot_mapping_reads_real_blobs_and_preserves_original_audit(self):
        repository, artifact, document = self.ci_snapshot_fixture()
        result = release.verify_snapshot_mapping(repository, artifact)
        self.assertTrue(result["verifiedActualGitBlobs"])
        self.assertEqual(result["fileCount"], 4)
        self.assertEqual(document["sourceFiles"][0]["gitObject"], document["sourceFiles"][1]["gitObject"])
        self.assertEqual(document["sourceFiles"][2]["bytes"], 0)
        audit = {"kind": "native-final-source-audit", "candidate": artifact["originalSourceCommit"],
                 "candidateTree": artifact["sourceTree"], "baselineCount": 442,
                 "sourceSnapshotSha256": artifact["sourceSnapshotSha256"], "sourceFiles": document["sourceFiles"], "status": "passed"}
        audit_path = repository / "source-audit.json"
        common.write_json(audit_path, audit)
        report = common.file_ref(audit_path, repository)
        identity = {**common.file_ref(repository / "metadata.json", repository), "sourcePath": "metadata.json"}
        artifact = {**artifact, "productIdentityInput": identity}
        passed = release.source_gate(repository, report, artifact)
        self.assertEqual(passed["status"], "passed")
        self.assertEqual(passed["snapshotMapping"]["originalSourceCommit"], audit["candidate"])
        self.assertEqual(common.read_json(audit_path), audit)
        original = {key: value for key, value in artifact.items() if key not in {"originalSourceCommit", "sourceTree", "snapshotMapping"}}
        original["sourceCommit"] = audit["candidate"]
        self.assertTrue(release.same_source_generation(repository, artifact, original))
        self.assertTrue(release.same_source_generation(repository, original, artifact))
        with self.assertRaises(common.EvidenceError):
            release.source_gate(repository, report, {**original, "sourceCommit": artifact["sourceCommit"]})
        changed = {**audit, "candidateTree": "0" * 40}
        common.write_json(repository / "wrong-tree-audit.json", changed)
        with self.assertRaisesRegex(common.EvidenceError, "verified CI tree/blob"):
            release.source_gate(repository, common.file_ref(repository / "wrong-tree-audit.json", repository), artifact)

    def test_ci_snapshot_mapping_rehashed_claims_do_not_replace_git_bytes(self):
        repository, artifact, document = self.ci_snapshot_fixture()
        release.verify_snapshot_mapping(repository, artifact)
        for field, value in (("sha256", "0" * 64), ("bytes", 1), ("mode", "100755"), ("gitObject", "0" * 40)):
            with self.subTest(field=field):
                changed = copy.deepcopy(document)
                changed["sourceFiles"][0][field] = value
                digest = common.canonical_sha(changed["sourceFiles"])
                changed["sourceSnapshotSha256"] = digest
                path = repository / ("rehashed-" + field + ".json")
                common.write_json(path, changed)
                with self.assertRaisesRegex(common.EvidenceError, "actual committed tree/blob bytes"):
                    release.verify_snapshot_mapping(repository, {**artifact, "snapshotMapping": common.file_ref(path, repository),
                                                                   "sourceSnapshotSha256": digest})
        changed = {**document, "sourceParentCommit": artifact["originalSourceCommit"]}
        common.write_json(repository / "wrong-parent.json", changed)
        with self.assertRaisesRegex(common.EvidenceError, "exact declared safe parent"):
            release.verify_snapshot_mapping(repository, {**artifact, "snapshotMapping": common.file_ref(repository / "wrong-parent.json", repository)})
        for label, value in (("foreign", document["sourceParentCommit"]), ("missing", "0" * 40)):
            changed = {**document, "originalSourceCommit": value}
            path = repository / (label + "-original.json")
            common.write_json(path, changed)
            error = common.MissingEvidence if label == "missing" else common.EvidenceError
            with self.subTest(label=label), self.assertRaises(error):
                release.verify_snapshot_mapping(repository, {**artifact, "originalSourceCommit": value,
                                                               "snapshotMapping": common.file_ref(path, repository)})
        for field in ("originalSourceCommit", "sourceTree", "snapshotMapping"):
            with self.subTest(missing=field), self.assertRaises(common.MissingEvidence):
                release.verify_snapshot_mapping(repository, {key: value for key, value in artifact.items() if key != field})
        common.write_json(repository / "no-raw.json", {**document, "rawEvidence": []})
        with self.assertRaises(common.MissingEvidence):
            release.verify_snapshot_mapping(repository, {**artifact, "snapshotMapping": common.file_ref(repository / "no-raw.json", repository)})
        for kind, product in (("legacy-release", "Nickel"), ("native-release", "Nickel")):
            with self.subTest(kind=kind), self.assertRaises(common.EvidenceError):
                release.verify_snapshot_mapping(repository, {**artifact, "kind": kind, "product": product})

    def test_ci_snapshot_mapping_never_aliases_product_build_receipts(self):
        repository, artifact, document = self.ci_snapshot_fixture()
        release.verify_snapshot_mapping(repository, artifact)
        binding = {key: artifact[key] for key in ("product", "sourceCommit", "sourceSnapshotSha256", "buildSourceSha256")}
        binding["executableSha256"] = artifact["executable"]["sha256"]
        release.bind_native_generation(binding, artifact, "Actual CI build")
        with self.assertRaisesRegex(common.EvidenceError, "build generation"):
            release.bind_native_generation({**binding, "sourceCommit": artifact["originalSourceCommit"]}, artifact, "Unmapped old product")
        with self.assertRaisesRegex(common.EvidenceError, "build generation"):
            release.bind_native_generation({**binding, "buildSourceSha256": "0" * 64}, artifact, "Dirty actual build")
        unrelated = {"kind": "native-release", "product": "Palladium", "sourceCommit": document["sourceParentCommit"],
                     "sourceSnapshotSha256": artifact["sourceSnapshotSha256"]}
        self.assertFalse(release.same_source_generation(repository, artifact, unrelated))
        self.assertFalse(release.same_source_generation(repository, artifact, {**unrelated, "sourceCommit": artifact["originalSourceCommit"],
                                                                             "sourceSnapshotSha256": "0" * 64}))
        with self.assertRaises(common.MissingEvidence):
            release.verify_manifest(repository, {"schemaVersion": 1, "kind": "native-release", "scope": "full-launcher", **artifact,
                                      "releaseId": "mapping-only", "configuration": "Release", "platform": "windows", "architecture": "x64",
                                      "snapshotMapping": {"path": "missing-map.json", "sha256": "0" * 64, "bytes": 1}})
        schema = common.read_json(common.ROOT / "tests/native-release/artifact-manifest.schema.json")
        self.assertEqual(set(schema["$defs"]["sourceMappingFields"]["required"]),
                         {"originalSourceCommit", "sourceTree", "snapshotMapping"})

    def test_palladium_old_function_legal_performance_reports_stay_pending(self):
        artifact = self.manifest(product="Palladium")
        binding = {"product": artifact["product"], "sourceCommit": artifact["sourceCommit"],
                   "sourceSnapshotSha256": artifact["sourceSnapshotSha256"], "buildSourceSha256": artifact["buildSourceSha256"],
                   "executableSha256": artifact["executable"]["sha256"]}
        old = {"kind": "full-launcher-functional-acceptance", "scope": "full-launcher",
               "sourceCommit": artifact["sourceCommit"], "executableSha256": artifact["executable"]["sha256"]}
        report = self.material("old-function.json", common.json_bytes(old))
        with self.assertRaises(common.MissingEvidence):
            release.function_gate(self.root, report, artifact)
        current = self.material("pd-function-incomplete.json", common.json_bytes({**old, **binding}))
        self.assertEqual(release.function_gate(self.root, current, artifact)["status"], "pending")
        materials = self.material("legal/materials-manifest.json")
        self.material("legal/input-materials.json", common.json_bytes({"scope": "native-release", "releaseId": artifact["releaseId"],
                      "build": {"executable": artifact["executable"]}}))
        with self.assertRaises(common.MissingEvidence):
            release.legal_gate(self.root, {}, {"path": "legal", "manifest": materials}, artifact, self.root)
        report = self.material("pd-performance-no-runs.json", common.json_bytes({"kind": "full-launcher-performance-evidence",
                               **binding, "status": "passed", "gates": {}, "rawRuns": []}))
        with self.assertRaisesRegex(common.MissingEvidence, "real raw runs"):
            release.performance_report_gate(self.root, report, artifact)
        report = self.material("old-performance.json", common.json_bytes({"kind": "full-launcher-performance-evidence",
                               **binding, "product": "Nickel", "status": "passed"}))
        with self.assertRaisesRegex(common.EvidenceError, "different Palladium product"):
            release.performance_report_gate(self.root, report, artifact)

    def test_palladium_isolation_and_equivalence_need_same_generation(self):
        artifact = self.manifest(product="Palladium")
        raw = self.material("synthetic-ui-observer.raw.log")
        driver = self.material("synthetic-driver.py", b"# synthetic protocol test only, never launched\n")
        launcher = {"launchArgs": ["--context", "{context}"], "environment": {}, "driver": {"source": driver}}
        binding = {"product": artifact["product"], "sourceCommit": artifact["sourceCommit"],
                   "sourceSnapshotSha256": artifact["sourceSnapshotSha256"], "buildSourceSha256": artifact["buildSourceSha256"],
                   "executableSha256": artifact["executable"]["sha256"]}
        document = {"kind": "actual-launcher-isolation-evidence", **binding,
                    "launchConfigurationSha256": performance.launch_configuration_sha(launcher), "pathContextProtocol": "explicit-accepted-path-context-v1",
                    "systemKnownFolderAccess": "disabled", "status": "passed", "allDataRootsWithinRunFixture": True,
                    "executableGuardPassed": True, "fixturePathsGuardPassed": True, "method": "synthetic-unit-test-only", "rawEvidence": [raw]}
        ref = self.material("isolation.json", common.json_bytes(document))
        launcher["runtimeIsolationEvidence"] = ref
        self.assertEqual(performance.runtime_isolation(self.root, launcher, artifact), document)
        for field in ("product", "sourceSnapshotSha256", "buildSourceSha256"):
            incomplete = {key: value for key, value in document.items() if key != field}
            ref = self.material("isolation-missing-" + field + ".json", common.json_bytes(incomplete))
            with self.assertRaises(common.MissingEvidence):
                performance.runtime_isolation(self.root, {**launcher, "runtimeIsolationEvidence": ref}, artifact)
        config = common.read_json(common.ROOT / "tests/native-release/performance.example.json")
        config["inputCorpus"] = [raw]
        manifests = {"native": artifact, "legacy": {"product": "Nickel", "sourceCommit": "3" * 40, "executable": {"sha256": "4" * 64}}}
        document = {"kind": "full-launcher-equivalence-evidence", "scope": "full-launcher", "status": "passed", "featuresComplete": True,
                    "visualsPreserved": True, "scenarios": sorted(performance.COMMON_SCENARIOS), "themes": sorted(performance.THEME_IDS),
                    "expectedStates": {case["id"]: case["expectedState"] for case in config["scenarios"] if case["comparison"] == "legacy-common"},
                    "inputCorpusSha256": common.canonical_sha(config["inputCorpus"]), "rawEvidence": [raw],
                    "products": {role: value["product"] for role, value in manifests.items()},
                    "executableSha256s": {role: value["executable"]["sha256"] for role, value in manifests.items()},
                    "sourceCommits": {role: value["sourceCommit"] for role, value in manifests.items()},
                    "sourceSnapshotSha256s": {role: value.get("sourceSnapshotSha256") for role, value in manifests.items()},
                    "buildSourceSha256s": {role: value.get("buildSourceSha256") for role, value in manifests.items()}}
        ref = self.material("equivalence.json", common.json_bytes(document))
        self.assertEqual(performance.validate_equivalence(self.root, ref, manifests, config), document)
        for field in ("products", "sourceSnapshotSha256s", "buildSourceSha256s"):
            incomplete = {key: value for key, value in document.items() if key != field}
            ref = self.material("equivalence-missing-" + field + ".json", common.json_bytes(incomplete))
            with self.assertRaises(common.MissingEvidence):
                performance.validate_equivalence(self.root, ref, manifests, config)


    def test_probe_and_debug_manifest_not_full_product(self):
        manifest = self.manifest()
        for scope in ("probe", "assets-only", "component", "prototype"):
            altered = {**manifest, "scope": scope}
            with self.assertRaises(common.EvidenceError):
                release.verify_manifest(self.root, altered)
        with self.assertRaises(common.EvidenceError):
            release.verify_manifest(self.root, {**manifest, "configuration": "Debug"})

    def test_runtime_sidecar_not_single_exe(self):
        manifest = self.manifest()
        manifest["files"].append({**self.material("qt.dll"), "role": "runtime-dependency", "distributionPath": "Qt6Core.dll"})
        with self.assertRaises(common.EvidenceError):
            release.verify_manifest(self.root, manifest)
        manifest["files"].pop()
        manifest["deployment"]["selfExtractsRuntime"] = True
        with self.assertRaises(common.EvidenceError):
            release.verify_manifest(self.root, manifest)

    def test_manifest_case_collision_and_hash_drift(self):
        manifest = self.manifest()
        manifest["files"].append({**manifest["files"][0], "distributionPath": "NI-LAUNCHER.EXE"})
        with self.assertRaises(common.EvidenceError):
            release.verify_manifest(self.root, manifest)
        manifest["files"].pop()
        manifest["files"][1]["sha256"] = "0" * 64
        with self.assertRaises(common.EvidenceError):
            release.verify_manifest(self.root, manifest)

    def test_no_artifact_collector_is_explicit_pending(self):
        output = self.root / "result"
        output.mkdir()
        result = release.collect(self.root, {"schemaVersion": 1, "artifactManifest": None}, output)
        self.assertEqual(result["status"], "pending")
        self.assertFalse(result["nativeReleaseReady"])
        self.assertTrue(all(value["status"] == "pending" for value in result["gates"].values()))

    def test_observed_hash_binding(self):
        report = self.material("report.json", common.json_bytes({"kind": "full-launcher-functional-acceptance", "executableSha256": "0" * 64}))
        with self.assertRaises(common.EvidenceError):
            release.bind_report(self.root, report, "1" * 64)

    def test_source_audit_final_commit_binding(self):
        artifact = self.manifest()
        report = self.material("source.json", common.json_bytes({"kind": "native-final-source-audit", "candidate": "3" * 40,
                                                               "sourceSnapshotSha256": artifact["sourceSnapshotSha256"], "baselineCount": 442, "status": "passed"}))
        with self.assertRaises(common.EvidenceError):
            release.source_gate(self.root, report, artifact)

    def test_component_function_tests_not_complete(self):
        artifact = self.manifest()
        report = self.material("function.json", common.json_bytes({"kind": "full-launcher-functional-acceptance", "scope": "component",
                                                                  "executableSha256": artifact["executable"]["sha256"], "sourceCommit": artifact["sourceCommit"]}))
        with self.assertRaises(common.EvidenceError):
            release.function_gate(self.root, report, artifact)

    def test_macos_no_host_is_pending(self):
        report = self.material("macos.json", common.json_bytes({"kind": "native-platform-release-evidence", "scope": "full-launcher",
                                                               "platform": "macos", "architecture": "arm64", "sourceCommit": "1" * 40,
                                                               "executedOnTargetHost": False, "host": None}))
        with self.assertRaises(common.MissingEvidence):
            release.platform_gate(self.root, report, "macos-arm64", "1" * 40)

    def test_legal_missing_is_pending(self):
        with self.assertRaises(common.MissingEvidence):
            release.legal_gate(self.root, None, None, self.manifest(), self.root)

    def test_stats_median_tail_no_guesses(self):
        self.assertIsNone(performance.stats([]))
        result = performance.stats(list(range(1, 21)))
        self.assertEqual(result["median"], 10.5)
        self.assertEqual(result["p95NearestRank"], 19)
        self.assertEqual(result["max"], 20)

    def test_ready_state_identity_gate(self):
        schema = common.read_json(common.ROOT / "tests/native-release/driver-protocol.schema.json")
        self.assertTrue({"role", "observationAdapter", "phaseRequestNonce"} <= set(schema["required"]))
        self.assertTrue({"sourceSnapshotSha256", "buildSourceSha256"} <= set(schema["allOf"][2]["then"]["required"]))
        self.assertNotIn("buildSourceSha256", schema["allOf"][3]["then"].get("required", []))
        self.assertEqual(set(schema["$defs"]["state"]["properties"]["themeId"]["enum"]), performance.THEME_IDS)
        self.assertIn("externalClientFramePresented", schema["$defs"]["nativeUiObservation"]["required"])
        self.assertNotIn("oneOf", schema["$defs"]["nativeUiObservation"])
        state = {"route": "home", "windowLogicalSize": [16, 12], "dpiPercent": 100, "fontProfile": "same-machine",
                 "themeId": "mono", "mode": "dark", "backgroundSha256": None, "blur": 0,
                 "reducedMotion": False, "fixtureId": "owned-synthetic", "pendingImageCount": 0, "activeTaskCount": 0}
        for role in ("native", "legacy"):
            with self.subTest(role=role), patch.object(performance.time, "perf_counter_ns", return_value=1000):
                adapter = "native-production-markers-v1" if role == "native" else "legacy-external-ui-observer-v1"
                context = {"runId": "owned-run", "scenarioId": "home-no-background", "requestNonce": "owned-nonce",
                           "phaseRequestNonce": "ready-phase-nonce", "phase": "ready", "role": role, "observationAdapter": adapter,
                           "sourceCommit": "3" * 40, "sourceSnapshotSha256": "4" * 64, "buildSourceSha256": "6" * 64,
                           "driverSourceSha256": "5" * 64, "executableSha256": "1" * 64, "rootPid": 123456,
                           "rootCreationTime100ns": 123, "inputCorpusSha256": "2" * 64, "fixtureRoot": str(self.root),
                           "launchBeginNs": 100, "phaseBeginNs": 100}
                raw = self.material(role + "-synthetic-receipt.raw.txt", b"synthetic unit test only, not product observation\n")
                marker = {"observed": True, "rootPid": context["rootPid"], "scenarioId": context["scenarioId"],
                          "requestNonce": context["requestNonce"], "method": "synthetic-unit-test-only", "rawEvidence": [raw]}
                if role == "native":
                    receipt = synthetic_wgc_receipt(self.root, context, state, prefix="native-ready")
                else:
                    observation = {"platform": "windows", "clock": "perf_counter_ns",
                        "windowIdentity": {"hwnd": 88888, "rootPid": context["rootPid"], "rootCreationTime100ns": 123},
                        "scenarioAppliedBeforePaint": {**marker, "monotonicNs": 110},
                        "presentDwmConfirmed": {**marker, "method": "DwmFlush", "hresult": 0, "monotonicNs": 140},
                        "externalClientFramePresented": {**marker, "monotonicNs": 150,
                            "method": "windows-graphics-capture-client-frame", "hwnd": 88888, "clientOnly": True,
                            "frozenStateObserved": True, "frameEvidence": raw},
                        "inputReady": {"observed": True, "rootPid": context["rootPid"], "scenarioId": context["scenarioId"],
                            "requestNonce": context["phaseRequestNonce"], "replyRequestNonce": context["phaseRequestNonce"],
                            "queuedEventMonotonicNs": 155, "queryMonotonicNs": 160, "replyMonotonicNs": 170,
                            "query": "observed-legacy-runtime-state", "method": "uia-reversible-action-observed-response",
                            "rawEvidence": [raw], "normalization": "external-driver-only", "productNonceSupported": False,
                            "responseObserved": True, "frozenStateRestored": True}}
                    receipt = {"schemaVersion": 1, "protocol": "nickel-evidence-driver-v1", "origin": "actual-launcher-ui-observation",
                        **context, "state": state, "ready": True, "inputResponsive": True, "readyMonotonicNs": 180,
                        "pendingImageCount": 0, "uiObservation": observation,
                        "dataRoots": {key: str(self.root / key) for key in ("config", "game", "cache", "temp")},
                        "pathContextAcceptance": {"protocol": "legacy-isolated-runtime-v1", "requestNonce": context["requestNonce"],
                            "explicitAcceptedPathContext": True, "executableGuardPassed": True, "fixturePathsGuardPassed": True,
                            "allDataRootsWithinRunFixture": True, "systemKnownFolderAccess": "fixture-isolated-os-profile", "rawEvidence": [raw]}}
                observation = receipt["uiObservation"]
                performance.validate_receipt(receipt, context, state, "ready", root=self.root)
                for changed in ({"rootPid": 1}, {"rootCreationTime100ns": 456}, {"phase": "other"}, {"phaseRequestNonce": "stale"},
                                {"observationAdapter": "wrong-adapter"}, {"pendingImageCount": 4}, {"sourceSnapshotSha256": "0" * 64},
                                {"sourceCommit": "0" * 40}, {"runId": "other-run"}, {"scenarioId": "other-scenario"},
                                {"requestNonce": "stale"}, {"driverSourceSha256": "0" * 64}, {"executableSha256": "0" * 64},
                                {"inputCorpusSha256": "0" * 64}, {"protocol": "pd-evidence-driver-v1"}, {"origin": "synthetic-observation"},
                                {"buildSourceSha256": "0" * 64}, {"buildSourceSha256": None},
                                {"state": {**state, "mode": "light"}}, {"dataRoots": {**receipt["dataRoots"], "config": str(self.root.parent)}}):
                    with self.assertRaises(common.EvidenceError):
                        performance.validate_receipt({**receipt, **changed}, context, state, "ready", root=self.root)
                for unsupported in ({"platform": "linux"}, {"platform": "macos"}, {"clock": "std::chrono::steady_clock"}):
                    with self.subTest(unsupported=unsupported), self.assertRaises(common.MissingEvidence):
                        performance.validate_receipt({**receipt, "uiObservation": {**observation, **unsupported}}, context, state, "ready", root=self.root)
                unbound = {key: value for key, value in context.items() if key not in {"sourceSnapshotSha256", "buildSourceSha256"}}
                unbound_receipt = {key: value for key, value in receipt.items() if key not in {"sourceSnapshotSha256", "buildSourceSha256"}}
                if role == "native":
                    with self.assertRaises(common.MissingEvidence):
                        performance.validate_receipt(unbound_receipt, unbound, state, "ready", root=self.root)
                else:
                    performance.validate_receipt(unbound_receipt, unbound, state, "ready", root=self.root)
                with self.assertRaises(common.MissingEvidence):
                    performance.validate_receipt({**receipt, "uiObservation": {"startupReady": True}}, context, state, "ready", root=self.root)
                corruptions = [("inputReady", {"replyRequestNonce": "stale"}), ("windowIdentity", {"rootCreationTime100ns": 456}),
                               ("inputReady", {"replyMonotonicNs": 1100}), ("externalClientFramePresented", {"method": "window-title-discovery"})]
                if role == "legacy":
                    corruptions += [("presentDwmConfirmed", {"hresult": -1}), ("inputReady", {"frozenStateRestored": False}),
                                    ("inputReady", {"productNonceSupported": True})]
                with self.assertRaises(common.EvidenceError):
                    performance.validate_receipt({**receipt, "uiObservation": {**observation, "paintComplete": marker}}, context, state, "ready", root=self.root)
                for marker_name, change in corruptions:
                    wrong = copy.deepcopy(receipt)
                    wrong["uiObservation"][marker_name].update(change)
                    with self.assertRaises(common.EvidenceError):
                        performance.validate_receipt(wrong, context, state, "ready", root=self.root)
                fresh_context = {**context, "phase": "state", "phaseRequestNonce": "state-phase-nonce", "phaseBeginNs": 300}
                fresh = copy.deepcopy(receipt)
                fresh.update({**fresh_context, "readyMonotonicNs": 360})
                with self.assertRaises(common.EvidenceError):
                    performance.validate_receipt(fresh, fresh_context, state, "state", root=self.root)
                if role == "native":
                    fresh = synthetic_wgc_receipt(self.root, fresh_context, state, prefix="native-state")
                else:
                    fresh["uiObservation"]["inputReady"].update({"requestNonce": "state-phase-nonce", "replyRequestNonce": "state-phase-nonce",
                        "queuedEventMonotonicNs": 310, "queryMonotonicNs": 320, "replyMonotonicNs": 330})
                performance.validate_receipt(fresh, fresh_context, state, "state", root=self.root)
                shutdown_context = {**context, "phase": "shutdown", "phaseRequestNonce": "shutdown-phase-nonce", "phaseBeginNs": 500}
                shutdown = {**receipt, **shutdown_context, "shutdownObserved": True, "shutdownMonotonicNs": 510, "rawEvidence": [raw]}
                performance.validate_receipt(shutdown, shutdown_context, state, "shutdown", root=self.root)
                for change in ({"shutdownMonotonicNs": 499}, {"shutdownMonotonicNs": 1001}, {"phaseRequestNonce": context["phaseRequestNonce"]},
                               {"sourceCommit": "0" * 40}, {"shutdownObserved": False}, {"rawEvidence": []}):
                    with self.subTest(shutdown=change), self.assertRaises(common.EvidenceError):
                        performance.validate_receipt({**shutdown, **change}, shutdown_context, state, "shutdown", root=self.root)
        for size in ([1100], [0, 720], [True, 720], [1100, 720, 1]):
            with self.assertRaises(common.EvidenceError):
                performance.validate_state({**state, "windowLogicalSize": size})

    def test_preflight_never_launches_missing_products(self):
        config = common.read_json(common.ROOT / "tests/native-release/performance.example.json")
        with self.assertRaises(common.MissingEvidence):
            performance.preflight(self.root, config)
        with self.assertRaises(common.EvidenceError):
            performance.preflight(self.root, {**config, "workerCount": 2})

    def test_argv_no_shell_and_unknown_placeholders(self):
        self.assertEqual(performance.expand_argv(["--root", "{fixture}"], {"fixture": "D:\\owned space"}), ["--root", "D:\\owned space"])
        with self.assertRaises(common.EvidenceError):
            performance.expand_argv(["{unknown}"], {})

    def test_event_lifetime_keeps_short_children_pending(self):
        events = [{"kind": "start", "pid": 10, "parentPid": 1, "eventTime100ns": 105},
                  {"kind": "start", "pid": 20, "parentPid": 10, "eventTime100ns": 110},
                  {"kind": "start", "pid": 30, "parentPid": 20, "eventTime100ns": 120},
                  {"kind": "stop", "pid": 30, "eventTime100ns": 130},
                  {"kind": "stop", "pid": 20, "eventTime100ns": 140},
                  {"kind": "stop", "pid": 10, "eventTime100ns": 150}]
        result = event_trace.trace_descendants(events, 10, 100, {(10, 100), (20, 108)}, trace_observed=True)
        self.assertEqual(result["allTracedDescendantsCount"], 2)
        self.assertFalse(result["completeObservedLifetime"])
        self.assertEqual(result["unobservedShortLivedDescendants"][0]["pid"], 30)
        observed = {(10, 100), (20, 108), (30, 115)}
        self.assertTrue(event_trace.trace_descendants(events, 10, 100, observed, trace_observed=True)["completeObservedLifetime"])
        self.assertFalse(event_trace.trace_descendants(events, 10, 100, observed)["completeObservedLifetime"])
        self.assertFalse(event_trace.trace_descendants([], 10, 100, observed)["completeObservedLifetime"])
        self.assertFalse(event_trace.trace_descendants(events, 10, 100, observed | {(30, 155)}, trace_observed=True)["completeObservedLifetime"])

    def test_protected_cli_pids(self):
        self.assertIn(47516, PROTECTED_PIDS)
        self.assertIn(52148, PROTECTED_PIDS)
        self.assertIn(os.getpid(), PROTECTED_PIDS)

    @unittest.skipUnless(os.name == "nt" and os.environ.get("PD_RELEASE_TOOLS_NO_LIVE_OBSERVER") != "1",
                         "Actual live observer selftest requires Windows and authorization")
    def test_actual_windows_owned_helper_tree_and_modules(self):
        # This helper is not a launcher and its metrics are never release performance evidence.
        raw = "import subprocess,sys,time;p=subprocess.Popen([sys.executable,'-B','-c','import time;time.sleep(2)']);time.sleep(2.5);p.wait()"
        process = subprocess.Popen([sys.executable, "-B", "-c", raw], cwd=self.root)
        try:
            observer = WindowsObserver(process.pid)
            deadline, sample = time.monotonic() + 1.5, None
            while time.monotonic() < deadline:
                sample = observer.sample()
                if sample.get("totals") and sample["totals"]["processCount"] >= 2:
                    break
                time.sleep(0.05)
            self.assertIsNotNone(sample["totals"], sample["errors"])
            self.assertGreaterEqual(sample["totals"]["processCount"], 2)
            self.assertGreater(sample["totals"]["privateBytes"], 0)
            self.assertGreater(sample["totals"]["privateWorkingSetBytes"], 0)
            modules = observer.module_snapshot("observer-selftest-not-launcher")
            self.assertTrue(modules["treeComplete"], modules["errors"])
            self.assertTrue(all(row["moduleStatus"] == "observed" for row in modules["processTree"]))
        finally:
            process.wait(timeout=5)

    @unittest.skipUnless(os.name == "nt" and os.environ.get("PD_RELEASE_TOOLS_NO_LIVE_OBSERVER") != "1",
                         "Actual GPU selftest requires Windows and authorization")
    def test_gpu_absence_is_unavailable_not_zero(self):
        gpu = GpuObserver()
        try:
            result = gpu.sample({os.getpid()})
            if result["status"] == "unavailable":
                self.assertIsNone(result["bytes"])
                self.assertTrue(result["reason"])
            else:
                self.assertEqual(result["status"], "observed")
        finally:
            gpu.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
