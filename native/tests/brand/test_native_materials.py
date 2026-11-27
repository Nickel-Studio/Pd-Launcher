# SPDX-License-Identifier: AGPL-3.0-only
"""Small synthetic material contracts, not real compilation or release evidence."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("nickel_native_material_inspection", ROOT / "scripts/native/brand-native-materials.py")
INSPECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSPECT)
LEGAL = INSPECT.LEGAL


class NativeMaterialContracts(unittest.TestCase):
    def setUp(self):
        parent = ROOT / ".snow/legal-unit"
        parent.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(parent).free < 16 * 1024 * 1024:
            self.fail("Insufficient workspace disk space before creating small test fixtures")
        self.temporary = tempfile.TemporaryDirectory(prefix="native-material-contract-", dir=parent)
        self.root = Path(self.temporary.name)
        self.portal = self.root / ".snow/native-tools"
        self.portal.mkdir(parents=True)
        self.license = self.put("LICENSE.txt", (ROOT / "LICENSE").read_bytes(), "license")
        archive = self.root / "source.zip"
        with zipfile.ZipFile(archive, "w") as source:
            source.writestr("src/contract.cpp", "// Synthetic contract input, not application source.\nint contract() { return 0; }\n")
        self.source = self.put("source.zip", archive.read_bytes(), "source-archive")
        self.component = {"id": "contract-library", "name": "Synthetic material contract library", "version": "fixture-1",
                          "category": "other", "linkage": "static", "licenseConcluded": "AGPL-3.0-only",
                          "copyrightText": "Synthetic fixture only, no compiled release claim", "origin": "NOASSERTION",
                          "artifacts": [self.put("library.lib", self.library(self.object()), "static-library")],
                          "licenseFiles": [self.license], "noticePolicy": "copyright-in-license",
                          "versionEvidence": [self.put("version.txt", b"fixture-1\n", "version-evidence")],
                          "correspondingSource": [self.source],
                          "buildMaterials": [self.put("build.txt", b"Synthetic contract input; no actual build.\n", "build-configuration")]}
        self.document = {"schemaVersion": 1, "scope": "native-dependency-snapshot", "releaseId": "material-fixture-1",
                         "components": [self.component]}

    def tearDown(self):
        self.temporary.cleanup()

    def put(self, relative: str, data: bytes, kind: str = "file") -> dict:
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return {"path": relative, "sha256": hashlib.sha256(data).hexdigest(), "kind": kind}

    @staticmethod
    def object() -> bytes:
        return struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0, 0) + b"synthetic-contract-object"

    @staticmethod
    def library(member: bytes) -> bytes:
        header = b"fixture.obj/".ljust(16) + b"0".ljust(12) + b"0".ljust(6) + b"0".ljust(6) + b"0".ljust(8) + str(len(member)).encode("ascii").ljust(10) + b"`\n"
        return b"!<arch>\n" + header + member + (b"\n" if len(member) % 2 else b"")

    def collect(self) -> tuple[dict, Path]:
        input_path = self.root / "input.json"
        input_path.write_bytes((json.dumps(self.document) + "\n").encode("utf-8"))
        output = self.root / "bundle"
        return LEGAL.collect(input_path, output, self.root), output

    def test_dependency_scope_contract_never_authorizes_native_release(self):
        result, output = self.collect()
        self.assertTrue(result["dependencySnapshotValidationPassed"])
        self.assertFalse(result["nativeLegalMaterialValidationPassed"])
        self.assertFalse(result["nativeReleaseReady"])
        verified = LEGAL.verify_bundle(output)
        self.assertEqual(verified["errors"], [])
        self.assertTrue(verified["dependencySnapshotValidationPassed"])
        self.assertFalse(verified["nativeReleaseReady"])

    def test_snapshot_cannot_include_application_asset_or_system_exceptions(self):
        for category, linkage in [("application", "static"), ("artwork", "embedded"), ("font", "embedded"), ("other", "system"), ("other", "build-only")]:
            with self.subTest(category=category, linkage=linkage):
                document = copy.deepcopy(self.document)
                document["components"][0].update(category=category, linkage=linkage)
                self.assertTrue(LEGAL.validate_input(document, self.root))

    def test_snapshot_requires_source_archive_and_build_materials(self):
        self.component["correspondingSource"] = []
        self.assertIn("source", " ".join(LEGAL.validate_input(self.document, self.root)))
        self.component["correspondingSource"] = [self.source]
        self.component["buildMaterials"] = []
        self.assertIn("build", " ".join(LEGAL.validate_input(self.document, self.root)))

    def test_asset_scope_and_inspection_kind_cannot_bypass_native_build_gate(self):
        self.document["scope"] = "native-release"
        self.assertIn("build", " ".join(LEGAL.validate_input(self.document, self.root)))
        self.assertTrue(LEGAL.validate_input({"kind": "native-dependency-inspection", "scope": "native-dependency-snapshot"}, self.root))

    def test_snapshot_shared_source_and_license_have_one_physical_copy(self):
        second = copy.deepcopy(self.component)
        second["id"] = "other-library"
        second["correspondingSource"] = [self.put("renamed-source.zip", (self.root / self.source["path"]).read_bytes(), "source-archive")]
        self.document["components"].append(second)
        result, output = self.collect()
        rows = result["materials"]
        self.assertGreater(len(rows), len({row["path"] for row in rows}))
        sources = [row for row in rows if row["role"] == "correspondingSource"]
        self.assertEqual(sources[0]["path"], sources[1]["path"])
        self.assertEqual(LEGAL.verify_bundle(output)["errors"], [])

    def test_insufficient_capacity_fails_before_any_bundle_write(self):
        with patch.object(LEGAL.shutil, "disk_usage", return_value=shutil._ntuple_diskusage(1024, 1023, 1)):
            with self.assertRaisesRegex(LEGAL.MaterialError, "Insufficient disk space"):
                self.collect()
        self.assertFalse((self.root / "bundle").exists())
        self.assertEqual((self.root / "LICENSE.txt").read_bytes(), (ROOT / "LICENSE").read_bytes())

    def test_matching_license_ref_is_copied_and_independently_valid_spdx(self):
        text = (ROOT / "native/assets/brand/NOTICE.txt").read_bytes()
        reference = dict(self.put("custom-license.txt", text, "license-text"), licenseId="LicenseRef-Actual-Fixture")
        self.component["licenseConcluded"] = "AGPL-3.0-only AND LicenseRef-Actual-Fixture"
        self.component["licenseRefTexts"] = [reference]
        result, output = self.collect()
        self.assertTrue(any(row["role"] == "licenseRefTexts" for row in result["materials"]))
        spdx = json.loads((output / "dependencies.spdx.json").read_bytes())
        self.assertEqual(spdx["hasExtractedLicensingInfos"], [{"licenseId": reference["licenseId"], "extractedText": text.decode("utf-8")}])
        self.assertEqual(LEGAL.validate_spdx(output / "dependencies.spdx.json"), [])
        self.assertEqual(LEGAL.verify_bundle(output)["errors"], [])

    def test_declared_license_ref_also_requires_matching_extracted_text(self):
        self.component["licenseDeclared"] = "AGPL-3.0-only OR LicenseRef-Missing"
        self.assertIn("matching extracted text", " ".join(LEGAL.validate_input(self.document, self.root)))

    def test_conflicting_license_ref_text_is_rejected_before_copy(self):
        one = dict(self.put("one.txt", b"Synthetic license text one, not shipped.\n" * 8), licenseId="LicenseRef-Fixture")
        two = dict(self.put("two.txt", b"Synthetic license text two, not shipped.\n" * 8), licenseId="LicenseRef-Fixture")
        self.component["licenseConcluded"] += " AND LicenseRef-Fixture"
        self.component["licenseRefTexts"] = [one, two]
        self.assertIn("Conflicting extracted text", " ".join(LEGAL.validate_input(self.document, self.root)))
        with self.assertRaises(LEGAL.MaterialError):
            self.collect()
        self.assertFalse((self.root / "bundle").exists())

    def test_wrong_extracted_id_short_text_and_template_are_rejected(self):
        self.component["licenseConcluded"] += " AND LicenseRef-Fixture"
        for identifier, text in [("MIT", b"invalid id\n" * 20), ("LicenseRef-Fixture", b"short"), ("LicenseRef-Fixture", b"TEMPLATE ONLY\n" * 20)]:
            with self.subTest(identifier=identifier):
                self.component["licenseRefTexts"] = [dict(self.put("ref.txt", text), licenseId=identifier)]
                self.assertTrue(LEGAL.validate_input(self.document, self.root))

    def test_actual_coff_implementation_differs_from_short_import_library(self):
        path = self.root / "library.lib"
        self.assertEqual(INSPECT.coff_archive(path)["kind"], "static-library")
        self.assertEqual(LEGAL.binary_kind(path), "static-library")
        short_import = struct.pack("<HHHHIIHH", 0, 0xFFFF, 0, 0x8664, 0, 4, 0, 0) + b"sym\0"
        data = self.library(short_import)
        path.write_bytes(data)
        result = INSPECT.coff_archive(path)
        self.assertEqual(result["kind"], "import-library")
        self.assertEqual(result["shortImportObjects"], 1)
        self.assertEqual(LEGAL.binary_kind(path), "import-library")
        self.component["artifacts"] = [self.put("library.lib", data, "static-library")]
        self.assertIn("not a real archive", " ".join(LEGAL.validate_input(self.document, self.root)))

    def test_fake_or_truncated_library_does_not_pass_coff_parser(self):
        path = self.root / "fake.lib"
        for data in [b"library goes here\n", b"!<arch>\n" + b"x" * 60, self.library(self.object())[:-5],
                     self.library(b"unrecognized bytes") + self.library(self.object())[8:]]:
            with self.subTest(length=len(data)):
                path.write_bytes(data)
                with self.assertRaises(INSPECT.InspectionError):
                    INSPECT.coff_archive(path)
                self.assertEqual(LEGAL.binary_kind(path), "other")

    def test_msvc_bigobj_header_is_an_object_not_an_import_library(self):
        header = bytearray(96)
        header[:8] = b"\0\0\xff\xff\x02\0\x64\x86"
        header[12:28] = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")
        struct.pack_into("<I", header, 44, 1)
        path = self.root / "synthetic-bigobj.obj"
        path.write_bytes(header)
        self.assertEqual(LEGAL.binary_kind(path), "object")
        archive = self.root / "synthetic-bigobj.lib"
        archive.write_bytes(self.library(header))
        self.assertEqual(INSPECT.coff_archive(archive)["kind"], "static-library")
        header[12:28] = bytes(16)
        path.write_bytes(header)
        self.assertEqual(LEGAL.binary_kind(path), "other")
        archive.write_bytes(self.library(header))
        self.assertEqual(LEGAL.binary_kind(archive), "other")
        path.write_bytes(b"\0\0\xff\xff\0\0\x64\x86" + bytes(20))
        self.assertEqual(LEGAL.binary_kind(path), "other")

    def test_pe_dll_is_not_claimed_to_be_static_library(self):
        data = bytearray(128)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<H", data, 68, 0x8664)
        struct.pack_into("<H", data, 86, 0x2000)
        path = self.root / "synthetic.dll"
        path.write_bytes(data)
        self.assertEqual(INSPECT.pe_artifact(path)["kind"], "shared-library")
        self.assertEqual(LEGAL.binary_kind(path), "shared-library")
        path.write_bytes(data[:68])
        self.assertEqual(LEGAL.binary_kind(path), "other")

    def test_read_only_tools_reader_has_exact_portal_and_traversal_boundary(self):
        reader = INSPECT.ReadOnlyTools(self.root)
        self.put(".snow/native-tools/license.txt", b"actual bytes inspected read-only\n")
        self.assertTrue(reader.record("license.txt", "license")["available"])
        for value in ["../outside", "C:/outside", "/outside", "", None]:
            with self.subTest(value=value):
                with self.assertRaises(INSPECT.InspectionError):
                    reader.path(value)
        before = (self.portal / "license.txt").read_bytes()
        reader.record("license.txt", "license")
        self.assertEqual((self.portal / "license.txt").read_bytes(), before)

    def test_cached_read_only_record_rejects_changed_material(self):
        reader = INSPECT.ReadOnlyTools(self.root)
        path = self.portal / "cached.bin"
        path.write_bytes(b"first inspection bytes\n")
        reader.record("cached.bin", "file")
        path.write_bytes(b"modified material with a different length\n")
        with self.assertRaisesRegex(INSPECT.InspectionError, "changed while inspecting"):
            reader.record("cached.bin", "file")

    def test_install_manifest_never_opens_recorded_external_paths(self):
        self.assertEqual(INSPECT.install_paths("C:/trusted/prefix/lib/example.lib\n", ["C:/trusted/prefix"]), {"lib/example.lib"})
        for value in ["C:/other/lib/example.lib", "C:/trusted/prefix/../example.lib"]:
            with self.subTest(value=value):
                with self.assertRaises(INSPECT.InspectionError):
                    INSPECT.install_paths(value, ["C:/trusted/prefix"])

    def test_cache_options_preserve_actual_values_not_defaults(self):
        options = INSPECT.cache_options("BUILD_SHARED_LIBS:BOOL=OFF\nCMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded\nCURL_USE_SCHANNEL:BOOL=ON\n")
        self.assertEqual(options, {"BUILD_SHARED_LIBS": "OFF", "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded", "CURL_USE_SCHANNEL": "ON"})

    def test_entrypoint_target_binds_the_library_not_a_comment_or_other_target(self):
        text = ('set_target_properties(Qt6::EntryPointImplementation PROPERTIES\n'
                '  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/Qt6EntryPoint.lib"\n)\n')
        self.assertTrue(INSPECT.entrypoint_target_matches(text))
        for invalid in [text.replace("EntryPointImplementation", "OtherTarget"),
                        text.replace("Qt6EntryPoint.lib", "OtherLibrary.lib"),
                        text.replace("set_target_properties", "# set_target_properties"),
                        text.replace("  IMPORTED_LOCATION_RELEASE", "  # IMPORTED_LOCATION_RELEASE")]:
            with self.subTest(text=invalid):
                self.assertFalse(INSPECT.entrypoint_target_matches(invalid))

    def test_upstream_attribution_relaxes_only_literal_whitespace_controls(self):
        relative = "sources/fixture/qt_attribution.json"
        payload = b'{"Id":"fixture","LicenseId":"MIT","License":"first line\nsecond line"}\n'
        self.put(".snow/native-tools/" + relative, payload)
        with self.assertRaises(json.JSONDecodeError):
            json.loads(payload)
        entries, relaxed = INSPECT.read_attributions(INSPECT.ReadOnlyTools(self.root), "sources/fixture")
        self.assertEqual(relaxed, [relative])
        self.assertEqual(entries["fixture"]["data"]["License"], "first line\nsecond line")
        for invalid in [b'{"Id":"fixture",}', b'{"Id":"fixture","License":"bad\x01control"}',
                        b'{"Id":"fixture","License":"first\nthen\x01control"}', b'[]']:
            with self.subTest(payload=invalid):
                self.put(".snow/native-tools/" + relative, invalid)
                with self.assertRaises(INSPECT.InspectionError):
                    INSPECT.read_attributions(INSPECT.ReadOnlyTools(self.root), "sources/fixture")

    def test_observation_rebuild_rejects_omitted_artifacts_and_fabricated_counts(self):
        row = self.put(".snow/native-tools/first.bin", b"first synthetic material\n")
        second = self.put(".snow/native-tools/second.bin", b"second synthetic material\n")
        actual = {"kind": "native-dependency-inspection", "scope": "native-dependency-snapshot",
                  "observedAt": "2026-11-21T00:00:00Z", "installedLibraryCount": 2,
                  "lock": self.put("lock.json", b'{"sources": []}\n'),
                  "components": [{"id": "fixture", "artifacts": [row, second], "licenseConcluded": "MIT"}],
                  "materialBundleCollected": False, "dependencySnapshotValidationPassed": False,
                  "nativeLegalMaterialValidationPassed": False, "nativeReleaseReady": False}
        recorded = copy.deepcopy(actual)
        recorded["observedAt"] = "2026-11-21T01:00:00Z"
        with patch.object(INSPECT, "observation_schema_errors", side_effect=lambda *_: []), \
             patch.object(INSPECT, "inspect", side_effect=lambda *_: copy.deepcopy(actual)) as rebuild:
            self.assertEqual(INSPECT.verify_observation(self.root, recorded), [])
            for mutation in ["omitted-artifact", "fabricated-count", "license-drift"]:
                with self.subTest(mutation=mutation):
                    changed = copy.deepcopy(recorded)
                    if mutation == "omitted-artifact":
                        changed["components"][0]["artifacts"].pop()
                    elif mutation == "fabricated-count":
                        changed["installedLibraryCount"] = 999
                    else:
                        changed["components"][0]["licenseConcluded"] = "BSD-2-Clause"
                    self.assertIn("current actual materials", " ".join(INSPECT.verify_observation(self.root, changed)))
            self.assertEqual(rebuild.call_count, 4)

    def test_observation_hash_tamper_and_release_flags_fail_closed(self):
        row = self.put(".snow/native-tools/source.bin", b"synthetic read-only material\n")
        document = {"kind": "native-dependency-inspection", "scope": "native-dependency-snapshot", "components": [{"artifact": row}],
                    "lock": self.put("lock.json", b'{"sources": []}\n'),
                    "materialBundleCollected": False, "dependencySnapshotValidationPassed": False,
                    "nativeLegalMaterialValidationPassed": False, "nativeReleaseReady": False}
        # Isolate verification contracts; this synthetic inventory is not production-schema evidence.
        with patch.object(INSPECT, "observation_schema_errors", side_effect=lambda *_: []), \
             patch.object(INSPECT, "inspect", side_effect=lambda *_: copy.deepcopy(document)) as rebuild:
            self.assertEqual(INSPECT.verify_observation(self.root, document), [])
            document["nativeReleaseReady"] = True
            self.assertTrue(INSPECT.verify_observation(self.root, document))
            document["nativeReleaseReady"] = False
            (self.root / row["path"]).write_bytes(b"tampered\n")
            self.assertTrue(INSPECT.verify_observation(self.root, document))
            self.assertEqual(rebuild.call_count, 1)

    def test_format_only_inventory_is_not_read_only_evidence(self):
        for document in [None, [], "inventory", {"kind": "native-dependency-inspection", "scope": "native-dependency-snapshot", "components": []}]:
            with self.subTest(document=document):
                self.assertTrue(INSPECT.observation_schema_errors(document))
                self.assertTrue(INSPECT.verify_observation(self.root, document))


if __name__ == "__main__":
    unittest.main(verbosity=2)
