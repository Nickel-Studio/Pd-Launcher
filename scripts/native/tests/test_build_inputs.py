"""Raw build-input parser regressions; no application or prefix writes."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_inputs", HERE / "build_inputs.py")
inputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inputs)


class BuildInputTests(unittest.TestCase):
    def test_rc_dependencies_are_not_replaced_by_source_only(self):
        text = "native/app/icon.rc.res: #deps 2, deps mtime 123 (VALID)\n    C:/SDK/winres.h\n    C:/SDK/sal.h\n\n"
        record = inputs.parse_ninja_deps(text)["native/app/icon.rc.res"]
        self.assertEqual(record["dependencies"], ["C:/SDK/winres.h", "C:/SDK/sal.h"])
        self.assertEqual(record["status"], "VALID")

    def test_truncated_dependency_record_fails(self):
        with self.assertRaises(inputs.EvidenceError):
            inputs.parse_ninja_deps("a.obj: #deps 2, deps mtime 1 (VALID)\n    a.hpp\n")

    def test_duplicate_dependency_record_fails(self):
        record = "a.obj: #deps 0, deps mtime 1 (VALID)\n\n"
        with self.assertRaises(inputs.EvidenceError):
            inputs.parse_ninja_deps(record + record)

    def test_unrecognized_dependency_text_fails(self):
        with self.assertRaises(inputs.EvidenceError):
            inputs.parse_ninja_deps("unexpected command output\n")

    def test_phony_requires_query_proof(self):
        text = "a:\n  input: phony\n  outputs:\n    a.obj\nb:\n  input: CXX_COMPILER\n"
        self.assertEqual(inputs.parse_phony_query(text), {"a"})

    def test_moc_depfile_preserves_drive_and_escaped_spaces(self):
        text = "C:/build/moc_a.cpp: C:/src/a.hpp \\\n C:/Program\\ Files/SDK/header.h\n"
        target, deps = inputs.parse_depfile(text)
        self.assertEqual(target, ["C:/build/moc_a.cpp"])
        self.assertEqual(deps, ["C:/src/a.hpp", "C:/Program Files/SDK/header.h"])

    def test_make_escaped_hash_and_dollar(self):
        self.assertEqual(inputs.make_words(r"one\#two $$name C:\SDK\header.h"), ["one#two", "$name", r"C:\SDK\header.h"])

    def test_missing_depfile_delimiter_fails(self):
        with self.assertRaises(inputs.EvidenceError):
            inputs.parse_depfile("C:/build/moc_a.cpp C:/src/a.hpp")

    def test_ninja_escaped_drive_and_order_only(self):
        text = "build native\\app\\a.exe: LINK C$:\\Qt\\init.obj a.obj | a.lib || order\n  CONFIG = Release\n  RSP_FILE = a.rsp\n\n"
        edge = inputs.ninja_edge(text, "native/app/a.exe")
        self.assertEqual(edge["inputs"]["explicit"], ["C:\\Qt\\init.obj", "a.obj"])
        self.assertEqual(edge["inputs"]["implicit"], ["a.lib"])
        self.assertEqual(edge["inputs"]["orderOnly"], ["order"])
        self.assertEqual(edge["variables"]["CONFIG"], "Release")

    def test_unexpanded_ninja_variable_fails(self):
        with self.assertRaises(inputs.EvidenceError):
            inputs.ninja_words("$unresolved/a.obj")

    def test_windows_arguments_roundtrip(self):
        values = [r"C:\Program Files\Qt\Qt6Core.lib", "C:\\plain\\", r'embedded"quote', "", "gdi32.lib"]
        self.assertEqual(inputs.windows_words(subprocess.list2cmdline(values)), values)

    def test_unclosed_windows_quote_fails(self):
        with self.assertRaises(inputs.EvidenceError):
            inputs.windows_words('"unfinished path')

    def test_map_library_names_not_guess_from_arbitrary_text(self):
        text = " 0001:00001a00 symbol 0000000140002a00 f i LIBCMT:memcpy.obj\n comment Fake:member.obj\n"
        self.assertEqual(inputs.map_libraries(text), {"LIBCMT.lib"})

    def test_duplicate_paths_merge_roles_without_counting_twice(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.lib"
            path.write_bytes(b"same")
            records = inputs.Records()
            records.add(path, ["explicit"])
            records.add(path, ["actual-map"])
            self.assertEqual(len(records.rows()), 1)
            self.assertEqual(records.rows()[0]["roles"], ["actual-map", "explicit"])

    def test_rehash_drift_and_missing_file_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.hpp"
            path.write_bytes(b"original")
            records = inputs.Records()
            recorded = dict(records.add(path, ["header"]))
            path.write_bytes(b"changed")
            with self.assertRaises(inputs.EvidenceError):
                records.add(path, ["header"], recorded)
            path.unlink()
            with self.assertRaises(inputs.EvidenceError):
                records.add(path, ["header"])


if __name__ == "__main__":
    unittest.main()
