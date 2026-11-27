"""Self-tests of evidence handling only; no AppServices production claims."""
from __future__ import annotations

import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evidence
import generate_fixtures
import prepare_same_graph


class Tools(unittest.TestCase):
    def setUp(self) -> None:
        parent = evidence.checked_local(".snow/production-smoke/tools-temp", writable=True)
        evidence.capacity(parent, 1024 * 1024)
        parent.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix="owned-tools-", dir=parent))
        self.owner_token = self.root.name
        self.put("owner.json", {"owner": "production-smoke", "token": self.owner_token})

    def tearDown(self) -> None:
        marker = evidence.read_json(self.root / "owner.json")
        if marker != {"owner": "production-smoke", "token": self.owner_token}:
            raise RuntimeError("test cleanup ownership mismatch")
        evidence.checked_local(self.root, writable=True)
        shutil.rmtree(self.root)

    def put(self, name: str, value) -> Path:
        path = self.root / name
        evidence.write_json_new(path, value)
        return path

    def text(self, name: str, value: str) -> Path:
        path = evidence.checked_local(self.root / name, writable=True)
        evidence.capacity(path, len(value.encode("utf-8")))
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8") as stream:
            stream.write(value)
        return path

    def blob(self, name: str, value: bytes) -> Path:
        path = evidence.checked_local(self.root / name, writable=True)
        evidence.capacity(path, len(value))
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as stream:
            stream.write(value)
        return path

    @staticmethod
    def elf_bytes(kind=2, bits=64, byteorder="little") -> bytes:
        endian = "<" if byteorder == "little" else ">"
        elf64 = bits == 64
        minimum, program_size = (64, 56) if elf64 else (52, 32)
        payload = b"TOOL_ONLY_NOT_A_REAL_APP_BINARY\n"
        size, address = minimum + program_size + len(payload), 0x1000
        ident = b"\x7fELF" + bytes((2 if elf64 else 1, 1 if byteorder == "little" else 2, 1)) + b"\x00" * 9
        header = struct.pack(endian + ("16sHHIQQQIHHHHHH" if elf64 else "16sHHIIIIIHHHHHH"),
            ident, kind, 62 if elf64 else 3, 1, address + minimum + program_size, minimum, 0, 0,
            minimum, program_size, 1, 0, 0, 0)
        program = (struct.pack(endian + "IIQQQQQQ", 1, 5, 0, address, address, size, size, 0x1000) if elf64
                   else struct.pack(endian + "IIIIIIII", 1, 0, address, address, size, size, 5, 0x1000))
        return header + program + payload

    @staticmethod
    def ar_bytes(members=None) -> bytes:
        if members is None:
            members = [(b"sample.o/", b"TOOL_ONLY_NOT_A_REAL_OBJECT\n")]
        archive = bytearray(b"!<arch>\n")
        for name, payload in members:
            header = (name.ljust(16) + b"0".ljust(12) + b"0".ljust(6) + b"0".ljust(6)
                      + b"100644".ljust(8) + str(len(payload)).encode("ascii").ljust(10) + b"`\n")
            archive.extend(header + payload + (b"\n" if len(payload) % 2 else b""))
        return bytes(archive)

    def artifact_row(self, name: str, payload: bytes, role="smoke-executable") -> dict:
        path = self.blob(name, payload)
        if role != "production-library":
            path.chmod(0o755)
        return {"role": role, "physicalPath": str(path), "bytes": path.stat().st_size, "sha256": evidence.sha256(path)}


    def freeze(self, *, complete=True, wrong_marker=False, record_extra=None):
        source = self.text("native/app/AppServices.hpp", "class AppServices {};\n")
        record = {"path": "native/app/AppServices.hpp", "size": source.stat().st_size,
                  "sha256": evidence.sha256(source)}
        record.update(record_extra or {})
        manifest = self.put("snapshot.json", {"schema": 1, "files": [record]})
        pin = evidence.sha256(manifest)
        marker = self.put("publication-complete.json", {
            "state": "complete" if complete else "writing",
            "manifestSha256": "0" * 64 if wrong_marker else pin,
            "completedAt": "2026-11-21T13:39:13Z"})
        return manifest, marker, pin

    def verify(self, manifest, marker, pin, required=None):
        return evidence.verify_freeze(self.root, manifest, marker, pin, required)

    def gate_inputs(self):
        matrix = {"cases": [{"id": "startup-root", "checks": ["whole-root", "restart"]}]}
        freeze = {"kind": "verified-frozen-inputs", "fileCount": 2, "manifestSha256": "a" * 64}
        command = {"kind": "command-evidence", "exitCode": 0, "timedOut": False,
                   "executableSha256": "b" * 64}
        cases = {"schema": 1, "kind": "production-appservices-fixture", "realAppServices": True,
                 "realQObject": True, "manifestSha256": "a" * 64, "binarySha256": "b" * 64,
                 "cases": [{"id": "startup-root", "verdict": "PASS",
                            "checks": {"whole-root": True, "restart": True}}]}
        ctest = {"kind": "ctestInfo", "tests": [
            {"name": "native_production_appservices", "command": ["fixture.exe", "--run"]}]}
        return freeze, command, cases, matrix, ctest, ["native_production_appservices"]

    def test_canonical_relative_paths(self):
        self.assertEqual(evidence.relative_path("native/app/services.hpp"), "native/app/services.hpp")
        for value in (None, "", "../x", "/x", "a/../x", "a/./x", "a//x", "C:/x", "a\\x", "a\x00x"):
            with self.subTest(value=value), self.assertRaises(evidence.EvidenceError):
                evidence.relative_path(value)

    def test_owner_scopes(self):
        with self.assertRaises(evidence.EvidenceError):
            evidence.checked_local(evidence.OWNER.parent / "foreign.cpp")
        with self.assertRaises(evidence.EvidenceError):
            evidence.checked_local("native/app/foreign.cpp", writable=True)

    def test_capacity_fail_closed(self):
        low = shutil._ntuple_diskusage(100, 99, 1)
        with mock.patch.object(evidence.shutil, "disk_usage", return_value=low):
            with self.assertRaises(evidence.EvidenceError):
                evidence.capacity(self.root / "large", 2, 0)

    def test_exclusive_output(self):
        path = self.put("one.json", {"value": 1})
        with self.assertRaises(FileExistsError):
            evidence.write_json_new(path, {"value": 2})
        self.assertEqual(evidence.read_json(path), {"value": 1})

    def test_duplicate_json_rejected(self):
        path = self.text("duplicate.json", '{"state":"writing","state":"complete"}')
        with self.assertRaises(evidence.EvidenceError):
            evidence.read_json(path)

    def test_hash_strict(self):
        for value in (None, "A" * 64, "b" * 63, "z" * 64, 2):
            with self.subTest(value=value), self.assertRaises(evidence.EvidenceError):
                evidence.require_hash(value, "test")

    def test_completed_freeze(self):
        result = self.verify(*self.freeze(), required=["native/app/AppServices.hpp"])
        self.assertEqual(result["fileCount"], 1)
        self.assertFalse(result["productionPass"])

    def bounded_freeze(self, **changes):
        manifest, _, pin = self.freeze()
        marker = self.put("bounded-complete.json", {
            "schemaVersion": 1, "kind": "bounded-source-publication-complete",
            "publicationComplete": True, "receiver": "production-smoke",
            "finishedUtc": "2026-11-21T14:19:30Z", "receiverManifestSha256": pin,
            "sourceFiles": 1,
            "sourceBytes": (self.root / "native/app/AppServices.hpp").stat().st_size,
            "classification": "frozen-source-header-contractWIP-not-product-link-or-runtime-PASS",
            "productionBinaryAvailable": False, **changes})
        return manifest, marker, pin

    def test_bounded_complete_contract_is_not_production(self):
        result = self.verify(*self.bounded_freeze())
        self.assertEqual(result["fileCount"], 1)
        self.assertEqual(result["classification"], "frozen-source-header-contractWIP-not-product-link-or-runtime-PASS")
        self.assertFalse(result["productionBinaryAvailable"])
        self.assertFalse(result["matchingDomainLibrariesAvailable"])
        self.assertFalse(result["actualCTestInventoryAvailable"])
        self.assertFalse(result["productionPass"])

    def test_bounded_publication_rejects_wrong_receiver(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.bounded_freeze(receiver="another-owner"))

    def test_bounded_publication_requires_boolean_complete(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.bounded_freeze(publicationComplete=1))

    def test_bounded_publication_rejects_false_complete(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.bounded_freeze(publicationComplete=False))

    def test_bounded_publication_rejects_wrong_counts(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.bounded_freeze(sourceFiles=2))

    def test_bounded_publication_rejects_boolean_count(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.bounded_freeze(sourceFiles=True))

    def test_bounded_publication_rejects_wrong_bytes(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.bounded_freeze(sourceBytes=0))

    def test_bounded_publication_requires_timezone(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.bounded_freeze(finishedUtc="2026-11-21T14:19:30"))

    def test_bounded_publication_rejects_wrong_manifest_pin(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.bounded_freeze(receiverManifestSha256="0" * 64))

    def test_incomplete_publication_rejected(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.freeze(complete=False))

    def test_marker_hash_mismatch(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.freeze(wrong_marker=True))

    def test_manifest_pin_mismatch(self):
        manifest, marker, _ = self.freeze()
        with self.assertRaises(evidence.EvidenceError):
            self.verify(manifest, marker, "0" * 64)

    def test_source_hash_mismatch(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.freeze(record_extra={"sha256": "0" * 64}))

    def test_source_size_mismatch(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.freeze(record_extra={"size": 999}))

    def test_missing_required_source(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.freeze(), required=["native/app/service/launch_actions.cpp"])

    def test_case_colliding_manifest(self):
        source = self.text("native/app/a.cpp", "a\n")
        record = {"path": "native/app/a.cpp", "sha256": evidence.sha256(source)}
        manifest = self.put("snapshot.json", {"files": [record, {**record, "path": "native/app/A.cpp"}]})
        pin = evidence.sha256(manifest)
        marker = self.put("publication-complete.json", {
            "state": "complete", "manifestSha256": pin, "completedAt": "2026-11-21T00:00:00Z"})
        with self.assertRaises(evidence.EvidenceError):
            self.verify(manifest, marker, pin)

    def test_manifest_escape(self):
        with self.assertRaises(evidence.EvidenceError):
            self.verify(*self.freeze(record_extra={"path": "../foreign.cpp"}))

    def test_enum_inventory(self):
        text = 'enum class UiQuery { A, B = 3, /* comment */ C, };'
        self.assertEqual(evidence.enum_members(text, "UiQuery"), ["A", "B", "C"])
        for text in ('enum class UiQuery { A, A };', 'enum class UiQuery { A = call() };',
                     'enum class UiQuery { A }; enum class UiQuery { B };'):
            with self.assertRaises(evidence.EvidenceError):
                evidence.enum_members(text, "UiQuery")

    def test_actual_baseline_inventory(self):
        matrix = evidence.load_matrix("tests/native-production-smoke/matrix.json")
        current = copy.deepcopy(matrix)
        for case in current["cases"]:
            case["queries"] = [value for value in case["queries"] if value != "Onboarding"]
            case["actions"] = [value for value in case["actions"] if value not in {
                "AcceptEula", "AcknowledgeChangelog", "FinishOnboarding", "RejectEula"}]
        baseline = self.put("baseline-matrix.json", current)
        result = evidence.inventory(evidence.OWNER / "native/ui/UiServices.hpp", baseline)
        self.assertEqual(result["UiQuery"]["count"], 22)
        self.assertEqual(result["UiAction"]["count"], 54)
        self.assertFalse(result["productionPass"])

    def test_inventory_detects_new_action(self):
        header = self.text("port.hpp", 'enum class UiQuery { A }; enum class UiAction { New };')
        matrix = self.put("matrix.json", {"schema": 1, "cases": [
            {"id": "case-a", "queries": ["A"], "actions": [], "checks": ["actual"], "requirement_ids": ["D01"]}]})
        with self.assertRaises(evidence.EvidenceError):
            evidence.inventory(header, matrix)

    def test_empty_ctest_rejected(self):
        with self.assertRaises(evidence.EvidenceError):
            evidence.ctest_inventory({"kind": "ctestInfo", "tests": []}, ["production"])

    def test_tool_only_ctest_rejected(self):
        document = {"kind": "ctestInfo", "tests": [{"name": "tools", "command": ["python", "selftest"]}]}
        with self.assertRaises(evidence.EvidenceError):
            evidence.ctest_inventory(document, ["tools"])

    def test_ctest_requires_bound_command(self):
        with self.assertRaises(evidence.EvidenceError):
            evidence.ctest_inventory({"kind": "ctestInfo", "tests": [{"name": "production"}]}, ["production"])

    def test_gate_is_scoped_and_complete(self):
        result = evidence.production_gate(*self.gate_inputs())
        self.assertEqual(result["scope"], "independent-AppServices-fixture-only")
        self.assertFalse(result["completeProductPass"])

    def test_gate_never_upgrades_missing_cases(self):
        values = list(self.gate_inputs())
        values[2]["cases"] = []
        with self.assertRaises(evidence.EvidenceError):
            evidence.production_gate(*values)

    def test_gate_never_upgrades_blocked(self):
        values = list(self.gate_inputs())
        values[2]["cases"][0]["verdict"] = "BLOCKED"
        with self.assertRaises(evidence.EvidenceError):
            evidence.production_gate(*values)

    def test_gate_never_upgrades_fake(self):
        values = list(self.gate_inputs())
        values[2]["realAppServices"] = False
        with self.assertRaises(evidence.EvidenceError):
            evidence.production_gate(*values)

    def test_gate_never_upgrades_wrong_binary(self):
        values = list(self.gate_inputs())
        values[2]["binarySha256"] = "c" * 64
        with self.assertRaises(evidence.EvidenceError):
            evidence.production_gate(*values)

    def test_gate_never_upgrades_tool_output(self):
        values = list(self.gate_inputs())
        values[2]["kind"] = "tool-selftest"
        with self.assertRaises(evidence.EvidenceError):
            evidence.production_gate(*values)

    def test_gate_assertions_strict_boolean(self):
        values = list(self.gate_inputs())
        values[2]["cases"][0]["checks"]["restart"] = 1
        with self.assertRaises(evidence.EvidenceError):
            evidence.production_gate(*values)

    def test_command_evidence_preserves_exit_and_hash(self):
        output = self.root / "command-result"
        result = evidence.run_command([sys.executable, "-B", "-c", "print('tools only'); raise SystemExit(7)"],
                                      evidence.OWNER, output, timeout=10)
        self.assertEqual(result["exitCode"], 7)
        self.assertFalse(result["productionPass"])
        self.assertIn("tools only", Path(result["stdout"]).read_text())
        self.assertEqual(result["stdoutSha256"], evidence.sha256(result["stdout"]))

    def test_fixture_bytes_are_deterministic(self):
        entries = {"config/a.txt": b"alpha\n", "modrinth.index.json": b"{}\n"}
        first = generate_fixtures.archive_bytes(entries)
        second = generate_fixtures.archive_bytes(entries)
        self.assertEqual(first, second)
        with zipfile.ZipFile(io.BytesIO(first)) as archive:
            self.assertEqual(archive.read("config/a.txt"), b"alpha\n")
            self.assertIsNone(archive.testzip())

    def test_fixture_archive_escape_rejected(self):
        with self.assertRaises(evidence.EvidenceError):
            generate_fixtures.archive_bytes({"../escape.txt": b"not allowed"})

    def test_fixture_preserves_long_path_and_exact_integers(self):
        result = generate_fixtures.generate(self.root / "fixture", "0.1.0")
        self.assertGreaterEqual(result["rootCharacters"], 310)
        self.assertTrue(str(result["root"]).startswith(str(evidence.OWNER)))
        accepted = result["scenarios"][0]
        settings = evidence.read_json(Path(accepted["expectedConfigRoot"]) / "settings.json")
        self.assertEqual(settings["future"]["integerBeyondDouble"], 9007199254740993)
        self.assertEqual(settings["future"]["unsignedMax"], 18446744073709551615)
        self.assertFalse(result["productionPass"])
        for record in result["files"]:
            path = Path(result["root"]) / record["path"]
            self.assertEqual(evidence.sha256(path), record["sha256"])
            self.assertEqual(path.stat().st_size, record["size"])

    def test_corrupt_game_proof_keeps_whole_system_root(self):
        result = generate_fixtures.generate(self.root / "fixture", "0.1.0", deep=False)
        scenarios = {row["name"]: row for row in result["scenarios"]}
        self.assertEqual(set(scenarios), {"accepted", "unaccepted", "system-priority", "corrupt-proof"})
        corrupt = scenarios["corrupt-proof"]
        config = Path(corrupt["expectedConfigRoot"])
        self.assertEqual(config, Path(corrupt["systemConfigBase"]) / "studio.nickel.launcher")
        self.assertEqual(Path(corrupt["corruptProofFile"]), config / "game.json")
        self.assertEqual(evidence.sha256(corrupt["corruptProofFile"]), corrupt["corruptProofSha256"])
        with self.assertRaises(json.JSONDecodeError):
            json.loads(Path(corrupt["corruptProofFile"]).read_bytes())
        registry = evidence.read_json(config / "instances.json")
        self.assertEqual(registry["active"], corrupt["expectedGameRoot"])
        self.assertEqual(len(corrupt["foreignRootProofs"]), 2)
        for proof in corrupt["foreignRootProofs"]:
            self.assertEqual(evidence.sha256(proof["path"]), proof["sha256"])
        self.assertEqual(Path(corrupt["executable"]).name, "Pd-Launcher.exe")
        self.assertFalse(result["productionPass"])

    def test_strict_first_finish_is_required_without_adding_cases(self):
        matrix = evidence.load_matrix(evidence.OWNER / "tests/native-production-smoke/matrix.json")
        self.assertEqual(len(matrix["cases"]), 22)
        startup = next(row for row in matrix["cases"] if row["id"] == "startup-once")
        self.assertIn("first-finish-gate", startup["checks"])
        self.assertIn("FinishOnboarding", startup["actions"])
        self.assertIn("LaunchGame", startup["actions"])

    def same_graph_contract(self):
        pins = {"source_pin": "a" * 64, "source_commit": "c" * 40, "harness_pin": "b" * 64,
                "ledger_pin": "d" * 64, "compile_pin": "e" * 64, "link_pin": "f" * 64}
        source = {"sourceCommit": pins["source_commit"]}
        harness = {"kind": "owned-production-smoke-source-delta", "owner": "production-smoke"}
        identities = {"sourceCommit": pins["source_commit"], "sourceGenerationManifestSha256": pins["source_pin"],
                      "harnessManifestSha256": pins["harness_pin"]}
        artifacts = [{"role": role, "physicalPath": str(self.root / name), "bytes": 1, "sha256": "9" * 64}
                     for role, name in (("production-library", "app.lib"), ("production-executable", "Pd.exe"),
                                        ("smoke-executable", "matrix.exe"))]
        ledger = {"kind": "matched-same-graph-production-artifacts", **identities, "artifacts": artifacts}
        complete = {"publicationComplete": True, "receiver": "production-smoke", **identities,
                    "artifactManifestSha256": pins["ledger_pin"], "compileCommandsSha256": pins["compile_pin"],
                    "linkRecipeSha256": pins["link_pin"]}
        return source, harness, ledger, complete, pins

    def test_same_graph_structural_validator_does_not_claim_runtime(self):
        source, harness, ledger, complete, pins = self.same_graph_contract()
        self.assertEqual(prepare_same_graph.validate_graph(source, harness, ledger, complete, **pins), ledger["artifacts"])
        self.assertNotIn("productionPass", ledger)

    def test_same_graph_rejects_each_mixed_generation_scope(self):
        source, harness, ledger, complete, pins = self.same_graph_contract()
        for target, field in ((source, "sourceCommit"), (ledger, "sourceCommit"),
                              (ledger, "sourceGenerationManifestSha256"), (ledger, "harnessManifestSha256"),
                              (complete, "artifactManifestSha256"), (complete, "compileCommandsSha256"),
                              (complete, "linkRecipeSha256")):
            with self.subTest(field=field):
                original = target[field]
                target[field] = "0" * len(original)
                with self.assertRaises(evidence.EvidenceError):
                    prepare_same_graph.validate_graph(source, harness, ledger, complete, **pins)
                target[field] = original

    def test_same_graph_rejects_unfinished_or_foreign_completion(self):
        source, harness, ledger, complete, pins = self.same_graph_contract()
        for key, value in (("publicationComplete", False), ("publicationComplete", 1), ("receiver", "another-owner")):
            altered = {**complete, key: value}
            with self.subTest(key=key, value=value), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_graph(source, harness, ledger, altered, **pins)

    def test_same_graph_rejects_missing_or_duplicate_artifacts(self):
        source, harness, ledger, complete, pins = self.same_graph_contract()
        for artifacts in (ledger["artifacts"][:-1], ledger["artifacts"][1:],
                          ledger["artifacts"] + [ledger["artifacts"][0]]):
            with self.subTest(count=len(artifacts)), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_graph(source, harness, {**ledger, "artifacts": artifacts}, complete, **pins)

    def test_same_graph_rejects_hash_prefixes_and_boolean_sizes(self):
        source, harness, ledger, complete, pins = self.same_graph_contract()
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.validate_graph(source, harness, ledger, complete, **{**pins, "source_pin": "a" * 12})
        ledger["artifacts"][0]["bytes"] = True
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.validate_graph(source, harness, ledger, complete, **pins)
        for value in (None, "183b16fc", "G" * 40):
            with self.subTest(commit=value), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.require_commit(value)

    def test_same_graph_source_inventory_rejects_case_collisions(self):
        row = {"path": "native/app/AppServices.hpp", "size": 1, "sha256": "a" * 64}
        self.assertEqual(prepare_same_graph.source_records({"files": [row]}), [row])
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.source_records({"files": [row, {**row, "path": row["path"].lower()}]})
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.source_records({"files": [{**row, "size": True}]})

    def test_same_graph_inventory_binds_every_runtime_argument(self):
        _, _, ledger, _, _ = self.same_graph_contract()
        smoke = ledger["artifacts"][-1]
        argv = [smoke["physicalPath"], "--freeze", str(self.root / "freeze.json"), "--fixtures", str(self.root / "fixtures.json"),
                "--matrix", str(self.root / "matrix.json"), "--manifest-sha256", "a" * 64, "--output", str(self.root / "cases.json")]
        inventory = {"kind": "ctestInfo", "tests": [{"name": prepare_same_graph.TARGET, "command": argv}]}
        prepare_same_graph.validate_inventory(inventory, smoke, argv)
        changed = copy.deepcopy(inventory)
        changed["tests"][0]["command"][-1] = str(self.root / "other-cases.json")
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.validate_inventory(changed, smoke, argv)

    def same_graph_byte_inputs(self, name="same-graph", target_platform="windows"):
        base = self.root / name
        source_commit = "c" * 40
        raw_source = self.text(name + "/source/native/app/AppServices.hpp", "TOOL_ONLY_RAW_SOURCE\n")
        source = self.put(name + "/inputs/source.json", {"sourceCommit": source_commit, "files": [{
            "path": "native/app/AppServices.hpp", "size": raw_source.stat().st_size, "sha256": evidence.sha256(raw_source)}]})
        harness_root = base / "harness"
        harness_rows = []
        for filename in ("production_main.cpp", "suite_cases.hpp", "qt_probe.hpp", "same_graph_inputs.hpp",
                         "fixture_inputs.hpp", "generate_fixtures.py", "CMakeLists.txt", "matrix.json"):
            relative = "tests/native-production-smoke/" + filename
            if filename == "matrix.json":
                path = self.put(name + "/harness/" + relative, evidence.read_json(evidence.OWNER / relative))
            elif filename == "generate_fixtures.py":
                path = self.blob(name + "/harness/" + relative, (evidence.OWNER / relative).read_bytes())
            else:
                path = self.text(name + "/harness/" + relative, "TOOL_ONLY_NOT_COMPILED " + filename + "\n")
            harness_rows.append({"path": relative, "size": path.stat().st_size, "sha256": evidence.sha256(path)})
        harness = self.put(name + "/harness/snapshot.json", {
            "kind": "owned-production-smoke-source-delta", "owner": "production-smoke", "files": harness_rows})
        graph = base / "graph"
        linux = target_platform == "linux"
        artifacts = []
        for role, filename in (("production-library", "native/app/libapp.a" if linux else "native/app/app.lib"),
                               ("production-executable", "native/app/Pd" + ("" if linux else ".exe")),
                               ("smoke-executable", "tests/native-production-smoke/" + prepare_same_graph.TARGET + ("" if linux else ".exe"))):
            if linux:
                payload = self.ar_bytes() if role == "production-library" else self.elf_bytes(kind=3 if role == "smoke-executable" else 2)
                row = self.artifact_row(name + "/graph/" + filename, payload, role)
            else:
                path = self.text(name + "/graph/" + filename, "TOOL_ONLY_NOT_A_REAL_BINARY " + role + "\n")
                row = {"role": role, "physicalPath": str(path), "bytes": path.stat().st_size, "sha256": evidence.sha256(path)}
            artifacts.append(row)
        source_pin, harness_pin = evidence.sha256(source), evidence.sha256(harness)
        definitions = {"NICKEL_SMOKE_SOURCE_SHA256": source_pin, "NICKEL_SMOKE_SOURCE_COMMIT": source_commit,
                       "NICKEL_SMOKE_HARNESS_SHA256": harness_pin}
        prefix = "-D" if linux else "/D"
        command = ("c++ " if linux else "cl.exe ") + prefix + "NICKEL_SMOKE_SAME_GRAPH=1 "
        command += " ".join(prefix + key + '=\\"' + value + '\\"' for key, value in definitions.items())
        compile_path = self.put(name + "/inputs/compile.json", [{"directory": str(graph),
            "file": str(harness_root / "tests/native-production-smoke/production_main.cpp"), "command": command}])
        paths = [Path(row["physicalPath"]).relative_to(graph).as_posix() for row in artifacts]
        link = self.text(name + "/inputs/build.ninja", "# TOOL_ONLY_NOT_ACTUAL_BUILD\n"
            + "build " + paths[2] + ": CXX_EXECUTABLE_LINKER__" + prepare_same_graph.TARGET + " " + paths[0] + "\n"
            + "build " + paths[1] + ": CXX_EXECUTABLE_LINKER__ni-launcher " + paths[0] + "\n")
        identities = {"sourceCommit": source_commit, "sourceGenerationManifestSha256": source_pin, "harnessManifestSha256": harness_pin}
        platform_fields = {"targetPlatform": "linux"} if linux else {}
        ledger = self.put(name + "/inputs/artifacts.json", {
            "kind": "matched-same-graph-production-artifacts", **identities, **platform_fields, "artifacts": artifacts})
        complete = self.put(name + "/inputs/complete.json", {"publicationComplete": True, "receiver": "production-smoke", **identities,
            **platform_fields, "artifactManifestSha256": evidence.sha256(ledger), "compileCommandsSha256": evidence.sha256(compile_path),
            "linkRecipeSha256": evidence.sha256(link)})
        fixture_document = generate_fixtures.generate(base / "template", "0.1.0", source_commit=source_commit,
            source_sha256=source_pin, harness_sha256=harness_pin)
        fixtures = Path(fixture_document["root"]) / "fixture-manifest.json"
        output, case_output = base / "prepared.json", base / "cases.json"
        argv = [artifacts[2]["physicalPath"], "--freeze", str(output), "--fixtures", str(fixtures),
                "--matrix", str(harness_root / "tests/native-production-smoke/matrix.json"), "--manifest-sha256", source_pin, "--output", str(case_output)]
        inventory = self.put(name + "/inputs/ctest.json", {"kind": "ctestInfo", "toolOnly": True,
            "tests": [{"name": prepare_same_graph.TARGET, "command": argv}]})
        return SimpleNamespace(source_manifest=str(source), source_sha256=source_pin, source_commit=source_commit,
            harness_manifest=str(harness), harness_sha256=harness_pin, harness_root=str(harness_root),
            artifact_manifest=str(ledger), artifact_sha256=evidence.sha256(ledger),
            complete_marker=str(complete), complete_sha256=evidence.sha256(complete),
            compile_commands=str(compile_path), compile_sha256=evidence.sha256(compile_path),
            link_recipe=str(link), link_sha256=evidence.sha256(link), ctest_inventory=str(inventory),
            ctest_sha256=evidence.sha256(inventory), fixtures=str(fixtures), output=str(output), case_output=str(case_output),
            target_platform=target_platform)

    def test_same_graph_prepare_binds_tool_bytes_without_claiming_runtime(self):
        args = self.same_graph_byte_inputs()
        result = prepare_same_graph.prepare(args)
        self.assertEqual(result["kind"], "verified-same-graph-production-inputs")
        self.assertEqual(result["runtimeStatus"], "WrittenNotRun")
        self.assertIs(result["productionPass"], False)
        self.assertEqual(result["copiedLibraryBytes"], 0)
        self.assertEqual(result["sourceCommit"], args.source_commit)
        self.assertEqual(result["fixtureManifestSha256"], evidence.sha256(args.fixtures))
        self.assertEqual(len(result["artifactReferences"]), 3)
        self.assertEqual(evidence.read_json(args.output), result)
        self.assertFalse(Path(args.case_output).exists())

    def test_same_graph_prepare_rejects_harness_byte_drift(self):
        args = self.same_graph_byte_inputs()
        with (Path(args.harness_root) / "tests/native-production-smoke/qt_probe.hpp").open("ab") as stream:
            stream.write(b"\n")
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.prepare(args)
        self.assertFalse(Path(args.output).exists())

    def test_same_graph_prepare_rejects_artifact_byte_drift(self):
        args = self.same_graph_byte_inputs()
        artifact = evidence.read_json(args.artifact_manifest)["artifacts"][0]
        with Path(artifact["physicalPath"]).open("ab") as stream:
            stream.write(b"\n")
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.prepare(args)
        self.assertFalse(Path(args.output).exists())

    def test_same_graph_prepare_rejects_shared_output_namespace(self):
        args = self.same_graph_byte_inputs()
        args.case_output = args.output
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.prepare(args)
        self.assertFalse(Path(args.output).exists())

    def test_same_graph_compile_requires_unique_named_definitions(self):
        args = self.same_graph_byte_inputs()
        original = evidence.read_json(args.compile_commands)
        pins = {"source_pin": args.source_sha256, "source_commit": args.source_commit, "harness_pin": args.harness_sha256}
        source = Path(original[0]["file"])
        self.assertEqual(prepare_same_graph.validate_compile_commands(original, source, **pins), Path(original[0]["directory"]))
        for command in (original[0]["command"].replace("NICKEL_SMOKE_SOURCE_SHA256", "UNUSED_SOURCE_HASH"),
                        original[0]["command"] + ' /DNICKEL_SMOKE_SOURCE_COMMIT="' + "0" * 40 + '"',
                        original[0]["command"].replace(args.harness_sha256, args.harness_sha256 + "0")):
            with self.subTest(command=command), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_compile_commands([{**original[0], "command": command}], source, **pins)
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.validate_compile_commands(original + original, source, **pins)

    def test_same_graph_compile_accepts_argument_inventory(self):
        args = self.same_graph_byte_inputs()
        row = evidence.read_json(args.compile_commands)[0]
        row.pop("command")
        row["arguments"] = ["c++", "-DNICKEL_SMOKE_SAME_GRAPH=1", '-DNICKEL_SMOKE_SOURCE_SHA256="' + args.source_sha256 + '"',
            '-DNICKEL_SMOKE_SOURCE_COMMIT="' + args.source_commit + '"', '-DNICKEL_SMOKE_HARNESS_SHA256="' + args.harness_sha256 + '"']
        self.assertEqual(prepare_same_graph.validate_compile_commands([row], Path(row["file"]),
            source_pin=args.source_sha256, source_commit=args.source_commit, harness_pin=args.harness_sha256), Path(row["directory"]))

    def test_same_graph_link_rejects_same_named_other_library(self):
        args = self.same_graph_byte_inputs()
        artifacts = evidence.read_json(args.artifact_manifest)["artifacts"]
        graph = Path(evidence.read_json(args.compile_commands)[0]["directory"])
        original = Path(args.link_recipe).read_text(encoding="utf-8")
        wrong = self.text("other-link.ninja", original.replace("native/app/app.lib", "other-generation/app.lib"))
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.validate_link_recipe(wrong, artifacts, graph)

    def test_same_graph_inventory_keeps_flag_and_hash_case(self):
        args = self.same_graph_byte_inputs()
        inventory = evidence.read_json(args.ctest_inventory)
        smoke = evidence.read_json(args.artifact_manifest)["artifacts"][-1]
        argv = inventory["tests"][0]["command"]
        for index in (1, 3, 5, 7, 8, 9):
            altered = copy.deepcopy(inventory)
            altered["tests"][0]["command"][index] = argv[index].upper()
            with self.subTest(index=index), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_inventory(altered, smoke, argv)

    def test_same_graph_rechecks_inputs_after_artifact_verification(self):
        real_hash = prepare_same_graph.hash_artifact
        for platform in ("windows", "linux"):
            for field in ("source_manifest", "harness_manifest", "artifact_manifest", "complete_marker", "compile_commands",
                          "link_recipe", "ctest_inventory", "fixtures", "matrix", "compiled-harness"):
                args = self.same_graph_byte_inputs("mutation-" + platform + "-" + field, platform)
                changed = (Path(args.harness_root) / "tests/native-production-smoke/matrix.json" if field == "matrix"
                           else Path(args.harness_root) / "tests/native-production-smoke/qt_probe.hpp" if field == "compiled-harness"
                           else Path(getattr(args, field)))
                mutated = [False]
                def after_hash(row, **kwargs):
                    result = real_hash(row, **kwargs)
                    if not mutated[0]:
                        with changed.open("ab") as stream:
                            stream.write(b"\n")
                        mutated[0] = True
                    return result
                with (self.subTest(platform=platform, field=field),
                      mock.patch.object(prepare_same_graph, "hash_artifact", side_effect=after_hash),
                      self.assertRaises(evidence.EvidenceError)):
                    prepare_same_graph.prepare(args)
                self.assertFalse(Path(args.output).exists())

    def test_target_platform_defaults_and_rejects_unsupported_hosts(self):
        for host, expected in (("win32", "windows"), ("linux", "linux"), ("linux2", "linux")):
            with self.subTest(host=host), mock.patch.object(prepare_same_graph.sys, "platform", host):
                self.assertEqual(prepare_same_graph.artifact_platform(), expected)
        with mock.patch.object(prepare_same_graph.sys, "platform", "darwin"), self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.artifact_platform()
        for value in ("darwin", "Linux", "", False, []):
            with self.subTest(value=value), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.artifact_platform(value)

    def test_linux_elf_accepts_bounded_exec_and_pie_headers(self):
        for bits in (32, 64):
            for byteorder in ("little", "big"):
                for kind, expected in ((2, "elf-executable"), (3, "elf-pie-executable")):
                    row = self.artifact_row("elf-" + str(bits) + "-" + byteorder + "-" + str(kind) + ".native",
                                            self.elf_bytes(kind, bits, byteorder))
                    result = prepare_same_graph.hash_artifact(row, target_platform="linux")
                    self.assertEqual(result["binaryFormat"], expected)
                    self.assertEqual(result["targetPlatform"], "linux")
                    self.assertEqual(result["sha256"], row["sha256"])
                    self.assertEqual(result["size"], row["bytes"])
                    self.assertIs(result["handlesClosed"], True)
                    self.assertNotIn("productionPass", result)

    def test_linux_ar_accepts_bounded_gnu_bsd_and_odd_members(self):
        cases = [[(b"one.o/", b"even"), (b"two.o/", b"odd")],
                 [(b"/", b"index"), (b"//", b"long-object-name.o/\n"), (b"/0", b"object")],
                 [(b"#1/11", b"long-name.o" + b"object")]]
        for index, members in enumerate(cases):
            row = self.artifact_row("lib-" + str(index) + ".a", self.ar_bytes(members), "production-library")
            result = prepare_same_graph.hash_artifact(row, target_platform="linux")
            self.assertEqual(result["binaryFormat"], "ar-static-library")
            self.assertEqual(result["sha256"], row["sha256"])
            self.assertIs(result["handlesClosed"], True)

    def test_linux_elf_rejects_foreign_or_non_executable_formats(self):
        cases = [b"#!/bin/sh\nexit 0\n", b"MZ" + b"\x00" * 128, b"\xcf\xfa\xed\xfe" + b"\x00" * 128,
                 b"\x7fELF", self.elf_bytes(kind=1), self.elf_bytes(kind=4), self.elf_bytes()[:100]]
        for index, payload in enumerate(cases):
            row = self.artifact_row("foreign-" + str(index), payload)
            with self.subTest(index=index), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.hash_artifact(row, target_platform="linux")

    def test_linux_elf_rejects_header_and_segment_bound_drift(self):
        original = self.elf_bytes()
        fields = [("class", 4, b"\x00"), ("endian", 5, b"\x00"), ("ident-version", 6, b"\x02"),
                  ("machine", 18, b"\x00\x00"), ("version", 20, (2).to_bytes(4, "little")),
                  ("entry", 24, b"\x00" * 8), ("header-size", 52, (63).to_bytes(2, "little")),
                  ("program-offset", 32, len(original).to_bytes(8, "little")),
                  ("program-size", 54, (55).to_bytes(2, "little")), ("program-count", 56, b"\x00\x00"),
                  ("not-load", 64, (2).to_bytes(4, "little")), ("not-executable", 68, (4).to_bytes(4, "little")),
                  ("segment-offset", 72, len(original).to_bytes(8, "little")),
                  ("segment-size", 96, (len(original) + 1).to_bytes(8, "little")),
                  ("memory-size", 104, (1).to_bytes(8, "little"))]
        for name, offset, replacement in fields:
            changed = bytearray(original)
            changed[offset:offset + len(replacement)] = replacement
            row = self.artifact_row("bad-elf-" + name, bytes(changed))
            with self.subTest(field=name), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.hash_artifact(row, target_platform="linux")

    def test_linux_ar_rejects_thin_empty_truncated_and_malformed_members(self):
        original = self.ar_bytes([(b"object.o/", b"x")])
        bad_length = bytearray(original)
        bad_length[56:66] = b"9999999999"
        bad_trailer = bytearray(original)
        bad_trailer[66:68] = b"xx"
        cases = [b"!<thin>\n", b"!<arch>\n", b"not an archive", original[:-1], original[:-20],
                 original + b"junk", bytes(bad_length), bytes(bad_trailer),
                 self.ar_bytes([(b"#1/not-number", b"object")]), self.ar_bytes([(b"#1/99", b"object")]),
                 self.ar_bytes([(b"/", b"index")])]
        for index, payload in enumerate(cases):
            row = self.artifact_row("bad-archive-" + str(index) + ".a", payload, "production-library")
            with self.subTest(index=index), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.hash_artifact(row, target_platform="linux")

    def test_artifact_roles_and_extensions_are_platform_specific(self):
        for platform, role, names in (("linux", "production-library", ["app.lib", "app.so", "app.A"]),
                                     ("linux", "smoke-executable", ["smoke.exe", "smoke.dll", "smoke.a", "smoke.so"]),
                                     ("windows", "production-library", ["app.a", "app.dll"]),
                                     ("windows", "production-executable", ["Pd", "Pd.so"])):
            for name in names:
                with self.subTest(platform=platform, name=name), self.assertRaises(evidence.EvidenceError):
                    prepare_same_graph.artifact_path({"role": role, "physicalPath": str(self.root / name)}, platform)
        for row in ({"role": [], "physicalPath": str(self.root / "app.a")},
                    {"role": "smoke-executable", "physicalPath": "relative/Pd"},
                    {"role": "production-library", "physicalPath": None}):
            with self.subTest(row=row), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.artifact_path(row, "linux")
        self.assertEqual(prepare_same_graph.artifact_path({"role": "smoke-executable", "physicalPath": str(self.root / "Pd.EXE")},
                                                        "windows"), self.root / "Pd.EXE")

    def test_linux_graph_requires_platform_in_both_publications(self):
        args = self.same_graph_byte_inputs(target_platform="linux")
        source, harness = evidence.read_json(args.source_manifest), evidence.read_json(args.harness_manifest)
        ledger, complete = evidence.read_json(args.artifact_manifest), evidence.read_json(args.complete_marker)
        pins = {"source_pin": args.source_sha256, "source_commit": args.source_commit, "harness_pin": args.harness_sha256,
                "ledger_pin": args.artifact_sha256, "compile_pin": args.compile_sha256, "link_pin": args.link_sha256, "target_platform": "linux"}
        self.assertEqual(prepare_same_graph.validate_graph(source, harness, ledger, complete, **pins), ledger["artifacts"])
        for document_index in (0, 1):
            for value in (None, "windows", "Linux"):
                altered = [copy.deepcopy(ledger), copy.deepcopy(complete)]
                if value is None:
                    altered[document_index].pop("targetPlatform")
                else:
                    altered[document_index]["targetPlatform"] = value
                with self.subTest(document=document_index, value=value), self.assertRaises(evidence.EvidenceError):
                    prepare_same_graph.validate_graph(source, harness, *altered, **pins)

    def test_linux_prepare_binds_all_tool_bytes_without_running_product(self):
        args = self.same_graph_byte_inputs(target_platform="linux")
        result = prepare_same_graph.prepare(args)
        self.assertEqual(result["targetPlatform"], "linux")
        self.assertEqual(result["runtimeStatus"], "WrittenNotRun")
        self.assertIs(result["productionPass"], False)
        self.assertEqual(result["copiedLibraryBytes"], 0)
        self.assertEqual([row["binaryFormat"] for row in result["artifactReferences"]],
                         ["ar-static-library", "elf-executable", "elf-pie-executable"])
        self.assertEqual(result["actualRunArgv"], evidence.read_json(args.ctest_inventory)["tests"][0]["command"])
        self.assertEqual(evidence.read_json(args.output), result)
        self.assertFalse(Path(args.case_output).exists())

    def test_windows_prepare_keeps_legacy_platform_omission(self):
        args = self.same_graph_byte_inputs()
        del args.target_platform
        with mock.patch.object(prepare_same_graph.sys, "platform", "win32"):
            result = prepare_same_graph.prepare(args)
        self.assertEqual(result["targetPlatform"], "windows")
        self.assertEqual([row["binaryFormat"] for row in result["artifactReferences"]],
                         ["windows-static-library", "windows-executable", "windows-executable"])
        self.assertIs(result["productionPass"], False)

    def test_linux_prepare_rejects_same_size_artifact_tampering(self):
        for index in range(3):
            args = self.same_graph_byte_inputs("tamper-" + str(index), "linux")
            row = evidence.read_json(args.artifact_manifest)["artifacts"][index]
            with Path(row["physicalPath"]).open("r+b") as stream:
                stream.seek(-2, 2)
                stream.write(b"?")
            with self.subTest(role=row["role"]), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.prepare(args)
            self.assertFalse(Path(args.output).exists())

    def test_linux_graph_rejects_each_mixed_source_and_completion_scope(self):
        args = self.same_graph_byte_inputs(target_platform="linux")
        original = [evidence.read_json(getattr(args, field)) for field in
                    ("source_manifest", "harness_manifest", "artifact_manifest", "complete_marker")]
        pins = {"source_pin": args.source_sha256, "source_commit": args.source_commit, "harness_pin": args.harness_sha256,
                "ledger_pin": args.artifact_sha256, "compile_pin": args.compile_sha256, "link_pin": args.link_sha256, "target_platform": "linux"}
        for index, field in ((0, "sourceCommit"), (1, "owner"), (2, "sourceCommit"), (2, "sourceGenerationManifestSha256"),
                             (2, "harnessManifestSha256"), (3, "sourceCommit"), (3, "harnessManifestSha256"),
                             (3, "artifactManifestSha256"), (3, "compileCommandsSha256"), (3, "linkRecipeSha256")):
            changed = copy.deepcopy(original)
            changed[index][field] = "other-owner" if field == "owner" else "0" * len(changed[index][field])
            with self.subTest(field=field, index=index), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_graph(*changed, **pins)

    def test_linux_compile_rejects_overrides_and_wrong_source_identity(self):
        args = self.same_graph_byte_inputs(target_platform="linux")
        row = evidence.read_json(args.compile_commands)[0]
        pins = {"source_pin": args.source_sha256, "source_commit": args.source_commit,
                "harness_pin": args.harness_sha256, "target_platform": "linux"}
        source = Path(row["file"])
        self.assertEqual(prepare_same_graph.validate_compile_commands([row], source, **pins), Path(row["directory"]))
        for key in ("SAME_GRAPH", "SOURCE_SHA256", "SOURCE_COMMIT", "HARNESS_SHA256"):
            changed = {**row, "command": row["command"] + " -DNICKEL_SMOKE_" + key + "=0"}
            with self.subTest(key=key), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_compile_commands([changed], source, **pins)
        for file in (row["file"].upper(), str(self.root / "foreign/production_main.cpp")):
            with self.subTest(file=file), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_compile_commands([{**row, "file": file}], source, **pins)

    def test_linux_link_rejects_wrong_library_target_and_path_case(self):
        args = self.same_graph_byte_inputs(target_platform="linux")
        artifacts = evidence.read_json(args.artifact_manifest)["artifacts"]
        graph = Path(evidence.read_json(args.compile_commands)[0]["directory"])
        original = Path(args.link_recipe).read_text(encoding="utf-8")
        changes = [original.replace("native/app/libapp.a", "other-generation/libapp.a"),
                   original.replace("native/app/libapp.a", "libapp.a"),
                   original.replace("native/app/libapp.a", "native/app/LIBAPP.a"),
                   original.replace(prepare_same_graph.TARGET, "another_matrix")]
        for index, changed in enumerate(changes):
            link = self.text("linux-link-" + str(index) + ".ninja", changed)
            with self.subTest(index=index), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_link_recipe(link, artifacts, graph, target_platform="linux")

    def test_linux_inventory_rejects_every_argument_drift_and_malformed_argv(self):
        args = self.same_graph_byte_inputs(target_platform="linux")
        original = evidence.read_json(args.ctest_inventory)
        smoke = evidence.read_json(args.artifact_manifest)["artifacts"][-1]
        argv = original["tests"][0]["command"]
        for index in range(11):
            changed = copy.deepcopy(original)
            changed["tests"][0]["command"][index] = argv[index].upper()
            with self.subTest(index=index), self.assertRaises(evidence.EvidenceError):
                prepare_same_graph.validate_inventory(changed, smoke, argv, target_platform="linux")
        changed = copy.deepcopy(original)
        changed["tests"][0]["command"][0] = 1
        with self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.validate_inventory(changed, smoke, argv, target_platform="linux")

    def test_both_platforms_reject_reused_and_colliding_output_destinations(self):
        for platform in ("windows", "linux"):
            for collision in ("prepared", "cases", "same"):
                args = self.same_graph_byte_inputs("reuse-" + platform + "-" + collision, platform)
                if collision == "same":
                    args.case_output = args.output
                else:
                    existing = args.output if collision == "prepared" else args.case_output
                    evidence.write_json_new(existing, {"unchanged": True})
                with self.subTest(platform=platform, collision=collision), self.assertRaises(evidence.EvidenceError):
                    prepare_same_graph.prepare(args)
                if collision != "same":
                    self.assertEqual(evidence.read_json(existing), {"unchanged": True})

    def test_artifact_hash_rejects_handle_identity_change(self):
        row = self.artifact_row("identity-bound", self.elf_bytes())
        info = Path(row["physicalPath"]).stat()
        changed = SimpleNamespace(**{field: getattr(info, field) for field in
                                    ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_mode")})
        changed.st_ino += 1
        with mock.patch.object(prepare_same_graph.os, "fstat", return_value=changed), self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.hash_artifact(row, target_platform="linux")

    def test_artifact_hash_accepts_windows_cross_interface_mode_and_ctime(self):
        row = self.artifact_row("windows-stat.exe", b"TOOL_ONLY_WINDOWS_BYTES\n")
        fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_mode")
        info = Path(row["physicalPath"]).stat()
        handle = SimpleNamespace(**{field: getattr(info, field) for field in fields})
        handle.st_ctime_ns += 1
        handle.st_mode &= ~0o111
        with mock.patch.object(prepare_same_graph.os, "fstat", return_value=handle):
            result = prepare_same_graph.hash_artifact(row, target_platform="windows")
        self.assertEqual(result["sha256"], row["sha256"])
        self.assertIs(result["handlesClosed"], True)

    def test_artifact_hash_rejects_same_interface_handle_metadata_drift(self):
        row = self.artifact_row("metadata-bound", self.elf_bytes())
        with Path(row["physicalPath"]).open("rb") as stream:
            info = prepare_same_graph.os.fstat(stream.fileno())
        fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_mode")
        for field in ("st_ctime_ns", "st_mode"):
            changed = SimpleNamespace(**{name: getattr(info, name) for name in fields})
            setattr(changed, field, getattr(changed, field) ^ 1)
            with (self.subTest(field=field),
                  mock.patch.object(prepare_same_graph.os, "fstat", side_effect=[info, changed]),
                  self.assertRaises(evidence.EvidenceError)):
                prepare_same_graph.hash_artifact(row, target_platform="linux")


    def test_linux_mode_guard_rejects_a_non_executable_publication(self):
        row = self.artifact_row("non-executable", self.elf_bytes())
        Path(row["physicalPath"]).chmod(0o644)
        with mock.patch.object(prepare_same_graph.sys, "platform", "linux"), self.assertRaises(evidence.EvidenceError):
            prepare_same_graph.hash_artifact(row, target_platform="linux")





    def test_fixture_output_is_not_overwritten(self):
        generate_fixtures.generate(self.root / "fixture", "0.1.0", deep=False)
        with self.assertRaises(evidence.EvidenceError):
            generate_fixtures.generate(self.root / "fixture", "0.1.0", deep=False)

    def test_fixture_skin_is_bounded_real_png(self):
        data = generate_fixtures.skin_png()
        self.assertTrue(data.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertLess(len(data), 65536)


if __name__ == "__main__":
    print("TOOL SELF-TESTS ONLY: no production AppServices fixture has run", flush=True)
    unittest.main(verbosity=2)
