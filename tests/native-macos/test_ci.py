#!/usr/bin/env python3
"""Host-independent CI contract/negative tests, never native macOS acceptance."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import subprocess
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("delivery_ci", ROOT / "scripts/native/macos-delivery.ci.py")
ci = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ci)
api = ci.api


class CiGates(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="macos-ci-unit-", dir=Path(__file__).parent)
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, relative, content=b"synthetic source, never a real application"):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def complete_synthetic_source(self):
        paths = ["CMakeLists.txt", "native/app/CMakeLists.txt", "native/app/main.cpp", "native/app/AppServices.cpp",
                 "native/ui/MainWindow.cpp", "native/ui/CMakeLists.txt", "native/tests/ui/CMakeLists.txt", "scripts/native/deps-lock.json"]
        for relative in paths:
            self.write(relative)
        return paths

    def test_frozen_raw_publication_and_official_supplement(self):
        inputs = api.validate_inputs(ROOT / "scripts/native/macos-delivery.inputs.json")
        pins = ROOT / inputs["frozenInputs"]["root"]
        supplement = ROOT / "scripts/native/macos-delivery.supplement.json"
        result = api.verify_source_contract(pins / "deps-lock.json", pins / "source-contract.json", inputs["frozenInputs"]["sourceContractSha256"],
                                             inputs, pins / "verification-rules.json", supplement, api.digest(supplement))
        self.assertEqual(len(result["sourceRecords"]), 14)
        self.assertFalse(result["productionSourceBytesVerified"])
        self.assertFalse(result["archiveBytesVerified"])
        self.assertEqual(next(item for item in result["sourceRecords"] if item["name"] == "openssl")["version"], "3.5.9")
        with self.assertRaises(api.DeliveryError):
            api.verify_source_contract(pins / "deps-lock.json", pins / "source-contract.json", inputs["frozenInputs"]["sourceContractSha256"], inputs)
        with self.assertRaises(api.DeliveryError):
            api.verify_source_contract(pins / "deps-lock.json", pins / "source-contract.json", inputs["frozenInputs"]["sourceContractSha256"],
                                        inputs, pins / "verification-rules.json", supplement, "0" * 64)

    def test_generated_user_and_legacy_trees_are_not_sources(self):
        for relative in ["tests/native-portability/scratch/run-old/CMakeCache.txt", "native/.build/CMakeLists.txt", "native/user/accounts.json",
                         "native/user/credentials.key", "scripts/native/.env", "src/App.tsx", "src-tauri/Cargo.toml", ".snow/secret.json",
                         "native/evidence/fixture.json", "native/app/App.app/Contents/Info.plist",
                         "native/tests/minecraft/.build-policy/CMakeFiles/compiler.cpp", "native/tests/minecraft/.deps/nlohmann/json.hpp",
                         "native/tests/app/.temp/generated.cpp", "native/tests/ui/CMakeFiles/configure.cmake", "native/.tools-qt/config.json",
                         "native/download.zip", "tests/native-macos/inputs/.build-old/generated.zip"]:
            with self.subTest(relative=relative):
                self.assertFalse(ci.canonical_path(relative))
        for relative in ["native/ui/MainWindow.cpp", "native/assets/brand/brand.qrc", "cmake/NickelDependencies.cmake", "scripts/native/macos-delivery.py",
                         "tests/native-macos/inputs/qrc-relocation-647b6c/source.zip"]:
            self.assertTrue(ci.canonical_path(relative))
        with self.assertRaises(api.DeliveryError):
            ci.canonical_path("../source.cpp")

    def test_staged_build_input_extensions_cover_inc_cjs_rs_and_tooling_pairs(self):
        for extension in (".inc", ".cjs", ".rs", ".mjs", ".lock", ".manifest"):
            self.assertIn(extension, ci.EXTENSIONS)
        # Tracked staged-root build inputs the hosted app/probes phases actually consume.
        for relative in ("native/core/config/conditional_file_impl.inc",
                         "native/tests/identity/generate_builtin_heads.cjs",
                         "native/tests/identity/generate_offline_uuid_golden.cjs",
                         "native/tests/identity/offline_uuid_legacy_oracle.rs",
                         "tests/native-portability/probe.manifest",
                         "scripts/native/release_assets.mjs",
                         "scripts/native/release_assets.test.mjs",
                         "scripts/native/test-fixture-python.lock"):
            with self.subTest(relative=relative):
                self.assertTrue(ci.canonical_path(relative))

    def test_conditional_file_impl_inc_stages_with_the_config_domain(self):
        paths = self.complete_synthetic_source()
        self.write("native/core/config/atomic_file.cpp")
        self.write("native/core/config/conditional_file_impl.inc", b"// tracked C++ implementation fragment\n")
        manifest = ci.inventory(self.root, paths + ["native/core/config/atomic_file.cpp",
                                                    "native/core/config/conditional_file_impl.inc"], "a" * 40)
        selected = {item["path"] for item in manifest["files"]}
        self.assertIn("native/core/config/conditional_file_impl.inc", selected)
        target = self.root / "inc-manifest.json"
        api.write_json(target, manifest)
        ci.stage(self.root, target, api.digest(target), self.root / "inc-stage")
        self.assertEqual((self.root / "inc-stage/native/core/config/conditional_file_impl.inc").read_bytes(),
                         b"// tracked C++ implementation fragment\n")

    def test_real_tracked_staged_roots_are_fully_classified_and_fail_closed(self):
        result = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True, timeout=60)
        tracked = [path for path in result.stdout.decode("utf-8").split("\x00") if path]
        self.assertGreater(len(tracked), 500)
        self.assertEqual(ci.staging_inventory_gaps(tracked), [])
        self.assertGreater(len([path for path in tracked if ci.staged_source_path(path)]), 500)
        unclassified = "native/core/config/unclassified_future_input.xyz"
        self.assertEqual(ci.staging_inventory_gaps(tracked + [unclassified]), [unclassified])

    def test_staging_exclusions_are_explicit_not_silent(self):
        for relative in ("native/tests/app/build-composed/build.ninja",
                         "native/tests/app/build-composed/domains/minecraft/nickel_minecraft_autogen/timestamp",
                         "native/tests/app/build-composed/nickel_application_composed_autogen/2PTVP7XNZ3/moc_UiServices.cpp.d",
                         "native/tests/app/build-registry/build.ninja",
                         "native/tests/minecraft/.build-policy/CMakeFiles/rules.ninja",
                         "native/tests/minecraft/.build-policy/CMakeFiles/nickel_minecraft_policy_tests.dir/embed.manifest",
                         "native/tests/app/.temp/.keep",
                         "native/tests/identity/.gitignore",
                         "native/tests/brand/.gitattributes",
                         "native/assets/brand/.gitattributes",
                         "scripts/native/.gitignore",
                         "scripts/native/__pycache__/msvc.cpython-315.pyc",
                         "tests/native-cmake-generation/records/20261121T1808Z-df434-committed/generated/committed-before/build.ninja",
                         "tests/native-cmake-generation/records/20261121T1757Z-caa879-raw/raw.trace.jsonl",
                         "tests/native-capacity-pathfinder/java-path-probe.java",
                         "tests/native-capacity-pathfinder/run-probes.py",
                         "tests/native-capacity-pathfinder/runs/20261121T160525Z-6b7e919c/temp/node-compile-cache/v26.12.1-x64-310b2ce3/056407a9",
                         "tests/native-capacity-pathfinder/runs/20261121T160525Z-6b7e919c/wsl/syntax-probe.s"):
            with self.subTest(relative=relative):
                self.assertFalse(ci.staged_source_path(relative))
                self.assertTrue(ci.excluded_staging_input(relative))
        # Out-of-scope and root non-sources stay unstaged; the latter are handled by the explicit root allowlist.
        for relative in ("README.md", "src/App.tsx", "native/download.zip"):
            self.assertFalse(ci.staged_source_path(relative))

    def test_inventory_fails_closed_on_unclassified_staged_root_input(self):
        paths = self.complete_synthetic_source()
        self.write("native/core/config/unclassified_input.xyz", b"unknown tracked input")
        with self.assertRaisesRegex(api.DeliveryError, "neither includable nor explicitly excluded"):
            ci.inventory(self.root, paths + ["native/core/config/unclassified_input.xyz"], "a" * 40)

    def test_preflight_repository_gate_uses_current_pd_repository(self):
        inputs = api.validate_inputs(ROOT / "scripts/native/macos-delivery.inputs.json")
        options = SimpleNamespace(lock_sha256=inputs["frozenInputs"]["dependencyLockSha256"], source_manifest_sha256="",
                                  expected_executable_sha256="", source_ref="a" * 40, arch="arm64", work_root=self.root)
        for repository in (None, "Nickel-Studio/Pd-Launcher", "Nickel-Studio/Ni-Launcher", "untrusted/Pd-Launcher", ""):
            with self.subTest(repository=repository), mock.patch.dict(os.environ, {}, clear=True):
                if repository is not None:
                    os.environ["GITHUB_REPOSITORY"] = repository
                with mock.patch.object(api, "apple_environment", side_effect=api.DeliveryError("Synthetic native probe reached, not Apple evidence")) as native:
                    if repository in (None, "Nickel-Studio/Pd-Launcher"):
                        with self.assertRaisesRegex(api.DeliveryError, "Synthetic native probe reached"):
                            ci.preflight(options, inputs, self.root)
                        native.assert_called_once()
                    else:
                        with self.assertRaisesRegex(api.DeliveryError, "authorized Palladium repository"):
                            ci.preflight(options, inputs, self.root)
                        native.assert_not_called()

    def test_inventory_and_stage_verify_actual_bytes(self):
        paths = self.complete_synthetic_source()
        self.write("tests/native-portability/scratch/run-old/CMakeCache.txt")
        manifest = ci.inventory(self.root, paths + ["tests/native-portability/scratch/run-old/CMakeCache.txt"], "a" * 40)
        self.assertEqual(len(manifest["files"]), len(paths))
        self.assertFalse(manifest["productDelivered"])
        path = self.root / "manifest.json"
        api.write_json(path, manifest)
        ci.stage(self.root, path, api.digest(path), self.root / "staged")
        self.assertEqual((self.root / "staged/native/app/main.cpp").read_bytes(), (self.root / "native/app/main.cpp").read_bytes())
        with self.assertRaises(api.DeliveryError):
            ci.stage(self.root, path, api.digest(path), self.root / "staged")
        self.write("native/app/main.cpp", b"changed after inventory")
        with self.assertRaises(api.DeliveryError):
            ci.stage(self.root, path, api.digest(path), self.root / "changed-stage")

    def test_qrc_closure_stages_only_actual_tracked_oracle_artwork(self):
        paths = self.complete_synthetic_source()
        qrc = "native/assets/brand/functional-resources.qrc"
        artwork = "src/assets/blocks/Anvil.png"
        self.write(qrc, b'<RCC><qresource><file alias="icon">../../../src/assets/blocks/Anvil.png</file></qresource></RCC>')
        self.write(artwork, b"synthetic artwork, not product visual evidence")
        self.write("src/assets/unreferenced.png")
        self.write("src/App.tsx")
        tracked = paths + [qrc, artwork, "src/assets/unreferenced.png", "src/App.tsx"]
        manifest = ci.inventory(self.root, tracked, "a" * 40)
        self.assertEqual(manifest["qrcReferencedOracleAssets"], [artwork])
        selected = {item["path"] for item in manifest["files"]}
        self.assertIn(artwork, selected)
        self.assertNotIn("src/assets/unreferenced.png", selected)
        self.assertNotIn("src/App.tsx", selected)
        target = self.root / "qrc-manifest.json"
        api.write_json(target, manifest)
        ci.stage(self.root, target, api.digest(target), self.root / "qrc-stage")
        self.assertEqual((self.root / "qrc-stage" / artwork).read_bytes(), (self.root / artwork).read_bytes())

    def test_brand_oracle_contract_files_stage_with_the_brand_tests(self):
        paths = self.complete_synthetic_source()
        brand = "native/tests/brand/CMakeLists.txt"
        self.write(brand, b"cmake_minimum_required(VERSION 3.24)\n")
        for relative in api.BRAND_ORACLE_REQUIRED:
            self.write(relative, b"synthetic oracle contract bytes")
        tracked = paths + [brand, *api.BRAND_ORACLE_REQUIRED]
        manifest = ci.inventory(self.root, tracked, "a" * 40)
        self.assertEqual(manifest["brandOracleFilesStaged"], sorted(api.BRAND_ORACLE_REQUIRED))
        selected = {item["path"] for item in manifest["files"]}
        self.assertTrue(set(api.BRAND_ORACLE_REQUIRED).issubset(selected))
        target = self.root / "oracle-manifest.json"
        api.write_json(target, manifest)
        ci.stage(self.root, target, api.digest(target), self.root / "oracle-stage")
        for relative in api.BRAND_ORACLE_REQUIRED:
            self.assertEqual((self.root / "oracle-stage" / relative).read_bytes(), b"synthetic oracle contract bytes")
        oracle = api.asset_oracle_root(self.root / "oracle-stage", manifest_paths=set(api.BRAND_ORACLE_REQUIRED))
        self.assertEqual(oracle, (self.root / "oracle-stage").resolve())
        with self.assertRaisesRegex(api.DeliveryError, "not tracked in the selected source commit"):
            ci.inventory(self.root, [path for path in tracked if path != "tests/brand/functional-icons.sha256"], "a" * 40)

    def test_qrc_rejects_untracked_secret_legacy_or_escaping_references(self):
        paths = self.complete_synthetic_source()
        qrc = "native/assets/brand/functional-resources.qrc"
        self.write("src/assets/untracked.png")
        invalid = [b"<RCC>", b'<!DOCTYPE RCC [<!ENTITY x "bad">]><RCC/>',
                   *[('<RCC><qresource><file>' + name + '</file></qresource></RCC>').encode() for name in
                     ["../../../../outside.png", "../../../.snow/secret.json", "../../../src/App.tsx",
                      "../../../src/assets/untracked.png", "../../../native/user/accounts.json", "https://example.com/a.png", "C:\\a.png", "/a.png"]]]
        for document in invalid:
            with self.subTest(document=document):
                self.write(qrc, document)
                with self.assertRaises(api.DeliveryError):
                    ci.inventory(self.root, paths + [qrc], "a" * 40)


    def relocated_git_fixture(self):
        fixture = ROOT / "tests/native-macos/inputs/qrc-relocation-647b6c"
        receipt = api.small_json(fixture / "receipt.json")
        archive = fixture / "source.zip"
        self.assertEqual(receipt["archive"], {"path": "source.zip", "bytes": 200507,
                                             "sha256": "16525e95cd2beb394795d6dcb17d4b28bf86e81e320ab1cfe172d2c024d76b0a"})
        self.assertEqual(archive.stat().st_size, receipt["archive"]["bytes"])
        self.assertEqual(api.digest(archive), receipt["archive"]["sha256"])
        self.assertEqual(receipt["executionSourceCommit"], "474e5a67d326e5f9e326e57185120396ba4ef59a")
        self.assertEqual(receipt["sharedGitTree"], "f433dabc01c02171fdda2e8c5482e890407a5ae0")
        self.assertEqual(receipt["historicalQrcReferences"], 33)
        paths = self.complete_synthetic_source()
        with zipfile.ZipFile(archive) as bundle:
            self.assertEqual(bundle.namelist(), [item["path"] for item in receipt["files"]])
            for item in receipt["files"]:
                relative = str(api.safe_relative(item["path"]))
                data = bundle.read(relative)
                self.assertEqual(len(data), item["bytes"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
                self.write(relative, data)
                paths.append(relative)
        environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        subprocess.run(["git", "init", "--quiet", str(self.root)], env=environment, check=True, capture_output=True)
        subprocess.run(["git", "-C", str(self.root), "add", "--", *paths], env=environment, check=True, capture_output=True)
        result = subprocess.run(["git", "-C", str(self.root), "ls-files", "-z"], env=environment, check=True, capture_output=True)
        tracked = result.stdout.decode("utf-8").rstrip("\0").split("\0")
        self.assertEqual(set(tracked), set(paths))
        return tracked, receipt

    def test_historical_qrc_actual_git_relocation_is_verified_and_staged(self):
        tracked, receipt = self.relocated_git_fixture()
        manifest = ci.inventory(self.root, tracked, "a" * 40)
        selected = {item["path"] for item in manifest["files"]}
        self.assertTrue({item["path"] for item in receipt["files"]}.issubset(selected))
        self.assertIn("native/assets/brand/CC-BY-SA-4.0.txt", selected)
        self.assertNotIn("native/assets/brand/history/nickel/CC-BY-SA-4.0.txt", selected)
        self.assertFalse(manifest["appleBuildExecuted"])
        self.assertFalse(manifest["productDelivered"])
        target = self.root / "relocation-manifest.json"
        api.write_json(target, manifest)
        ci.stage(self.root, target, api.digest(target), self.root / "relocation-stage")
        for item in receipt["files"]:
            self.assertEqual(api.digest(self.root / "relocation-stage" / item["path"]), item["sha256"])

    def test_active_qrc_cannot_fall_back_to_historical_resources(self):
        tracked, _ = self.relocated_git_fixture()
        qrc = "native/assets/brand/brand.qrc"
        self.write(qrc, b"<RCC><qresource><file>NOTICE.txt</file></qresource></RCC>")
        with self.assertRaisesRegex(api.DeliveryError, "native/assets/brand/NOTICE.txt"):
            ci.inventory(self.root, tracked + [qrc], "a" * 40)

    def test_historical_qrc_missing_or_untracked_real_license_fails(self):
        tracked, _ = self.relocated_git_fixture()
        license_path = "native/assets/brand/CC-BY-SA-4.0.txt"
        with self.assertRaisesRegex(api.DeliveryError, "not tracked canonical material"):
            ci.inventory(self.root, [path for path in tracked if path != license_path], "a" * 40)
        (self.root / license_path).unlink()
        with self.assertRaisesRegex(api.DeliveryError, "Missing/unsafe actual input file"):
            ci.inventory(self.root, tracked, "a" * 40)

    def test_historical_qrc_actual_resource_qrc_and_manifest_tampering_fails(self):
        tracked, receipt = self.relocated_git_fixture()
        for relative in ["native/assets/brand/CC-BY-SA-4.0.txt", receipt["historicalQrcPath"],
                         "native/assets/brand/source/nickel-asset-manifest.json"]:
            original = (self.root / relative).read_bytes()
            for changed in (original + b"x", original[:-1] + bytes([original[-1] ^ 1])):
                with self.subTest(relative=relative, changedBytes=len(changed)):
                    self.write(relative, changed)
                    with self.assertRaisesRegex(api.DeliveryError, "bytes differ from the declared original"):
                        ci.inventory(self.root, tracked, "a" * 40)
                    self.write(relative, original)

    def test_historical_qrc_relocations_reject_missing_changed_duplicate_or_unsafe_records(self):
        tracked, _ = self.relocated_git_fixture()
        path = self.root / "native/assets/brand/asset-manifest.json"
        original = path.read_bytes()
        mutations = [lambda value: value["historicalFiles"].pop(0),
                     lambda value: value["historicalFiles"][0].update(sha256="0" * 64),
                     lambda value: value["historicalFiles"].append(dict(value["historicalFiles"][0])),
                     lambda value: value["historicalManifest"].update(path="source/missing.json"),
                     *[lambda value, destination=destination: value["historicalFiles"][0].update(path=destination)
                       for destination in ("../outside.txt", "/outside.txt", "history/.snow/secret.json", "history/accounts.json")]]
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index):
                value = json.loads(original)
                mutate(value)
                api.write_json(path, value)
                with self.assertRaises(api.DeliveryError):
                    ci.inventory(self.root, tracked, "a" * 40)
                path.write_bytes(original)

    def write_bound_historical_qrc(self, document):
        qrc = self.write("native/assets/brand/history/nickel/brand.qrc", document)
        manifest_path = self.root / "native/assets/brand/asset-manifest.json"
        old_path = self.root / "native/assets/brand/source/nickel-asset-manifest.json"
        manifest, old = api.small_json(manifest_path), api.small_json(old_path)
        for item in manifest["historicalFiles"]:
            if item["originalPath"] == "brand.qrc":
                item.update(bytes=len(document), sha256=api.digest(qrc))
        for item in old["assets"]:
            if item["path"] == "brand.qrc":
                item.update(bytes=len(document), sha256=api.digest(qrc))
        api.write_json(old_path, old)
        manifest["historicalManifest"].update(bytes=old_path.stat().st_size, sha256=api.digest(old_path))
        api.write_json(manifest_path, manifest)

    def test_historical_qrc_xml_and_unbound_references_still_fail(self):
        tracked, _ = self.relocated_git_fixture()
        documents = [b'<!DOCTYPE RCC [<!ENTITY x "bad">]><RCC/>', b"<RCC>", b"<OTHER/>",
                     *[('<RCC><qresource><file>' + reference + '</file></qresource></RCC>').encode()
                       for reference in ("../CC-BY-SA-4.0.txt", "missing.png", "/icon.png", "C:\\icon.png",
                                         "https://example.com/icon.png", "")]]
        for document in documents:
            with self.subTest(document=document):
                self.write_bound_historical_qrc(document)
                with self.assertRaises(api.DeliveryError):
                    ci.inventory(self.root, tracked, "a" * 40)

    @unittest.skipIf(os.name == "nt", "Unprivileged Windows fixture cannot create symlinks")
    def test_historical_qrc_linked_resource_and_manifest_fail(self):
        tracked, _ = self.relocated_git_fixture()
        for relative in ("native/assets/brand/CC-BY-SA-4.0.txt", "native/assets/brand/source/nickel-asset-manifest.json"):
            with self.subTest(relative=relative):
                path = self.root / relative
                data = path.read_bytes()
                target = self.write("linked-original.txt", data)
                path.unlink()
                path.symlink_to(target)
                with self.assertRaisesRegex(api.DeliveryError, "Linked/reparse input rejected"):
                    ci.inventory(self.root, tracked, "a" * 40)
                path.unlink()
                self.write(relative, data)


    def test_no_missing_source_commit_or_duplicate_inventory(self):
        with self.assertRaises(api.DeliveryError):
            ci.inventory(self.root, [], "a" * 40)
        paths = self.complete_synthetic_source()
        for commit in ("testing-branch", "a" * 39, "A" * 40, "main; echo bad"):
            with self.assertRaises(api.DeliveryError):
                ci.inventory(self.root, paths, commit)
        with self.assertRaises(api.DeliveryError):
            ci.inventory(self.root, paths + paths[:1], "a" * 40)

    def test_frozen_files_map_checks_snapshot_and_byte_count(self):
        source = self.write("native/example.cpp", b"synthetic")
        files = {"native/example.cpp": {"owner": "unit-test", "sha256": api.digest(source), "bytes": source.stat().st_size}}
        value = {"schemaVersion": 1, "files": files, "snapshotSha256": hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")).hexdigest()}
        manifest = self.root / "map.json"
        api.write_json(manifest, value)
        self.assertEqual(api.verify_file_manifest(self.root, manifest, api.digest(manifest), "Synthetic map")["filesVerified"], 1)
        files["native/example.cpp"]["bytes"] += 1
        value["snapshotSha256"] = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")).hexdigest()
        api.write_json(manifest, value)
        with self.assertRaises(api.DeliveryError):
            api.verify_file_manifest(self.root, manifest, api.digest(manifest), "Synthetic map")
        value["snapshotSha256"] = "0" * 64
        with self.assertRaises(api.DeliveryError):
            api.manifest_entries(value, "Synthetic map")

    @unittest.skipIf(os.name == "nt", "Unprivileged Windows fixture cannot create symlinks")
    def test_linked_parent_rejected_even_when_inside_root(self):
        self.write("real/file.cpp")
        (self.root / "linked").symlink_to(self.root / "real", target_is_directory=True)
        with self.assertRaises(api.DeliveryError):
            api.checked_file(self.root, "linked/file.cpp")

    def test_partial_artifact_collection_never_claims_product(self):
        evidence = self.root / "evidence"
        evidence.mkdir()
        api.write_json(evidence / "app-failure.json", {"schema": 1, "complete": False, "failure": "synthetic configure failure"})
        ci.collect(SimpleNamespace(work_root=self.root, source_ref="a" * 40, arch="arm64"), evidence)
        value = api.small_json(self.root / "artifacts/artifact-status.json")
        self.assertEqual(value["completePhases"], [])
        self.assertFalse(value["productDelivered"])
        self.assertFalse(value["fullSevenRouteSemanticReviewVerified"])

    def test_workflow_double_native_and_no_legacy_commands(self):
        text = (ROOT / ".github/workflows/native-macos.yml").read_text()
        self.assertIn("workflow_dispatch:", text)
        self.assertIn("runner: macos-15\n", text)
        self.assertIn("runner: macos-15-intel\n", text)
        self.assertIn("max-parallel: 2", text)
        self.assertIn("if: github.repository == 'Nickel-Studio/Pd-Launcher'", text)
        self.assertNotIn("Nickel-Studio/Ni-Launcher", text)
        self.assertNotIn("pull_request:", text)
        self.assertNotIn("secrets.", text)
        self.assertNotIn("npm ", text)
        self.assertNotIn("cargo ", text)
        self.assertNotIn("actions/cache", text)
        self.assertIn("persist-credentials: false", text)
        self.assertIn("retention-days: 7", text)
        self.assertIn("--expected-executable-sha256", text)
        self.assertIn("--source-manifest-sha256", text)
        self.assertIn("PHASE: materials", text)
        for suite in ("test_delivery.py", "test_ci.py", "test_tools.py", "test_install_delivery.py"):
            with self.subTest(suite=suite):
                self.assertIn("python3 -B tests/native-macos/" + suite, text)
        self.assertIn("sh -n scripts/native/macos-delivery.sh", text)

    def test_workflow_matrix_fails_closed_per_architecture_without_canceling_the_other_leg(self):
        text = (ROOT / ".github/workflows/native-macos.yml").read_text()
        strategy = text.split("    strategy:\n", 1)[1].split("    runs-on:", 1)[0]
        self.assertIn("fail-fast: false", strategy)
        self.assertIn("max-parallel: 2", strategy)
        matrix = strategy.split("matrix:\n", 1)[1]
        for entry in ("arch: arm64", "runner: macos-15\n", "arch: x86_64", "runner: macos-15-intel\n"):
            with self.subTest(entry=entry):
                self.assertIn(entry, matrix)
        self.assertNotIn("fail-fast: true", text)

    def test_workflow_is_manual_dispatch_only(self):
        text = (ROOT / ".github/workflows/native-macos.yml").read_text()
        triggers = text.split("\non:\n", 1)[1].split("\npermissions:\n", 1)[0]
        events = [line.strip() for line in triggers.splitlines() if line.startswith("  ") and not line.startswith("   ") and line.endswith(":")]
        self.assertEqual(events, ["workflow_dispatch:"])
        self.assertIn("dependency_lock_sha256:", triggers)
        self.assertIn("required: true", triggers)

    def test_workflow_resolved_commit_fallback_and_always_evidence(self):
        text = (ROOT / ".github/workflows/native-macos.yml").read_text()
        lock = api.validate_inputs(ROOT / "scripts/native/macos-delivery.inputs.json")["frozenInputs"]["dependencyLockSha256"]
        self.assertIn("SOURCE_REF: ${{ inputs.source_ref || github.sha }}", text)
        self.assertIn("LOCK_SHA256: ${{ inputs.dependency_lock_sha256 || '" + lock + "' }}", text)
        self.assertIn("ref: ${{ env.SOURCE_REF }}", text)
        self.assertIn("group: palladium-macos-${{ github.workflow }}-${{ github.ref }}", text)
        self.assertIn("cancel-in-progress: true", text)
        self.assertIn("name: palladium-macos-${{ matrix.arch }}-${{ env.SOURCE_REF }}-${{ github.run_attempt }}", text)
        for phase in ("materials", "collect"):
            steps = [block for block in text.split("      - name: ") if "\n          PHASE: " + phase + "\n" in block]
            self.assertEqual(len(steps), 1)
            self.assertIn("\n        if: always()\n", steps[0])
        upload = text.split("uses: actions/upload-artifact@", 1)[0].rsplit("      - name: ", 1)[1]
        self.assertIn("\n        if: always()\n", upload)

    def test_runner_temp_is_resolved_in_a_step_not_job_environment(self):
        text = (ROOT / ".github/workflows/native-macos.yml").read_text()
        job_environment = text.split("    env:\n", 1)[1].split("    steps:\n", 1)[0]
        self.assertNotIn("${{ runner.", job_environment)
        self.assertNotIn("NATIVE_WORK:", job_environment)
        first_step = text.split("      - name: ")[1]
        self.assertIn("printf 'NATIVE_WORK=%s/nickel-native-macos-%s-%s-%s\\n'", first_step)
        self.assertIn('"$RUNNER_TEMP" "$NATIVE_ARCH" "$GITHUB_RUN_ID" "$GITHUB_RUN_ATTEMPT" >> "$GITHUB_ENV"', first_step)
        self.assertIn("path: ${{ env.NATIVE_WORK }}/artifacts", text)

    def test_every_phase_has_an_explicit_complete_driver_without_yaml_aliases(self):
        text = (ROOT / ".github/workflows/native-macos.yml").read_text()
        self.assertNotIn("run: &", text)
        self.assertNotIn("run: *", text)
        for phase in ci.PHASES:
            steps = [block for block in text.split("      - name: ") if "\n          PHASE: " + phase + "\n" in block]
            self.assertEqual(len(steps), 1)
            self.assertIn("\n        run: |\n", steps[0])
            self.assertIn('python3 -B scripts/native/macos-delivery.ci.py --phase "$PHASE"', steps[0])
            self.assertIn('--expected-executable-sha256 "$EXPECTED_EXE_SHA256" --work-root "$NATIVE_WORK" --jobs 2', steps[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
