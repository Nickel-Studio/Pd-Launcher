# SPDX-License-Identifier: AGPL-3.0-only
"""Synthetic binary-shaped negative fixtures; never native build/release evidence."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("nickel_relink_legal", ROOT / "scripts/native/brand-legal.py")
LEGAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LEGAL)


class NativeRelinkContractTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / ".snow/legal-unit"
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="relink-negative-", dir=parent)
        self.root = Path(self.temporary.name)
        self.original = self.put("original.exe", self.pe(b"synthetic-original"), "executable")
        self.relinked = self.put("replacement.exe", self.pe(b"synthetic-relinked"), "executable")
        self.archive = self.put("lib/original.lib", self.object_archive(b"synthetic-original"), "static-library")
        self.replacement = self.put("lib/modified.lib", self.object_archive(b"synthetic-modified"), "static-library")
        self.replacement.update({"componentId": "qtbase", "commandPath": "C:/fixture/lib/modified.lib"})
        self.libraries = [{"id": "qtbase", "artifacts": [self.archive]}]
        self.log = {"argv": ["link.exe", "/OUT:C:/fixture/replacement.exe", "C:/fixture/lib/modified.lib"],
                    "exitCode": 0, "outputSha256": self.relinked["sha256"]}
        self.relink = {
            "applicationCode": [self.source_archive("application.zip")],
            "linkCommands": [self.put("link-command.txt", b"Synthetic contract fixture only.\n", "link-command")],
            "buildInstructions": [self.put("build.txt", b"Synthetic contract fixture only.\n", "build-instructions")],
            "installationInformation": [self.put("install.txt", b"Synthetic contract fixture only.\n", "installation-information")],
            "resources": [self.put("resources.qrc", b'<RCC><qresource prefix="/fixture" /></RCC>\n', "resource-source")],
            "probe": {"relinkedExecutable": self.relinked, "log": self.put_json("probe.json", self.log),
                      "replacementLibraries": [self.replacement]}}

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def pe(marker: bytes) -> bytes:
        data = bytearray(192)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 128)
        data[128:132] = b"PE\0\0"
        return bytes(data) + marker

    @staticmethod
    def object_archive(marker: bytes) -> bytes:
        member = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0, 0) + marker
        header = b"fixture.obj/".ljust(16) + b"0".ljust(12) + b"0".ljust(6) + b"0".ljust(6) + b"0".ljust(8) + str(len(member)).encode("ascii").ljust(10) + b"`\n"
        return b"!<arch>\n" + header + member + (b"\n" if len(member) % 2 else b"")

    def put(self, path: str, data: bytes, kind: str = "file") -> dict:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return {"path": path, "sha256": hashlib.sha256(data).hexdigest(), "kind": kind}

    def put_json(self, path: str, value: dict) -> dict:
        return self.put(path, (json.dumps(value, indent=2) + "\n").encode("utf-8"), "probe-log")

    def source_archive(self, name: str) -> dict:
        target = self.root / name
        with zipfile.ZipFile(target, "w") as archive:
            archive.writestr("src/main.cpp", "// Synthetic test input, not corresponding release source.\nint main() { return 0; }\n")
            archive.writestr("CMakeLists.txt", "# Synthetic test input only.\n")
        return self.put(name, target.read_bytes(), "source-archive")

    def validate(self):
        return LEGAL.validate_relink(self.relink, self.root, self.libraries, self.root / self.original["path"])

    def rewrite_log(self):
        self.relink["probe"]["log"] = self.put_json("probe.json", self.log)

    def test_command_binding_contract_passes_without_claiming_real_execution(self):
        self.validate()

    def test_log_must_use_the_actual_replacement_library_not_merely_a_changed_output(self):
        self.log["argv"] = ["link.exe", "C:/fixture/lib/original.lib"]
        self.rewrite_log()
        with self.assertRaisesRegex(LEGAL.MaterialError, "replacement.*link|link.*replacement"):
            self.validate()

    def test_same_basename_in_other_directory_does_not_bind_replacement(self):
        self.log["argv"][-1] = "C:/different/lib/modified.lib"
        self.rewrite_log()
        with self.assertRaisesRegex(LEGAL.MaterialError, "replacement.*link|link.*replacement"):
            self.validate()

    def test_argv_must_be_a_nonempty_string_array(self):
        for argv in ["link.exe modified.lib", [], [None], [1], [""]]:
            with self.subTest(argv=argv):
                self.log["argv"] = argv
                self.rewrite_log()
                with self.assertRaises(LEGAL.MaterialError):
                    self.validate()

    def test_recorded_response_file_must_be_consumed_by_the_actual_command(self):
        response = self.put("link.rsp", b'"C:/fixture/lib/modified.lib"\n', "link-response")
        response["commandPath"] = "C:/fixture/link.rsp"
        self.relink["probe"]["responseFiles"] = [response]
        self.log["argv"] = ["link.exe", "@C:/fixture/unrelated.rsp"]
        self.rewrite_log()
        with self.assertRaisesRegex(LEGAL.MaterialError, "response"):
            self.validate()
        self.log["argv"] = ["link.exe", "@C:/fixture/link.rsp"]
        self.rewrite_log()
        self.validate()

    def test_response_file_hash_and_reference_remain_verifiable_after_bundle_relocation(self):
        response = self.put("link.rsp", b'"C:/fixture/lib/modified.lib"\n', "link-response")
        response["commandPath"] = "C:/fixture/link.rsp"
        self.relink["probe"]["responseFiles"] = [response]
        self.log["argv"] = ["link.exe", "@C:/fixture/link.rsp"]
        self.rewrite_log()
        document = {"components": [], "relink": self.relink}
        refs = list(LEGAL.all_refs(document))
        self.assertTrue(any(role == "responseFiles" for _, role, _ in refs))
        relocated = copy.deepcopy(self.relink)
        for _, _, row in LEGAL.all_refs({"components": [], "relink": relocated}):
            old = self.root / row["path"]
            new_path = "staged/" + row["sha256"][:12] + "-" + old.name
            target = self.root / new_path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(old.read_bytes())
            row["path"] = new_path
        LEGAL.validate_relink(relocated, self.root, self.libraries, self.root / self.original["path"])
        (self.root / response["path"]).write_bytes(b'"C:/fixture/lib/original.lib"\n')
        with self.assertRaisesRegex(LEGAL.MaterialError, "hash mismatch"):
            self.validate()

    def test_original_library_cannot_be_substituted_as_a_relink_probe(self):
        row = dict(self.archive, componentId="qtbase", commandPath="C:/fixture/lib/original.lib")
        self.relink["probe"]["replacementLibraries"] = [row]
        self.log["argv"][-1] = row["commandPath"]
        self.rewrite_log()
        with self.assertRaisesRegex(LEGAL.MaterialError, "Unmodified original"):
            self.validate()

    def test_probe_must_cover_every_static_lgpl_component(self):
        self.libraries.append({"id": "qtsvg", "artifacts": [self.archive]})
        with self.assertRaisesRegex(LEGAL.MaterialError, "every static LGPL"):
            self.validate()

    def test_source_archive_application_code_is_verified_not_just_a_kind_label(self):
        self.relink["applicationCode"] = [self.put("promise.zip", b"A source archive will be provided later.\n", "source-archive")]
        with self.assertRaisesRegex(LEGAL.MaterialError, "real ZIP/TAR"):
            self.validate()

    def test_changed_binary_alone_and_wrong_output_hash_do_not_pass(self):
        self.log["outputSha256"] = self.original["sha256"]
        self.rewrite_log()
        with self.assertRaisesRegex(LEGAL.MaterialError, "actual output SHA-256"):
            self.validate()

    def test_native_legal_pass_is_not_release_readiness(self):
        self.assertFalse(LEGAL.readiness("native-release", passed=True)["nativeReleaseReady"])
        self.assertTrue(LEGAL.readiness("native-release", passed=True)["nativeLegalMaterialValidationPassed"])
        self.assertFalse(LEGAL.readiness("native-release", passed=False)["nativeLegalMaterialValidationPassed"])
        self.assertFalse(LEGAL.readiness("asset-only", passed=True)["nativeLegalMaterialValidationPassed"])

    def native_document(self) -> dict:
        application = {"id": "nickel", "category": "application", "version": "native-negative-1",
                       "licenseConcluded": "AGPL-3.0-only", "linkage": "embedded",
                       "artifacts": [self.original], "correspondingSource": [self.source_archive("complete-application.zip")]}
        inventory = self.put_json("inventory.json", {"components": [{"id": "nickel", "version": "native-negative-1", "artifacts": [self.original]}]})
        inventory["kind"] = "dependency-inventory"
        return {"components": [application],
                "build": {"platform": "synthetic", "architecture": "x64", "compiler": "synthetic", "compilerVersion": "negative-1",
                          "executable": self.original,
                          "evidence": [inventory, self.put("native-link.txt", b"synthetic-only\n", "link-command"),
                                       self.put("native-map.txt", b"synthetic-only\n", "link-map"),
                                       self.put("native-build.txt", b"synthetic-only\n", "build-log")]}}

    def test_native_application_requires_full_source_archive_not_one_source_file(self):
        document = self.native_document()
        LEGAL.validate_release_build(document, self.root, {"nickel"})
        document["components"][0]["correspondingSource"] = [self.put("one-source.cpp", b"// negative-only\n", "source-file")]
        with self.assertRaisesRegex(LEGAL.MaterialError, "full corresponding application source archive"):
            LEGAL.validate_release_build(document, self.root, {"nickel"})

    def test_native_application_artifact_must_match_the_actual_build_executable(self):
        document = self.native_document()
        document["build"]["executable"] = self.relinked
        with self.assertRaisesRegex(LEGAL.MaterialError, "does not match the actual built executable"):
            LEGAL.validate_release_build(document, self.root, {"nickel"})

    def test_duplicate_linked_dependency_rows_are_rejected(self):
        document = self.native_document()
        entry = {"id": "nickel", "version": "native-negative-1", "artifacts": [self.original]}
        row = self.put_json("inventory.json", {"components": [entry, entry]})
        row["kind"] = "dependency-inventory"
        document["build"]["evidence"][0] = row
        with self.assertRaisesRegex(LEGAL.MaterialError, "Duplicate linked dependency"):
            LEGAL.validate_release_build(document, self.root, {"nickel"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
