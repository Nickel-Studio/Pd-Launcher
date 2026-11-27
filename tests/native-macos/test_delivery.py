#!/usr/bin/env python3
"""Host-independent parser and negative gates, never macOS product acceptance."""
from __future__ import annotations

import importlib.util
import inspect
import io
import json
import os
import re
from pathlib import Path
import stat
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile


sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("macos_delivery", ROOT / "scripts/native/macos-delivery.py")
delivery = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(delivery)


class DeliveryGates(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="macos-delivery-unit-", dir=Path(__file__).parent)
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def json_file(self, name, value):
        target = self.root / name
        target.write_text(json.dumps(value), encoding="utf-8")
        return target

    def inputs(self):
        return delivery.validate_inputs(ROOT / "scripts/native/macos-delivery.inputs.json")

    def contract(self, omit_imageformats=False):
        lock_path = ROOT / "tests/native-macos/inputs/frozen-20261121T150627Z/deps-lock.json"
        lock = delivery.small_json(lock_path)
        sources = [entry for entry in lock["sources"] if not (omit_imageformats and entry["name"] == "qtimageformats")]
        value = {"schema": 1, "lockSha256": delivery.digest(lock_path), "sources": sources}
        return self.json_file("synthetic-contract.json", value)

    def test_exact_qt_and_minimum_os(self):
        value = self.inputs()
        self.assertEqual(value["qtVersion"], "6.8.3")
        self.assertEqual(value["policy"]["deploymentTarget"], "12.0")
        self.assertEqual(value["policy"]["architectures"], ["arm64", "x86_64"])
        self.assertFalse(value["policy"]["qtBusinessNetwork"])
        value["qtVersion"] = "6.8.4"
        with self.assertRaises(delivery.DeliveryError):
            delivery.validate_inputs(self.json_file("wrong-version.json", value))

    def test_contract_hash_and_source_tuples(self):
        contract = self.contract()
        lock = ROOT / "tests/native-macos/inputs/frozen-20261121T150627Z/deps-lock.json"
        result = delivery.verify_source_contract(lock, contract, delivery.digest(contract), self.inputs())
        self.assertTrue(result["sourceTupleVerified"])
        self.assertFalse(result["archiveBytesVerified"])
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_source_contract(lock, contract, "0" * 64, self.inputs())
        value = delivery.small_json(contract)
        value["sources"][0]["url"] += "?mirror=changed"
        bad = self.json_file("bad-tuple.json", value)
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_source_contract(lock, bad, delivery.digest(bad), self.inputs())

    def test_missing_supplemental_source_fails_closed(self):
        contract = self.contract(omit_imageformats=True)
        lock = ROOT / "tests/native-macos/inputs/frozen-20261121T150627Z/deps-lock.json"
        with self.assertRaisesRegex(delivery.DeliveryError, "qtimageformats"):
            delivery.verify_source_contract(lock, contract, delivery.digest(contract), self.inputs())

    def test_relative_paths_reject_escape(self):
        for value in ("", "../x", "/x", "C:/x", "x\\y", "x/../y", "x//y", "./x", "x\x00y"):
            with self.subTest(value=value), self.assertRaises(delivery.DeliveryError):
                delivery.safe_relative(value)
        self.assertEqual(str(delivery.safe_relative("QtCore.framework/Versions/A/QtCore")),
                         "QtCore.framework/Versions/A/QtCore")

    def test_otool_libraries_and_build_load_commands(self):
        text = "App:\n\t@rpath/QtCore.framework/Versions/A/QtCore (compatibility version 6.0.0, current version 6.8.3)\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1345.0.0)\n"
        self.assertEqual(len(delivery.parse_otool_libraries(text)), 2)
        commands = delivery.parse_load_commands("cmd LC_RPATH\npath @executable_path/../Frameworks (offset 12)\ncmd LC_BUILD_VERSION\nplatform 1\nminos 12.0\nsdk 15.5\ncmd LC_ID_DYLIB\nname @rpath/QtCore.framework/Versions/A/QtCore (offset 24)\n")
        self.assertEqual(commands["minimums"], ["12.0"])
        self.assertEqual(commands["rpaths"], ["@executable_path/../Frameworks"])
        self.assertTrue(commands["installId"].startswith("@rpath/"))
        with self.assertRaises(delivery.DeliveryError):
            delivery.parse_otool_libraries("")
        with self.assertRaises(delivery.DeliveryError):
            delivery.parse_otool_libraries("App:\n\tunexpected syntax\n")

    def test_no_developer_prefix_or_browser_import(self):
        bundle = self.root / "App.app"
        executable = bundle / "Contents/MacOS/App"
        framework = bundle / "Contents/Frameworks/QtCore.framework/Versions/A/QtCore"
        executable.parent.mkdir(parents=True)
        framework.parent.mkdir(parents=True)
        executable.write_bytes(b"app")
        framework.write_bytes(b"library")
        result = delivery.resolve_dependency("@rpath/QtCore.framework/Versions/A/QtCore", executable,
                                             executable, ["@executable_path/../Frameworks"], bundle)
        self.assertFalse(result["system"])
        for library in ("/opt/homebrew/lib/libcrypto.3.dylib", "@rpath/QtWebEngineCore.framework/QtWebEngineCore",
                        "@loader_path/../../../../outside", "@rpath/unresolved.dylib", "/System/Library/../evil"):
            with self.subTest(library=library), self.assertRaises(delivery.DeliveryError):
                delivery.resolve_dependency(library, executable, executable,
                                            ["@executable_path/../Frameworks"], bundle)
        self.assertTrue(delivery.resolve_dependency("/usr/lib/libSystem.B.dylib", executable,
                                                    executable, [], bundle)["system"])

    def test_bundle_links_cannot_escape(self):
        bundle = self.root / "App.app"
        bundle.mkdir()
        (bundle / "readme.txt").write_text("fixture", encoding="ascii")
        (bundle / "readme.txt").chmod(0o644)
        if os.name != "nt":
            os.symlink(self.root / "external", bundle / "escape")
            with self.assertRaises(delivery.DeliveryError):
                delivery.bundle_inventory(bundle)
        (bundle / "QtNetwork.framework").mkdir()
        with self.assertRaises(delivery.DeliveryError):
            delivery.bundle_inventory(bundle)

    def test_source_archive_hash_and_links(self):
        archive = self.root / "fixture.tar"
        with tarfile.open(archive, "w") as stream:
            member = tarfile.TarInfo("fixture/CMakeLists.txt")
            data = b"cmake_minimum_required(VERSION 3.25)\n"
            member.size = len(data)
            stream.addfile(member, io.BytesIO(data))
        entry = {"sha256": delivery.digest(archive), "sourceDirectory": "fixture"}
        source = delivery.extract_verified_archive(archive, entry, self.root / "source")
        self.assertTrue((source / "CMakeLists.txt").is_file())
        with self.assertRaises(delivery.DeliveryError):
            delivery.extract_verified_archive(archive, entry, self.root / "source")
        entry["sha256"] = "0" * 64
        with self.assertRaises(delivery.DeliveryError):
            delivery.extract_verified_archive(archive, entry, self.root / "wrong")
        with tarfile.open(archive, "w") as stream:
            member = tarfile.TarInfo("fixture/escape")
            member.type = tarfile.SYMTYPE
            member.linkname = "../../external"
            stream.addfile(member)
        entry["sha256"] = delivery.digest(archive)
        with self.assertRaises(delivery.DeliveryError):
            delivery.extract_verified_archive(archive, entry, self.root / "linked")

    def tar_member(self, stream, name, kind="file", data=b"synthetic source bytes\n", linkname=None):
        member = tarfile.TarInfo(name)
        if kind == "sym":
            member.type = tarfile.SYMTYPE
            member.linkname = linkname
            stream.addfile(member)
        elif kind == "file":
            member.size = len(data)
            stream.addfile(member, io.BytesIO(data))
        else:
            member.type = kind
            stream.addfile(member)

    def member_archive(self, name, members):
        archive = self.root / name
        with tarfile.open(archive, "w") as stream:
            for member in members:
                self.tar_member(stream, member[0], kind=member[1] if len(member) > 1 else "file",
                                linkname=member[2] if len(member) > 2 else None)
        return archive, {"sha256": delivery.digest(archive), "sourceDirectory": "fixture"}

    def test_safe_internal_relative_symlinks_extract_like_pinned_zstd(self):
        probe_target = self.root / "probe-target.txt"
        probe_target.write_bytes(b"probe")
        try:
            os.symlink(probe_target.name, self.root / "probe-link")
        except OSError:
            self.skipTest("Host cannot create OS symlinks; the rejection matrix still covers the policy")
        archive = self.root / "safe.tar"
        with tarfile.open(archive, "w") as stream:
            self.tar_member(stream, "fixture/", kind=tarfile.DIRTYPE)
            self.tar_member(stream, "fixture/CMakeLists.txt", data=b"cmake_minimum_required(VERSION 3.25)\n")
            self.tar_member(stream, "fixture/programs/", kind=tarfile.DIRTYPE)
            self.tar_member(stream, "fixture/programs/zstd")
            self.tar_member(stream, "fixture/tests/", kind=tarfile.DIRTYPE)
            self.tar_member(stream, "fixture/tests/cli-tests/", kind=tarfile.DIRTYPE)
            self.tar_member(stream, "fixture/tests/cli-tests/bin/", kind=tarfile.DIRTYPE)
            self.tar_member(stream, "fixture/tests/cli-tests/bin/zstd")
            # The exact pinned zstd-1.5.7 members: one-hop relative links to a real member.
            self.tar_member(stream, "fixture/tests/cli-tests/bin/unzstd", kind="sym", linkname="zstd")
            self.tar_member(stream, "fixture/tests/cli-tests/bin/zstdcat", kind="sym", linkname="zstd")
            # A relative link to an explicit real directory member also passes.
            self.tar_member(stream, "fixture/dir-link", kind="sym", linkname="tests")
        entry = {"sha256": delivery.digest(archive), "sourceDirectory": "fixture"}
        source = delivery.extract_verified_archive(archive, entry, self.root / "safe-source")
        self.assertTrue((source / "CMakeLists.txt").is_file())
        self.assertTrue((source / "programs/zstd").is_file())
        for name in ("unzstd", "zstdcat"):
            link = source / "tests/cli-tests/bin" / name
            self.assertTrue(link.is_symlink())
            self.assertEqual(os.readlink(link), "zstd")
            self.assertTrue(link.is_file())
        self.assertEqual(os.readlink(source / "dir-link"), "tests")
        self.assertTrue((source / "dir-link" / "cli-tests").is_dir())

    def test_unsafe_source_symlink_and_member_shapes_fail_closed(self):
        base = ("fixture/CMakeLists.txt", "file")
        cases = {
            "absolute-target": ("fixture/abs", "sym", "/etc/passwd"),
            "protocol-target": ("fixture/abs2", "sym", "//server/share"),
            "windows-drive-target": ("fixture/win", "sym", "C:/temp/evil"),
            "windows-bare-drive-target": ("fixture/win2", "sym", "C:evil"),
            "backslash-target": ("fixture/back", "sym", "..\\evil"),
            "empty-target": ("fixture/empty", "sym", ""),
            "root-escape": ("fixture/escape", "sym", "../../outside"),
            "normalized-escape": ("fixture/escape2", "sym", "sub/../../above"),
            "dangling-target": ("fixture/dangling", "sym", "missing-target"),
            "dangling-parent-target": ("fixture/dangling2", "sym", "../missing"),
            "symlink-chain": [("fixture/link2", "sym", "CMakeLists.txt"), ("fixture/link1", "sym", "link2")],
            "self-loop": ("fixture/self", "sym", "self"),
            "symlink-cycle": [("fixture/loop-a", "sym", "loop-b"), ("fixture/loop-b", "sym", "loop-a")],
            "duplicate-member": [("fixture/dupe", "file"), ("fixture/dupe", "file")],
            "dir-file-name-collision": [("fixture/collide/", tarfile.DIRTYPE), ("fixture/collide", "file")],
            "file-with-nested-children": [("fixture/nested", "file"), ("fixture/nested/child", "file")],
            "hardlink": ("fixture/hard", tarfile.LNKTYPE, "CMakeLists.txt"),
            "special-member": ("fixture/fifo", tarfile.FIFOTYPE),
            "oversized-target": ("fixture/long", "sym", "z" * (delivery.MAX_SYMLINK_TARGET + 1)),
        }
        for label, extra in cases.items():
            with self.subTest(case=label):
                archive, entry = self.member_archive(label + ".tar", [base, *(extra if isinstance(extra, list) else [extra])])
                destination = self.root / ("dest-" + label)
                with self.assertRaises(delivery.DeliveryError):
                    delivery.extract_verified_archive(archive, entry, destination)
                self.assertEqual(list(destination.iterdir()), [])

    def test_source_symlink_count_is_bounded(self):
        archive = self.root / "too-many.tar"
        with tarfile.open(archive, "w") as stream:
            self.tar_member(stream, "fixture/CMakeLists.txt")
            for index in range(delivery.MAX_ARCHIVE_SYMLINKS + 1):
                self.tar_member(stream, f"fixture/link-{index}", kind="sym", linkname="CMakeLists.txt")
        entry = {"sha256": delivery.digest(archive), "sourceDirectory": "fixture"}
        destination = self.root / "bounded-dest"
        with self.assertRaisesRegex(delivery.DeliveryError, "too many symlinks"):
            delivery.extract_verified_archive(archive, entry, destination)
        self.assertEqual(list(destination.iterdir()), [])

    def test_verified_archive_hash_and_byte_count_still_gate_extraction(self):
        downloads = self.root / "downloads"
        downloads.mkdir()
        archive = downloads / "verified.tar"
        with tarfile.open(archive, "w") as stream:
            self.tar_member(stream, "fixture/CMakeLists.txt")
        entry = {"archive": "verified.tar", "sha256": delivery.digest(archive), "bytes": archive.stat().st_size,
                 "sourceDirectory": "fixture"}
        self.assertTrue(delivery.verify_archives({}, downloads, [entry])[0]["executed"])
        source = delivery.extract_verified_archive(archive, entry, self.root / "verified-source")
        self.assertTrue((source / "CMakeLists.txt").is_file())
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_archives({}, downloads, [dict(entry, sha256="0" * 64)])
        with self.assertRaisesRegex(delivery.DeliveryError, "byte count"):
            delivery.verify_archives({}, downloads, [dict(entry, bytes=entry["bytes"] + 1)])

    def test_verified_zip_wheel_members_still_reject_links_before_extraction(self):
        archive = self.root / "linked.whl"
        linked = zipfile.ZipInfo("cmake/data/bin/cmake")
        linked.create_system = 3
        linked.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr(linked, "../../outside")
        entry = {"name": "cmake-wheel", "version": "0", "archive": archive.name, "url": "https://example.invalid/cmake.whl",
                 "sha256": delivery.digest(archive), "bytes": archive.stat().st_size, "binaryDirectory": "cmake/data/bin"}
        destination = self.root / "wheel-root"
        destination.mkdir()
        with self.assertRaisesRegex(delivery.DeliveryError, "Linked tool wheel"):
            delivery.install_tool_wheel(destination, archive, entry)
        self.assertEqual(list(destination.iterdir()), [])

    def test_no_apple_host_claim_from_windows_or_linux(self):
        with patch.object(delivery.sys, "platform", "win32"), patch.object(delivery.shutil, "which", return_value=None):
            with self.assertRaisesRegex(delivery.DeliveryError, "Real Darwin"):
                delivery.apple_environment(self.inputs()["policy"], "arm64")
            self.assertFalse(delivery.discover()["appleHost"])
        with patch.object(delivery.sys, "platform", "linux"):
            with self.assertRaisesRegex(delivery.DeliveryError, "Real Darwin"):
                delivery.apple_environment(self.inputs()["policy"], "x86_64")

    def test_per_arch_native_gate_rejects_translation(self):
        with patch.object(delivery.sys, "platform", "darwin"), patch.object(delivery, "capture") as capture:
            capture.return_value = {"returnCode": 0, "stdout": "arm64\n"}
            with self.assertRaisesRegex(delivery.DeliveryError, "matching native Apple"):
                delivery.apple_environment(self.inputs()["policy"], "x86_64")
            capture.side_effect = [{"returnCode": 0, "stdout": "x86_64\n"},
                                   {"returnCode": 0, "stdout": "1\n"}]
            with self.assertRaisesRegex(delivery.DeliveryError, "Translated"):
                delivery.apple_environment(self.inputs()["policy"], "x86_64")

    def test_sources_absent_do_not_pass(self):
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_archives(self.inputs(), self.root)

    def test_json_write_keeps_machine_evidence_not_pass_labels(self):
        target = self.root / "report.json"
        delivery.write_json(target, {"schema": 1, "appleRuntimeExecuted": False})
        self.assertFalse(delivery.small_json(target)["appleRuntimeExecuted"])
        self.assertEqual(delivery.version_tuple("12.0"), (12, 0, 0))
        self.assertLess(delivery.version_tuple("12"), delivery.version_tuple("15.5"))

    def test_real_file_manifest_is_not_a_stamp(self):
        (self.root / "source.cpp").write_text("int main() {}", encoding="ascii")
        value = {"schema": 1, "files": [{"path": "source.cpp", "sha256": delivery.digest(self.root / "source.cpp")}]}
        manifest = self.json_file("source-manifest.json", value)
        record = delivery.verify_file_manifest(self.root, manifest, delivery.digest(manifest), "Synthetic source")
        self.assertEqual(record["filesVerified"], 1)
        (self.root / "source.cpp").write_text("changed", encoding="ascii")
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_file_manifest(self.root, manifest, delivery.digest(manifest), "Synthetic source")
        value["files"][0]["path"] = "../outside.cpp"
        invalid = self.json_file("invalid-manifest.json", value)
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_file_manifest(self.root, invalid, delivery.digest(invalid), "Synthetic source")

    def test_tools_need_acceptance_pin_before_fetch(self):
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_tools_contract(self.inputs(), {"tools": []})
        approved = {"tools": self.inputs()["tools"]["sources"]}
        delivery.verify_tools_contract(self.inputs(), approved)
        approved["tools"][0]["sha256"] = "0" * 64
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_tools_contract(self.inputs(), approved)

    def test_production_runtime_receipt_cannot_be_a_design_or_mock(self):
        value = {"schema": 1, "kind": "ci-workflow-template", "executed": False}
        receipt = self.json_file("unexecuted-receipt.json", value)
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_runtime_receipt(receipt, delivery.digest(receipt), {"bundleAudit": {}})

    def test_production_bundle_codemodel_required(self):
        with self.assertRaises(delivery.DeliveryError):
            delivery.verify_cmake_target(self.root / "no-build", "ni-launcher", self.root, set())

    def test_reviewed_tool_version_transcripts_are_exact(self):
        inputs = self.inputs()
        self.assertEqual(inputs["tools"]["cmakeVersion"], "3.31.6")
        self.assertEqual(inputs["tools"]["ninjaVersion"], "1.13.0")
        self.assertEqual(delivery.REVIEWED_TOOL_OUTPUTS["ninja"]["1.13.0"]["exactLines"],
                         {("1.13.0",), ("1.13.0.git.kitware.jobserver-pipe-1",)})
        accepted = [
            ("cmake", "3.31.6", "cmake version 3.31.6\n"),
            ("cmake", "3.31.6",
             "cmake version 3.31.6\n\nCMake suite maintained and supported by Kitware (kitware.com/cmake).\n"),
            ("ninja", "1.13.0", "1.13.0\n"),
            ("ninja", "1.13.0", "1.13.0.git.kitware.jobserver-pipe-1\n"),
        ]
        for name, pinned, output in accepted:
            with self.subTest(name=name, pinned=pinned, output=output):
                self.assertIsNone(delivery.verify_tool_output(name, pinned, output))

    def test_unreviewed_tool_output_fails_closed(self):
        rejected = [
            ("cmake", "3.31.6", ""),
            ("cmake", "3.31.6", None),
            ("cmake", "3.31.6",
             "cmake version 3.31.5\n\nCMake suite maintained and supported by Kitware (kitware.com/cmake).\n"),
            ("cmake", "3.31.6", "cmake version 3.31.6.1\n"),
            ("cmake", "3.31.6", "cmake version 3.31.6\nforged extra line\n"),
            ("cmake", "3.31.7", "cmake version 3.31.7\n"),
            ("ninja", "1.13.0", ""),
            ("ninja", "1.13.0", None),
            ("ninja", "1.13.1", "1.13.1\n"),
            ("ninja", "1.13.0", "1.12\n"),
            ("ninja", "1.13.0", "1.13.0.1\n"),
            ("ninja", "1.13.0", "1.13.0.git.arbitrary-suffix\n"),
            ("ninja", "1.13.0", "1.13.0.git.kitware.jobserver-pipe-2\n"),
            ("ninja", "1.13.0", "1.13.0 \n"),
            ("ninja", "1.13.0", "1.13.0\n1.13.0.git.kitware.jobserver-pipe-1\n"),
            ("ninja", "1.13.0", "forged\n1.13.0\n"),
            ("ninja", "1.13.0", " 1.13.0\n"),
            ("ninja", "1.12", "1.12\n"),
            ("unknown-tool", "1.13.0", "1.13.0\n"),
        ]
        for name, pinned, output in rejected:
            with self.subTest(name=name, pinned=pinned, output=output):
                with self.assertRaises(delivery.DeliveryError):
                    delivery.verify_tool_output(name, pinned, output)

    def test_core_and_qt_gates_use_the_reviewed_allowlist(self):
        core = (ROOT / "scripts/native/macos-delivery.core.py").read_text(encoding="utf-8")
        api = (ROOT / "scripts/native/macos-delivery.py").read_text(encoding="utf-8")
        self.assertIn('api["verify_tool_output"]("cmake"', core)
        self.assertIn('api["verify_tool_output"]("ninja"', core)
        self.assertNotIn("ninja_version.strip()", core)
        self.assertNotIn('cmake_version.startswith("cmake version "', core)
        self.assertEqual(api.count('verify_tool_output("cmake"'), 1)
        self.assertEqual(api.count('verify_tool_output("ninja"'), 1)

    def brand_oracle_stage(self, base=None):
        stage = (self.root if base is None else Path(base)) / "verified-stage"
        for relative in delivery.BRAND_ORACLE_REQUIRED:
            path = stage / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"synthetic oracle contract bytes, not product artwork")
        return stage

    def test_asset_oracle_contract_matches_the_brand_cmake_requirement(self):
        text = (ROOT / "native/tests/brand/CMakeLists.txt").read_text(encoding="utf-8-sig")
        items = re.search(r"foreach\(_required IN ITEMS (.*?)\)", text, re.S).group(1).split()
        self.assertEqual(tuple(items), delivery.BRAND_ORACLE_REQUIRED)

    def test_asset_oracle_root_binds_verified_stage_and_fails_closed(self):
        stage = self.brand_oracle_stage()
        oracle = delivery.asset_oracle_root(stage)
        self.assertTrue(oracle.is_absolute())
        self.assertEqual(oracle, stage.resolve())
        self.assertEqual(delivery.asset_oracle_root(stage, stage), oracle)
        outside = self.brand_oracle_stage(self.root / "unbound-root")
        with self.assertRaisesRegex(delivery.DeliveryError, "inside the verified source stage"):
            delivery.asset_oracle_root(stage, outside)
        with self.assertRaisesRegex(delivery.DeliveryError, "absolute trusted checkout path"):
            delivery.asset_oracle_root(stage, Path("relative/oracle"))
        with self.assertRaisesRegex(delivery.DeliveryError, "directory is missing"):
            delivery.asset_oracle_root(self.root / "absent-stage")
        with self.assertRaisesRegex(delivery.DeliveryError, "is missing src/assets/mod-placeholder.png"):
            delivery.asset_oracle_root(stage, stage / "src/assets")
        incomplete = self.root / "incomplete-stage"
        (incomplete / "src/assets/fonts").mkdir(parents=True)
        (incomplete / "src/assets/fonts/nickel-line.woff2").write_bytes(b"synthetic")
        with self.assertRaisesRegex(delivery.DeliveryError, "is missing src/assets/mod-placeholder.png"):
            delivery.asset_oracle_root(incomplete)
        with self.assertRaisesRegex(delivery.DeliveryError, "manifest omits the brand oracle file"):
            delivery.asset_oracle_root(stage, manifest_paths=set(delivery.BRAND_ORACLE_REQUIRED) - {"tests/brand/functional-icons.sha256"})

    def test_app_configure_passes_exactly_one_validated_absolute_oracle(self):
        configure = inspect.getsource(delivery.build_app)
        self.assertEqual(configure.count("-DASSET_ORACLE_ROOT="), 1)
        self.assertIn('f"-DASSET_ORACLE_ROOT={oracle}"', configure)
        self.assertIn("asset_oracle_root(source", configure)
        self.assertLess(configure.index("asset_oracle_root(source"), configure.index("recorder.run(arguments"))
        self.assertIn("-DBUILD_TESTING=ON", configure)
        self.assertIn('re.fullmatch(r"NICKEL_[A-Z0-9_]+", key)', configure)


if __name__ == "__main__":
    unittest.main(verbosity=2)
